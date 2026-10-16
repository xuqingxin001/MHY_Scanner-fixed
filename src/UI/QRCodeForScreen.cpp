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

// 枚举所有显示器(主屏+副屏)的回调, 收集虚拟桌面坐标矩形
static BOOL CALLBACK MonitorEnumProc(HMONITOR /*hMonitor*/, HDC /*hdcMonitor*/, LPRECT lprcMonitor, LPARAM dwData)
{
    auto* rects = reinterpret_cast<std::vector<RECT>*>(dwData);
    rects->push_back(*lprcMonitor);
    return TRUE;
}

// 枚举所有显示器(主屏+副屏+虚拟屏)的屏幕矩形(虚拟桌面坐标)
static std::vector<RECT> GetMonitorRects()
{
    std::vector<RECT> rects;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&rects));
    return rects;
}

// GDI 捕获指定屏幕区域(虚拟桌面坐标 x,y 起, 宽 width 高 height)
static bool CaptureScreenGDIRegion(uint8_t* buffer, int x, int y, int width, int height)
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
    if (!BitBlt(hdcMem, 0, 0, width, height, hdcScreen, x, y, SRCCOPY | CAPTUREBLT))
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

    // ===== GDI 多屏捕获(替代DXGI, 兼容UU超级屏等虚拟显示器: DXGI桌面复制在虚拟屏返回损坏帧) =====
    // 枚举所有显示器(主屏+副屏+虚拟屏), 每个屏幕独立缓冲与等比缩放比例
    std::vector<RECT> mons = GetMonitorRects();
    std::vector<int> mX, mY, mW, mH;
    std::vector<uint8_t*> mBuffer;
    std::vector<double> mScale;
    for (const auto& r : mons)
    {
        mX.push_back(r.left);
        mY.push_back(r.top);
        mW.push_back(r.right - r.left);
        mH.push_back(r.bottom - r.top);
        mBuffer.push_back(new UCHAR[(r.right - r.left) * (r.bottom - r.top) * 4]);
        mScale.push_back(std::min(1280.0 / (r.right - r.left), 720.0 / (r.bottom - r.top)));
    }

    while (m_stop.load())
    {
        // 屏幕数量/尺寸变化检测: 变化时重新枚举并重分配缓冲(分辨率自适应)
        std::vector<RECT> newMons = GetMonitorRects();
        bool changed = newMons.size() != mons.size();
        if (!changed)
        {
            for (size_t i = 0; i < newMons.size(); ++i)
            {
                if (newMons[i].left != mons[i].left || newMons[i].top != mons[i].top ||
                    newMons[i].right != mons[i].right || newMons[i].bottom != mons[i].bottom)
                {
                    changed = true;
                    break;
                }
            }
        }
        if (changed)
        {
            mons = newMons;
            for (auto* b : mBuffer)
            {
                delete[] b;
            }
            mBuffer.clear();
            mX.clear();
            mY.clear();
            mW.clear();
            mH.clear();
            mScale.clear();
            for (const auto& r : mons)
            {
                mX.push_back(r.left);
                mY.push_back(r.top);
                mW.push_back(r.right - r.left);
                mH.push_back(r.bottom - r.top);
                mBuffer.push_back(new UCHAR[(r.right - r.left) * (r.bottom - r.top) * 4]);
                mScale.push_back(std::min(1280.0 / (r.right - r.left), 720.0 / (r.bottom - r.top)));
            }
        }
        if (mons.empty())
        {
            continue;
        }
        for (size_t i = 0; i < mons.size(); ++i)
        {
            if (!CaptureScreenGDIRegion(mBuffer[i], mX[i], mY[i], mW[i], mH[i]))
            {
                continue; // GDI抓屏失败, 跳过
            }
            cv::Mat img;
            cv::resize(cv::Mat(mH[i], mW[i], CV_8UC4, mBuffer[i]), img,
                       cv::Size(static_cast<int>(mW[i] * mScale[i]), static_cast<int>(mH[i] * mScale[i])));
#ifndef SHOW
            cv::imshow("Video_Stream", img);
            cv::waitKey(1);
#endif
            threadPool.tryStart([&, img = std::move(img)]() {
                thread_local QRScanner qrScanners;
                std::string str;
                qrScanners.decodeSingle(img, str);
                std::string ticket;
                bool isWebLogin = false;
                if (!parseOfficialQRCode(str, ticket, isWebLogin))
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
                    if (isWebLogin)
                    {
                        // 网页端扫码: 二维码即通行证登录URL(含tk+token_types), 直接用stoken确认
                        lastTicket = ticket;
                        lastQrCode = str;
                        nlohmann::json config = nlohmann::json::parse(m_config->getConfig());
                        if (config["auto_login"])
                        {
                            continueLastLogin();
                        }
                        else
                        {
                            emit loginConfirm(gameType, true);
                        }
                        stop();
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
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(DELAYED));
    }
    for (auto* b : mBuffer)
    {
        delete[] b;
    }
}

void QRCodeForScreen::LoginBH3BiliBili()
{
    QThreadPool threadPool;
    threadPool.setMaxThreadCount(threadNumber);
    std::mutex mtx;

    // ===== GDI 多屏捕获(替代DXGI, 兼容UU超级屏等虚拟显示器: DXGI桌面复制在虚拟屏返回损坏帧) =====
    // 枚举所有显示器(主屏+副屏+虚拟屏), 每个屏幕独立缓冲与等比缩放比例
    std::vector<RECT> mons = GetMonitorRects();
    std::vector<int> mX, mY, mW, mH;
    std::vector<uint8_t*> mBuffer;
    std::vector<double> mScale;
    for (const auto& r : mons)
    {
        mX.push_back(r.left);
        mY.push_back(r.top);
        mW.push_back(r.right - r.left);
        mH.push_back(r.bottom - r.top);
        mBuffer.push_back(new UCHAR[(r.right - r.left) * (r.bottom - r.top) * 4]);
        mScale.push_back(std::min(1280.0 / (r.right - r.left), 720.0 / (r.bottom - r.top)));
    }

    while (m_stop.load())
    {
        // 屏幕数量/尺寸变化检测: 变化时重新枚举并重分配缓冲(分辨率自适应)
        std::vector<RECT> newMons = GetMonitorRects();
        bool changed = newMons.size() != mons.size();
        if (!changed)
        {
            for (size_t i = 0; i < newMons.size(); ++i)
            {
                if (newMons[i].left != mons[i].left || newMons[i].top != mons[i].top ||
                    newMons[i].right != mons[i].right || newMons[i].bottom != mons[i].bottom)
                {
                    changed = true;
                    break;
                }
            }
        }
        if (changed)
        {
            mons = newMons;
            for (auto* b : mBuffer)
            {
                delete[] b;
            }
            mBuffer.clear();
            mX.clear();
            mY.clear();
            mW.clear();
            mH.clear();
            mScale.clear();
            for (const auto& r : mons)
            {
                mX.push_back(r.left);
                mY.push_back(r.top);
                mW.push_back(r.right - r.left);
                mH.push_back(r.bottom - r.top);
                mBuffer.push_back(new UCHAR[(r.right - r.left) * (r.bottom - r.top) * 4]);
                mScale.push_back(std::min(1280.0 / (r.right - r.left), 720.0 / (r.bottom - r.top)));
            }
        }
        if (mons.empty())
        {
            continue;
        }
        for (size_t i = 0; i < mons.size(); ++i)
        {
            if (!CaptureScreenGDIRegion(mBuffer[i], mX[i], mY[i], mW[i], mH[i]))
            {
                continue; // GDI抓屏失败, 跳过
            }
            cv::Mat img;
            cv::resize(cv::Mat(mH[i], mW[i], CV_8UC4, mBuffer[i]), img,
                       cv::Size(static_cast<int>(mW[i] * mScale[i]), static_cast<int>(mH[i] * mScale[i])));
#ifndef SHOW
            cv::imshow("Video_Stream", img);
            cv::waitKey(1);
#endif
            threadPool.tryStart([&, img = std::move(img)]() {
                thread_local QRScanner qrScanners;
                std::string str;
                qrScanners.decodeSingle(img, str);
                std::string ticket;
                bool isWebLogin = false;
                if (!parseOfficialQRCode(str, ticket, isWebLogin) || gameType != GameType::Honkai3 || isWebLogin)
                {
                    return; // 网页端通行证二维码在B服模式下忽略(需官服stoken确认)
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
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(DELAYED));
    }
    for (auto* b : mBuffer)
    {
        delete[] b;
    }
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