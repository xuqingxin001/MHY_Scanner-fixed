#include "QRCodeForScreen.h"

#include <chrono>
#include <thread>
#include <algorithm>
#include <vector>
#include <Windows.h>
#include <opencv2/imgcodecs.hpp>

#include "Logger.hpp"

#include <QFuture>
#include <QtConcurrent/QtConcurrent>
#include <QThreadPool>

#include "QRScanner.h"
#include "ScreenScan.h"
#include "ScreenShotDXGI.hpp"
#include <opencv2/imgproc.hpp>

#define DELAYED 200

// DXGI 坏帧检测: UU超级屏等虚拟显示器上 DXGI 桌面复制返回水平撕裂彩色条纹,
// 特征为高饱和像素占绝对多数(正常桌面远低于50%); 命中即判定该屏 DXGI 不可用, 回退 GDI
static bool IsFrameCorrupted(const cv::Mat& bgra)
{
    cv::Mat small;
    cv::resize(bgra, small, cv::Size(64, 36));
    const int total = small.rows * small.cols;
    int sat = 0;
    for (int y = 0; y < small.rows; ++y)
    {
        const cv::Vec4b* row = small.ptr<cv::Vec4b>(y);
        for (int x = 0; x < small.cols; ++x)
        {
            // BGRA: 手动计算饱和度(避免依赖各OpenCV版本枚举差异), S=(max-min)/max
            const cv::Vec4b& p = row[x];
            const int mx = std::max(p[0], std::max(p[1], p[2]));
            const int mn = std::min(p[0], std::min(p[1], p[2]));
            if (mx > 0 && (mx - mn) * 100 / mx > 70)
            {
                ++sat;
            }
        }
    }
    return static_cast<double>(sat) / total > 0.5;
}

