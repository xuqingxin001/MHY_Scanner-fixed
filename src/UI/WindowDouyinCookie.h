#pragma once

#include <string>
#include <string_view>

#include <Windows.h>

#include <QWidget>
#include <QPushButton>
#include <WebView2.h>
#include <wil/com.h>
#include <wrl.h>

// 抖音扫码登录窗口: 内置网页打开抖音, 手机扫码登录后自动保存 Cookie 到 douyin_cookie.txt
class WindowDouyinCookie : public QWidget
{
    Q_OBJECT
public:
    WindowDouyinCookie(QWidget* parent = nullptr);
    ~WindowDouyinCookie();

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void saveCookie();

    wil::com_ptr<ICoreWebView2Controller> webViewController{};
    wil::com_ptr<ICoreWebView2> webView{};
    wil::com_ptr<ICoreWebView2Settings> settings;
    EventRegistrationToken webResourceRequestedToken{};
    QPushButton* btnSave{};
    std::string m_cookieStr{};
};
