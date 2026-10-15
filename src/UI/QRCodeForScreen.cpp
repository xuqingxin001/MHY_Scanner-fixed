#include "QRCodeForScreen.h"

#include <chrono>
#include <thread>
#include <algorithm>
#include <vector>
#include <memory>
#include <cstdlib>
#include <Windows.h>
#include <opencv2/imgcodecs.hpp>

#include "Logger.hpp"

#include <QFuture>
#include <QtConcurrent/QtConcurrent>
#include <QThreadPool>

#include "QRScanner.h"
#include "ScreenScan.h"
#include "ScreenShotDXGI.hpp"

#define DELAYED 200

// GDI 屏幕捕获(BitBlt): DXGI桌面复制在UU超级屏等虚拟显示器上返回损坏的条纹帧,
// GDI方式在虚拟屏下能抓到真实画面, 用于替代DXGI
static bool CaptureScreenGDI(uint8_t* buffer, int width, int height)
{
    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen)
    {
        return false;
    }
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    if (!hBitmap)
    {
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }
    HGDIOBJ hOld = SelectObject(hdcMem, hBitmap);
    if (!BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY | CAPTUREBLT))
    {
        SelectObject(hdcMem, hOld);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // 自顶向下, BGRA
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(hdcMem, hBitmap, 0, static_cast<UINT>(height), buffer, &bmi, DIB_RGB_COLORS);
    SelectObject(hdcMem, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return true;
}

QRCodeForScreen::QRCodeForScreen(QObject* parent) :
    QThread(parent),
    m_stop(false)
{
    m_config = &ConfigDate::getInstance();
}

QRCodeForScreen::~QRCodeForScreen()
{
    if (!this->isInterruptionRequested())
    {
        m_stop.store(false);
    }
    this->requestInterruption();
    this->wait();
}

void QRCodeForScreen::setLoginInfo(const std::string& uid, const std::string& token)
{
    this->uid = uid;
    this->gameToken = token;
}

void QRCodeForScreen::setLoginInfo(const std::string& uid, const std::string& token, const std::string& name)
{
    this->uid = uid;
    this->gameToken = token;
    this->m_name = name;
}

void QRCodeForScreen::setLoginInfo1(const std::string& uid, const std::string& stoken, const std::string& mid)
{
    this->uid = uid;
    this->gameToken = stoken;
    this->mid = mid;
}

void QRCodeForScreen::LoginOfficial()
{
    QThreadPool threadPool;
    threadPool.setMaxThreadCount(threadNumber);
    std::mutex mtx;

    // ===== GDI 屏幕捕获(替代DXGI, 兼容UU超级屏等虚拟显示器: DXGI桌面复制在虚拟屏返回损坏帧) =====
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    long mBufferSize = w * h * 4;
    uint8_t* mBuffer = new UCHAR[mBufferSize];
    // 等比缩放: 保持宽高比, 避免修改分辨率后二维码被拉伸变形无法识别
    double scale = std::min(1280.0 / w, 720.0 / h);
    DebugLog::Log("[QRCodeForScreen] GDI捕获初始化: " + std::to_string(w) + "x" + std::to_string(h));
    int diagTick = 0;

    while (m_stop.load())
    {
        // 分辨率自适应: 主屏尺寸变化时重新分配缓冲
        const int sw = GetSystemMetrics(SM_CXSCREEN);
        const int sh = GetSystemMetrics(SM_CYSCREEN);
        if (sw != w || sh != h)
        {
            w = sw;
            h = sh;
            mBufferSize = w * h * 4;
            delete[] mBuffer;
            mBuffer = new UCHAR[mBufferSize];
            scale = std::min(1280.0 / w, 720.0 / h);
            DebugLog::Log("[QRCodeForScreen] 分辨率变化, 已自适应: " + std::to_string(w) + "x" + std::to_string(h));
        }
        if (!CaptureScreenGDI(mBuffer, w, h))
        {
            continue; // GDI抓屏失败, 跳过
        }
        cv::Mat img;
        cv::resize(cv::Mat(h, w, CV_8UC4, mBuffer), img,
                   cv::Size(static_cast<int>(w * scale), static_cast<int>(h * scale)));
#ifndef SHOW
        cv::imshow("Video_Stream", img);
        cv::waitKey(1);
#endif
        // ===== 诊断(临时): 输出内容检测, 判断是否黑屏/抓到真实画面 =====
        if (++diagTick % 30 == 0)
        {
            const cv::Scalar meanVal = cv::mean(img);
            DebugLog::Log("[QRCodeForScreen] 灰度均值: " + std::to_string(static_cast<int>(meanVal[0])));
            if (diagTick == 30)
            {
                char tempPath[MAX_PATH];
                const DWORD tempLen = GetEnvironmentVariableA("TEMP", tempPath, MAX_PATH);
                const std::string shotPath = std::string(tempLen > 0 ? tempPath : ".") + "\\mhy_screen_shot.png";
                cv::imwrite(shotPath, img);
                DebugLog::Log("[QRCodeForScreen] 已保存屏幕截图: " + shotPath);
            }
        }
        threadPool.tryStart([&, img = std::move(img)]() {
            thread_local QRScanner qrScanners;
            std::string str;
            qrScanners.decodeSingle(img, str);
            std::string ticket;
            if (!parseOfficialQRCode(str, ticket))
            {
                return;
            }
            if (lastTicket == ticket)
            {
                return;
            }
            if (mtx.try_lock())
            {
                if (!m_stop.load())
                {
                    mtx.unlock();
                    return;
                }
                const std::string passportQrUrl = PandaScanQRCode(scanUrl, ticket, gameType);
                if (!passportQrUrl.empty())
                {
                    lastTicket = ticket;
                    lastQrCode = passportQrUrl;
                    nlohmann::json config = nlohmann::json::parse(m_config->getConfig());
                    if (config["auto_login"])
                    {
                        continueLastLogin();
                    }
                    else
                    {
                        emit loginConfirm(gameType, true);
                    }
                }
                else
                {
                    emit loginResults(ScanRet::FAILURE_1);
                }
                stop();
                mtx.unlock();
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(DELAYED));
    }
    delete[] mBuffer;
}

void QRCodeForScreen::LoginBH3BiliBili()
{
    QThreadPool threadPool;
    threadPool.setMaxThreadCount(threadNumber);
    std::mutex mtx;

    // ===== GDI 屏幕捕获(替代DXGI, 兼容UU超级屏等虚拟显示器: DXGI桌面复制在虚拟屏返回损坏帧) =====
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    long mBufferSize = w * h * 4;
    uint8_t* mBuffer = new UCHAR[mBufferSize];
    // 等比缩放: 保持宽高比, 避免修改分辨率后二维码被拉伸变形无法识别
    double scale = std::min(1280.0 / w, 720.0 / h);
    DebugLog::Log("[QRCodeForScreen] GDI捕获初始化: " + std::to_string(w) + "x" + std::to_string(h));
    int diagTick = 0;

    while (m_stop.load())
    {
        // 分辨率自适应: 主屏尺寸变化时重新分配缓冲
        const int sw = GetSystemMetrics(SM_CXSCREEN);
        const int sh = GetSystemMetrics(SM_CYSCREEN);
        if (sw != w || sh != h)
        {
            w = sw;
            h = sh;
            mBufferSize = w * h * 4;
            delete[] mBuffer;
            mBuffer = new UCHAR[mBufferSize];
            scale = std::min(1280.0 / w, 720.0 / h);
            DebugLog::Log("[QRCodeForScreen] 分辨率变化, 已自适应: " + std::to_string(w) + "x" + std::to_string(h));
        }
        if (!CaptureScreenGDI(mBuffer, w, h))
        {
            continue; // GDI抓屏失败, 跳过
        }
        cv::Mat img;
        cv::resize(cv::Mat(h, w, CV_8UC4, mBuffer), img,
                   cv::Size(static_cast<int>(w * scale), static_cast<int>(h * scale)));
#ifndef SHOW
            cv::imshow("Video_Stream", img);
            cv::waitKey(1);
#endif
        // ===== 诊断(临时): 输出内容检测, 判断是否黑屏/抓到真实画面 =====
        if (++diagTick % 30 == 0)
        {
            const cv::Scalar meanVal = cv::mean(img);
            DebugLog::Log("[QRCodeForScreen] 灰度均值: " + std::to_string(static_cast<int>(meanVal[0])));
                if (diagTick == 30)
                {
                    char tempPath[MAX_PATH];
                    const DWORD tempLen = GetEnvironmentVariableA("TEMP", tempPath, MAX_PATH);
                    const std::string shotPath = std::string(tempLen > 0 ? tempPath : ".") + "\\mhy_screen_shot.png";
                    cv::imwrite(shotPath, img);
                    DebugLog::Log("[QRCodeForScreen] 已保存屏幕截图: " + shotPath);
                }
            }
            threadPool.tryStart([&, img = std::move(img)]() {
                thread_local QRScanner qrScanners;
                std::string str;
                qrScanners.decodeSingle(img, str);
                std::string ticket;
                if (!parseOfficialQRCode(str, ticket) || gameType != GameType::Honkai3)
                {
                    return;
                }
                if (lastTicket == ticket)
                {
                    return;
                }
                if (mtx.try_lock())
                {
                    if (!m_stop.load())
                    {
                        mtx.unlock();
                        return;
                    }
                    if (ret = scanCheck(ticket); ret == ScanRet::SUCCESS)
                    {
                        lastTicket = ticket;
                        nlohmann::json config = nlohmann::json::parse(m_config->getConfig());
                        if (config["auto_login"])
                        {
                            continueLastLogin();
                        }
                        else
                        {
                            emit loginConfirm(GameType::Honkai3_BiliBili, true);
                        }
                    }
                    else
                    {
                        emit loginResults(ScanRet::FAILURE_1);
                    }
                    stop();
                    mtx.unlock();
                }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(DELAYED));
    }
    delete[] mBuffer;
}

void QRCodeForScreen::continueLastLogin()
{
    switch (servertype)
    {
        using enum ServerType;
    case Official:
    {
        bool b = ScanPassportQRLogin(lastQrCode, gameToken, mid) &&
                 ConfirmPassportQRLogin(lastQrCode, gameToken, mid);
        if (b)
        {
            Q_EMIT loginResults(ScanRet::SUCCESS);
        }
        else
        {
            Q_EMIT loginResults(ScanRet::FAILURE_2);
        }
    }
    break;
    case BH3_BiliBili:
    {
        ret = scanConfirm(lastTicket, uid, gameToken, m_name);
        Q_EMIT loginResults(ret);
    }
    break;
    default:
        break;
    }
}

void QRCodeForScreen::run()
{
    ret = ScanRet::UNKNOW;
    m_stop.store(true);
#ifndef SHOW
    cv::namedWindow("Video_Stream", cv::WINDOW_AUTOSIZE);
#endif
    switch (servertype)
    {
    case ServerType::Official:
        LoginOfficial();
        break;
    case ServerType::BH3_BiliBili:
        LoginBH3BiliBili();
        break;
    default:
        break;
    }
#ifndef SHOW
    cv::destroyWindow("Video_Stream");
#endif
}

void QRCodeForScreen::stop()
{
    m_stop.store(false);
}

void QRCodeForScreen::setServerType(const ServerType servertype)
{
    this->servertype = servertype;
}