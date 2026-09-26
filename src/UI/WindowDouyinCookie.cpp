#include "WindowDouyinCookie.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <QMessageBox>
#include <QMetaObject>
#include <QVBoxLayout>
#include <QLabel>

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
    setWindowFlags(Qt::Window);
    setFixedSize(QSize(560, 760));
    setWindowTitle(QString::fromUtf8("抖音扫码登录 - 自动保存Cookie"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* tip = new QLabel(QString::fromUtf8("用手机抖音App扫码登录网页版抖音，登录完成后点击下方按钮，软件会自动保存Cookie。"), this);
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
                                                              }
                                                              return S_OK;
                                                          })
                                                          .Get(),
                                                      &webResourceRequestedToken);

                                                  webView->Navigate(L"https://www.douyin.com/");

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
        QMessageBox::information(this, QString::fromUtf8("成功"), QString::fromUtf8("抖音Cookie已保存到 douyin_cookie.txt！\n现在可以监视抖音直播间了。"));
    }
    else
    {
        QMessageBox::warning(this, QString::fromUtf8("提示"), QString::fromUtf8("写入 douyin_cookie.txt 失败，请检查软件目录是否有写权限。"));
    }
}
