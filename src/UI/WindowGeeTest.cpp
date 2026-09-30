#include "WindowGeeTest.h"

#include <algorithm>
#include <format>
#include <iostream>

#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>

WindowGeeTest::WindowGeeTest(QWidget* parent) :
    QWidget(parent)
{
    setWindowFlags(Qt::Window);
    setFixedSize(QSize(560, 860));
    setWindowTitle("请完成验证");
}

WindowGeeTest::~WindowGeeTest()
{
    if (webViewController)
    {
        webViewController->Close();
    }
}

void WindowGeeTest::closeEvent(QCloseEvent* event)
{
    if (webViewController)
    {
        webViewController->Close();
    }
}

void WindowGeeTest::Init(const std::wstring_view gt, const std::wstring_view challenge)
{
    constexpr std::wstring_view TemplateHtml{ LR"(
            <html>
                <head>
                    <title>GeeTest</title>
                    <style>
                        html, body {{
                            margin:0; padding:0; overflow:auto;
                        }}
                        #geetest-div {{
                            display:flex; align-items:flex-start; justify-content:center;
                            min-height:100%;
                            padding:16px;
                            box-sizing:border-box;
                        }}
                    </style>
                </head>
                <body>
                    <div id="geetest-div"></div>
                </body>
                <script src="https://static.geetest.com/static/js/gt.0.5.2.js"></script>
                <script>
                    initGeetest(
                        {{
                            protocol: "https://",
                            gt: "{}",
                            challenge: "{}",
                            new_captcha: true,
                            product: 'bind',
                            api_server: 'api.geetest.com'
                        }},
                        function (captchaObj) {{
                            captchaObj.onReady(function () {{
                                captchaObj.verify();
                                // 等验证码渲染完成后，把实际内容尺寸回传给窗口自动适配
                                function fitWindow() {{
                                    var h = Math.max(document.body.scrollHeight, document.documentElement.scrollHeight);
                                    var w = Math.max(document.body.scrollWidth, document.documentElement.scrollWidth);
                                    chrome.webview.postMessage(JSON.stringify({{type:'fit', width: w, height: h}}));
                                }}
                                setTimeout(fitWindow, 500);
                                setTimeout(fitWindow, 1500);
                                setTimeout(fitWindow, 3000);
                            }});
                            captchaObj.onSuccess(function () {{
                                var result = captchaObj.getValidate();
                                chrome.webview.postMessage(result);
                            }});
                        }}
                    );
              </script>
            </html>)" };
    indexHtml = std::format(TemplateHtml, gt, challenge);
}

void WindowGeeTest::showEvent(QShowEvent* event)
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
                                                  webViewController->put_Bounds({ 0, 0, width(), height() });

                                                  webView->get_Settings(&settings);
                                                  settings->put_IsStatusBarEnabled(false);

                                                  webView->NavigateToString(indexHtml.c_str());
                                                  //webView->Navigate(L"https://www.bing.com/");

                                                  webView->add_NewWindowRequested(
                                                      Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                                          [this](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) {
                                                              args->put_Handled(TRUE);
                                                              return S_OK;
                                                          })
                                                          .Get(),
                                                      &webResourceRequestedToken);

                                                  webView->add_WebMessageReceived(
                                                      Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                                          [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) {
                                                              LPWSTR message;
                                                              args->get_WebMessageAsJson(&message);
                                                              const QString msg{ QString::fromWCharArray(message) };
                                                              CoTaskMemFree(message);
                                                              // 验证码尺寸上报：自适应窗口
                                                              const QJsonDocument doc{ QJsonDocument::fromJson(msg.toUtf8()) };
                                                              if (!doc.isNull() && doc.isObject())
                                                              {
                                                                  const QJsonObject obj{ doc.object() };
                                                                  if (obj.value("type").toString() == QLatin1String("fit"))
                                                                  {
                                                                      FitWindow(obj.value("width").toInt(), obj.value("height").toInt());
                                                                      return S_OK;
                                                                  }
                                                              }
                                                              emit postMessage(msg);
                                                              return S_OK;
                                                          })
                                                          .Get(),
                                                      &webResourceRequestedToken);
                                                  return S_OK;
                                              }).Get());
            return S_OK;
        }).Get());
}

void WindowGeeTest::FitWindow(int width, int height)
{
    // 加一点边距，避免内容贴边
    constexpr int Margin{ 8 };
    const QRect screenRect{ QGuiApplication::primaryScreen()->availableGeometry() };
    const int maxW{ screenRect.width() };
    const int maxH{ screenRect.height() };

    const int newW{ std::clamp(width + Margin, 460, maxW) };
    const int newH{ std::clamp(height + Margin, 600, maxH) };
    if (newW != this->width() || newH != this->height())
    {
        setFixedSize(newW, newH);
        if (webViewController)
        {
            webViewController->put_Bounds({ 0, 0, newW, newH });
        }
    }
}
