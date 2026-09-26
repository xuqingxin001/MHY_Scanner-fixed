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
    if (!webView)
    {
        QMessageBox::warning(this, QString::fromUtf8("提示"), QString::fromUtf8("网页还没加载完成，请稍等片刻再点。"));
        return;
    }

    wil::com_ptr<ICoreWebView2CookieManager> cookieManager;
    if (FAILED(webView->get_CookieManager(&cookieManager)))
    {
        QMessageBox::warning(this, QString::fromUtf8("提示"), QString::fromUtf8("获取Cookie管理器失败。"));
        return;
    }

    cookieManager->GetCookiesAsync(
        Microsoft::WRL::Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [this](HRESULT error, ICoreWebView2CookieList* cookieList) {
                if (!!error || !cookieList)
                {
                    QMetaObject::invokeMethod(this, [this]() {
                        QMessageBox::warning(this, QString::fromUtf8("提示"), QString::fromUtf8("读取Cookie失败，请重试。"));
                    });
                    return error;
                }

                std::string cookieStr;
                unsigned int count = 0;
                cookieList->get_Count(&count);
                for (unsigned int i = 0; i < count; ++i)
                {
                    wil::com_ptr<ICoreWebView2Cookie> cookie;
                    cookieList->GetValueAtIndex(i, &cookie);
                    LPWSTR name = nullptr;
                    LPWSTR value = nullptr;
                    cookie->get_Name(&name);
                    cookie->get_Value(&value);
                    if (name && value)
                    {
                        cookieStr += WideToUtf8(name);
                        cookieStr += '=';
                        cookieStr += WideToUtf8(value);
                        cookieStr += "; ";
                    }
                    if (name)
                    {
                        CoTaskMemFree(name);
                    }
                    if (value)
                    {
                        CoTaskMemFree(value);
                    }
                }

                // 写入 exe 所在目录的 douyin_cookie.txt
                std::ofstream ofs("douyin_cookie.txt", std::ios::trunc);
                if (ofs)
                {
                    ofs << cookieStr;
                    ofs.close();
                    QMetaObject::invokeMethod(this, [this]() {
                        QMessageBox::information(this, QString::fromUtf8("成功"), QString::fromUtf8("抖音Cookie已保存到 douyin_cookie.txt！\n现在可以监视抖音直播间了。"));
                    });
                }
                else
                {
                    QMetaObject::invokeMethod(this, [this]() {
                        QMessageBox::warning(this, QString::fromUtf8("提示"), QString::fromUtf8("写入 douyin_cookie.txt 失败，请检查软件目录是否有写权限。"));
                    });
                }
                return S_OK;
            }).Get());
}
