#include "QRCodeForScreen.h"

#include <chrono>
#include <thread>
#include <algorithm>
#include <Windows.h>

#include "Logger.hpp"

#include <QFuture>
#include <QtConcurrent/QtConcurrent>
#include <QThreadPool>

#include "QRScanner.h"
#include "ScreenScan.h"
#include "ScreenShotDXGI.hpp"

#define DELAYED 200

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
    ScreenShotDXGI screenshotdxgi;
    int w{ 0 };
    int h{ 0 };
    screenshotdxgi.InitDevice();
    if (!screenshotdxgi.InitDuplAuto(w, h))
    {
        DebugLog::Log("[QRCodeForScreen] 初始化失败: 未找到可复制的显示器输出(检查远程/虚拟屏占用), 将自动重试");
    }
    if (w == 0 || h == 0)
    {
        w = 1920;
        h = 1080; // 兜底尺寸, 防止分配0字节缓冲或除零, 循环内会自动重新初始化
    }
    long mBufferSize = w * h * 4;
    uint8_t* mBuffer = new UCHAR[mBufferSize];
    // 等比缩放: 保持宽高比, 避免修改分辨率后二维码被拉伸变形无法识别
    double scale = std::min(1280.0 / w, 720.0 / h);
    while (m_stop.load())
    {
        // 分辨率自适应: 输出尺寸变化时自动重新初始化(含远程/虚拟屏), 运行中改分辨率也能继续扫
        int sw{ 0 }, sh{ 0 };
        screenshotdxgi.GetOutputSize(sw, sh);
        if (sw != w || sh != h)
        {
            if (screenshotdxgi.InitDevice() && screenshotdxgi.InitDuplAuto(w, h))
            {
                mBufferSize = w * h * 4;
                delete[] mBuffer;
                mBuffer = new UCHAR[mBufferSize];
                scale = std::min(1280.0 / w, 720.0 / h);
                DebugLog::Log("[QRCodeForScreen] 桌面分辨率变化, 已自适应重新初始化: " + std::to_string(w) + "x" + std::to_string(h));
            }
            else
            {
                DebugLog::Log("[QRCodeForScreen] 分辨率变化重初始化失败, 下帧重试");
            }
        }
        if (screenshotdxgi.getFrame(100) != 0)
        {
            continue; // 无新帧或复制异常, 跳过本次(防止空指针崩溃)
        }
        screenshotdxgi.copyFrameToBuffer(&mBuffer, mBufferSize);
        cv::Mat img;
        cv::resize(cv::Mat(h, w, CV_8UC4, mBuffer), img,
                   cv::Size(static_cast<int>(w * scale), static_cast<int>(h * scale)));
#ifndef SHOW
        cv::imshow("Video_Stream", img);
        cv::waitKey(1);
#endif
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
        screenshotdxgi.doneWithFrame();
    }
    delete[] mBuffer;
}

void QRCodeForScreen::LoginBH3BiliBili()
{
    QThreadPool threadPool;
    threadPool.setMaxThreadCount(threadNumber);
    std::mutex mtx;
    ScreenShotDXGI screenshotdxgi;
    int w{ 0 };
    int h{ 0 };
    screenshotdxgi.InitDevice();
    if (!screenshotdxgi.InitDuplAuto(w, h))
    {
        DebugLog::Log("[QRCodeForScreen] 初始化失败: 未找到可复制的显示器输出(检查远程/虚拟屏占用), 将自动重试");
    }
    if (w == 0 || h == 0)
    {
        w = 1920;
        h = 1080; // 兜底尺寸, 防止分配0字节缓冲或除零, 循环内会自动重新初始化
    }
    long mBufferSize = w * h * 4;
    uint8_t* mBuffer = new UCHAR[mBufferSize];
    // 等比缩放: 保持宽高比, 避免修改分辨率后二维码被拉伸变形无法识别
    double scale = std::min(1280.0 / w, 720.0 / h);
    while (m_stop.load())
    {
        // 分辨率自适应: 输出尺寸变化时自动重新初始化(含远程/虚拟屏), 运行中改分辨率也能继续扫
        int sw{ 0 }, sh{ 0 };
        screenshotdxgi.GetOutputSize(sw, sh);
        if (sw != w || sh != h)
        {
            if (screenshotdxgi.InitDevice() && screenshotdxgi.InitDuplAuto(w, h))
            {
                mBufferSize = w * h * 4;
                delete[] mBuffer;
                mBuffer = new UCHAR[mBufferSize];
                scale = std::min(1280.0 / w, 720.0 / h);
                DebugLog::Log("[QRCodeForScreen] 桌面分辨率变化, 已自适应重新初始化: " + std::to_string(w) + "x" + std::to_string(h));
            }
            else
            {
                DebugLog::Log("[QRCodeForScreen] 分辨率变化重初始化失败, 下帧重试");
            }
        }
        if (screenshotdxgi.getFrame(100) != 0)
        {
            continue; // 无新帧或复制异常, 跳过本次(防止空指针崩溃)
        }
        screenshotdxgi.copyFrameToBuffer(&mBuffer, mBufferSize);
        cv::Mat img;
        cv::resize(cv::Mat(h, w, CV_8UC4, mBuffer), img,
                   cv::Size(static_cast<int>(w * scale), static_cast<int>(h * scale)));
#ifndef SHOW
        cv::imshow("Video_Stream", img);
        cv::waitKey(1);
#endif
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
        screenshotdxgi.doneWithFrame();
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