// SEH 异常保护: DXGI 桌面复制在部分环境(虚拟屏/被其他软件同时占用)可能内部崩溃,
// 任何 DXGI 调用崩溃都被捕获并视为该屏 DXGI 不可用, 自动回退 GDI, 保证程序不闪退。
// 包装函数内无 C++ 局部对象析构, 可安全使用 __try/__except
static int SafeGetFrame(ScreenShotDXGI& dxgi, int timeout)
{
    __try { return dxgi.getFrame(timeout); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}

static bool SafeCopyFrame(ScreenShotDXGI& dxgi, uint8_t* buffer, size_t size)
{
    __try { return dxgi.copyFrameToBuffer(&buffer, static_cast<long>(size)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeDoneWithFrame(ScreenShotDXGI& dxgi)
{
    __try { return dxgi.doneWithFrame(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeInitDupl(ScreenShotDXGI& dxgi, int idx, int& w, int& h)
{
    __try { return dxgi.InitDupl(static_cast<UINT>(idx), w, h); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// 每个屏幕一个捕获源: DXGI 优先, 初始化失败/抓帧失败/坏帧/崩溃/连续无新帧时自动回退 GDI
struct ScreenCapturer
{
    ScreenShotDXGI dxgi;
    RECT rect{};
    bool dxgiUsable = false;
    int dxgiFailCount = 0; // 连续无新帧计数, 超过阈值永久回退GDI(远程软件抢占桌面复制时DXGI常超时)

    // 按全局输出序号初始化 DXGI 桌面复制, 记录虚拟桌面坐标用于与 GDI 屏配对
    bool Init(int outputIdx)
    {
        int w = 0, h = 0;
        if (SafeInitDupl(dxgi, outputIdx, w, h))
        {
            rect = dxgi.GetOutputRect();
            dxgiUsable = true;
            return true;
        }
        return false;
    }

    bool Matches(int x, int y, int w, int h) const
    {
        return rect.left == x && rect.top == y &&
               rect.right - rect.left == w && rect.bottom - rect.top == h;
    }

    // 返回值: 0=已捕获有效帧 1=本轮无新帧(需GDI补抓, 不再整屏跳过) 2=失败需GDI兜底
    int CaptureDXGI(uint8_t* buffer, int w, int h)
    {
        if (!dxgiUsable)
        {
            return 2;
        }
        const int fr = SafeGetFrame(dxgi, 100);
        if (fr == 1)
        {
            dxgiUsable = false; // 桌面复制失效/错误(切屏/锁屏/崩溃等) → 永久回退GDI
            return 2;
        }
        if (fr == 2)
        {
            // 无新帧(超时): 远程软件(UU/向日葵等)同时占用桌面复制时可能持续超时,
            // 连续阈值内先返回1由GDI补抓, 超过阈值永久回退GDI
            if (++dxgiFailCount >= 15)
            {
                dxgiUsable = false;
                return 2;
            }
            return 1;
        }
        const bool ok = SafeCopyFrame(dxgi, buffer, static_cast<size_t>(w) * h * 4);
        if (!SafeDoneWithFrame(dxgi))
        {
            dxgiUsable = false; // 释放帧失败/崩溃 → 回退GDI
            return 2;
        }
        if (!ok || IsFrameCorrupted(cv::Mat(h, w, CV_8UC4, buffer)))
        {
            dxgiUsable = false; // 复制失败或坏帧(虚拟屏条纹) → 回退GDI
            return 2;
        }
        dxgiFailCount = 0; // 拿到有效帧, 计数清零
        return 0;
    }
};

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

    // DXGI 优先捕获: 为所有全局输出建立桌面复制, 初始化失败/抓帧失败/坏帧时自动回退GDI
    std::vector<ScreenCapturer> capturers;
    const int outCount = ScreenShotDXGI::GetOutputCount();
    for (int o = 0; o < outCount; ++o)
    {
        ScreenCapturer sc;
        if (sc.Init(o))
        {
            capturers.push_back(std::move(sc));
        }
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
            // 屏幕布局变化(开/关超级屏等)后重建 DXGI 桌面复制
            capturers.clear();
            const int newOutCount = ScreenShotDXGI::GetOutputCount();
            for (int o = 0; o < newOutCount; ++o)
            {
                ScreenCapturer sc;
                if (sc.Init(o))
                {
                    capturers.push_back(std::move(sc));
                }
            }
        }
        if (mons.empty())
        {
            continue;
        }
        for (size_t i = 0; i < mons.size(); ++i)
        {
            // 1) DXGI 优先(按虚拟桌面坐标匹配该屏), 失败/坏帧自动回退GDI
            int capRes = 2;
            for (auto& sc : capturers)
            {
                if (sc.Matches(mX[i], mY[i], mW[i], mH[i]))
                {
                    capRes = sc.CaptureDXGI(mBuffer[i], mW[i], mH[i]);
                    break;
                }
            }
            if (capRes == 1)
            {
                // DXGI无新帧(超时/被远程软件抢占): 不整屏跳过, 用GDI补抓该屏, 保证能扫到
                if (!CaptureScreenGDIRegion(mBuffer[i], mX[i], mY[i], mW[i], mH[i]))
                {
                    continue; // GDI抓屏也失败, 跳过
                }
            }
            if (capRes == 2)
            {
                if (!CaptureScreenGDIRegion(mBuffer[i], mX[i], mY[i], mW[i], mH[i]))
                {
                    continue; // GDI抓屏失败, 跳过
                }
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

    // DXGI 优先捕获: 为所有全局输出建立桌面复制, 初始化失败/抓帧失败/坏帧时自动回退GDI
    std::vector<ScreenCapturer> capturers;
    const int outCount = ScreenShotDXGI::GetOutputCount();
    for (int o = 0; o < outCount; ++o)
    {
        ScreenCapturer sc;
        if (sc.Init(o))
        {
            capturers.push_back(std::move(sc));
        }
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
            // 屏幕布局变化(开/关超级屏等)后重建 DXGI 桌面复制
            capturers.clear();
            const int newOutCount = ScreenShotDXGI::GetOutputCount();
            for (int o = 0; o < newOutCount; ++o)
            {
                ScreenCapturer sc;
                if (sc.Init(o))
                {
                    capturers.push_back(std::move(sc));
                }
            }
        }
        if (mons.empty())
        {
            continue;
        }
        for (size_t i = 0; i < mons.size(); ++i)
        {
            // 1) DXGI 优先(按虚拟桌面坐标匹配该屏), 失败/坏帧自动回退GDI
            int capRes = 2;
            for (auto& sc : capturers)
            {
                if (sc.Matches(mX[i], mY[i], mW[i], mH[i]))
                {
                    capRes = sc.CaptureDXGI(mBuffer[i], mW[i], mH[i]);
                    break;
                }
            }
            if (capRes == 1)
            {
                // DXGI无新帧(超时/被远程软件抢占): 不整屏跳过, 用GDI补抓该屏, 保证能扫到
                if (!CaptureScreenGDIRegion(mBuffer[i], mX[i], mY[i], mW[i], mH[i]))
                {
                    continue; // GDI抓屏也失败, 跳过
                }
            }
            if (capRes == 2)
            {
                if (!CaptureScreenGDIRegion(mBuffer[i], mX[i], mY[i], mW[i], mH[i]))
                {
                    continue; // GDI抓屏失败, 跳过
                }
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