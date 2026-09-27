#include "WindowDouyinCookie.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <QMessageBox>
#include <QMetaObject>
#include <QVBoxLayout>
#include <QLabel>
#include <QTimer>

#include <wrl.h>

namespace
{
    // Wide 转 UTF-8
    std::string WideToUtf8(const std::wstring_view wide)
    {
        if (wide.empty())
        {
            return {};
        }
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size, nullptr, nullptr);
        return utf8;
    }
}

WindowDouyinCookie::WindowDouyinCookie(QWidget* parent) :
    QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* tip = new QLabel(QString::fromUtf8("用手机抖音App扫下方二维码登录\n扫码登录成功后软件会自动保存Cookie，无需其他操作。"), this);
    tip->setWordWrap(true);
    tip->setAlignment(Qt::AlignCenter);
    tip->setStyleSheet(QString::fromUtf8("background:#202124;color:#e8eaed;font-size:13px;padding:10px;"));
    tip->setFixedHeight(52);
    layout->addWidget(tip);

    btnSave = new QPushButton(QString::fromUtf8("登录完成，保存Cookie"), this);
    btnSave->setFixedHeight(46);
    btnSave->setStyleSheet(QString::fromUtf8("font-size:14px;font-weight:bold;background:#fe2c55;color:white;border:none;"));
    layout->addWidget(btnSave);
    connect(btnSave, &QPushButton::clicked, this, &WindowDouyinCookie::saveCookie);
}

WindowDouyinCookie::~WindowDouyinCookie()
{
    if (webViewController)
    {
        webViewController->Close();
    }
}

void WindowDouyinCookie::closeEvent(QCloseEvent* event)
{
    if (webViewController)
    {
        webViewController->Close();
    }
}

void WindowDouyinCookie::showEvent(QShowEvent* event)
{
    m_autoSaved = false;
    m_cookieStr.clear();
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this](HRESULT error, ICoreWebView2Environment* env) {
            if (!!error)
            {
                return error;
            }

            env->CreateCoreWebView2Controller(reinterpret_cast<HWND>(winId()),
                                              Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([this](HRESULT error, ICoreWebView2Controller* controller) {
                                                  if (!!error)
                                                  {
                                                      return error;
                                                  }
                                                  webViewController = controller;
                                                  webViewController->get_CoreWebView2(&webView);
                                                  // 顶部提示52 + 按钮46 = 98px 留给网页
                                                  webViewController->put_Bounds({ 0, 52, width(), height() - 98 });

                                                  webView->get_Settings(&settings);
                                                  settings->put_IsStatusBarEnabled(false);
                                                  settings->put_AreDefaultScriptDialogsEnabled(true);

                                                  // 拦截抖音请求, 从请求头读取 Cookie(HttpOnly 也能拿到)
                                                  webView->AddWebResourceRequestedFilter(
                                                      L"https://www.douyin.com/*",
                                                      COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                                                  webView->add_WebResourceRequested(
                                                      Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                                          [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) {
                                                              wil::com_ptr<ICoreWebView2WebResourceRequest> request;
                                                              args->get_Request(&request);
                                                              wil::com_ptr<ICoreWebView2HttpRequestHeaders> headers;
                                                              request->get_Headers(&headers);
                                                              LPWSTR cookieHeader = nullptr;
                                                              if (SUCCEEDED(headers->GetHeader(L"Cookie", &cookieHeader)) && cookieHeader)
                                                              {
                                                                  m_cookieStr = WideToUtf8(cookieHeader);
                                                                  CoTaskMemFree(cookieHeader);

                                                                  // 检测登录态: passport_auth_status=1 是抖音登录成功的明确标志
                                                                  if (!m_autoSaved && m_cookieStr.find("passport_auth_status=1") != std::string::npos)
                                                                  {
                                                                      m_autoSaved = true;
                                                                      // 等2秒让cookie写完整, 再自动保存
                                                                      QTimer::singleShot(2000, this, [this]() {
                                                                          saveCookie();
                                                                      });
                                                                  }
                                                              }
                                                              return S_OK;
                                                          })
                                                          .Get(),
                                                      &webResourceRequestedToken);

                                                  // 直接打开抖音登录页(含扫码登录二维码)
                                                  webView->Navigate(L"https://www.douyin.com/passport/?type=login");

                                                  webView->add_NewWindowRequested(
                                                      Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                                          [this](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) {
                                                              args->put_Handled(TRUE);
                                                              return S_OK;
                                                          })
                                                          .Get(),
                                                      &webResourceRequestedToken);
                                                  return S_OK;
                                              }).Get());
            return S_OK;
        }).Get());
}

void WindowDouyinCookie::saveCookie()
{
    if (m_cookieStr.empty())
    {
        QMessageBox::warning(this, QString::fromUtf8("提示"), QString::fromUtf8("还没捕获到抖音Cookie，请先完成扫码登录，页面出现「扫一扫登录成功」后再点。"));
        return;
    }

    // 写入 exe 所在目录的 douyin_cookie.txt
    std::ofstream ofs("douyin_cookie.txt", std::ios::trunc);
    if (ofs)
    {
        ofs << m_cookieStr;
        ofs.close();
        QMessageBox::information(this, QString::fromUtf8("登录成功"), QString::fromUtf8("抖音Cookie已自动保存到 douyin_cookie.txt！\n现在可以监视抖音直播间了。"));
        close();
    }
    else
    {
        QMessageBox::warning(this, QString::fromUtf8("提示"), QString::fromUtf8("写入 douyin_cookie.txt 失败，请检查软件目录是否有写权限。"));
    }
}
