#include "QRCodeForScreen.h"

#include <chrono>
#include <thread>
#include <algorithm>
#include <vector>
#include <memory>
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
    // ===== 多屏自我识别: 枚举所有可复制的显示器输出, 每个输出独立捕获, 二维码在哪个屏幕都能扫到 =====
    std::vector<std::unique_ptr<ScreenShotDXGI>> captures;
    std::vector<int> capsW, capsH;
    const int outCount = ScreenShotDXGI::GetOutputCount();
    for (int idx = 0; idx < outCount; ++idx)
    {
        auto cap = std::make_unique<ScreenShotDXGI>();
        int cw{ 0 }, ch{ 0 };
        cap->InitDevice();
        if (cap->InitDupl(static_cast<UINT>(idx), cw, ch))
        {
            captures.push_back(std::move(cap));
            capsW.push_back(cw);
            capsH.push_back(ch);
            DebugLog::Log("[QRCodeForScreen] 监视输出" + std::to_string(idx) + ": " + std::to_string(cw) + "x" + std::to_string(ch));
        }
        else
        {
            DebugLog::Log("[QRCodeForScreen] 输出" + std::to_string(idx) + "无法复制, 跳过");
        }
    }
    if (captures.empty())
    {
        DebugLog::Log("[QRCodeForScreen] 未找到可复制的显示器输出(检查远程/虚拟屏占用)");
    }

    // 每个输出独立缓冲与等比缩放比例
    std::vector<uint8_t*> buffers;
    std::vector<double> scales;
    for (size_t i = 0; i < captures.size(); ++i)
    {
        buffers.push_back(new UCHAR[capsW[i] * capsH[i] * 4]);
        scales.push_back(std::min(1280.0 / capsW[i], 720.0 / capsH[i]));
    }

    while (m_stop.load())
    {
        for (size_t i = 0; i < captures.size(); ++i)
        {
            auto& cap = captures[i];
            // 分辨率自适应: 输出尺寸变化时重新初始化
            int sw{ 0 }, sh{ 0 };
            cap->GetOutputSize(sw, sh);
            if (sw != capsW[i] || sh != capsH[i])
            {
                if (cap->InitDevice() && cap->InitDupl(static_cast<UINT>(i), sw, sh))
                {
                    capsW[i] = sw;
                    capsH[i] = sh;
                    delete[] buffers[i];
                    buffers[i] = new UCHAR[sw * sh * 4];
                    scales[i] = std::min(1280.0 / sw, 720.0 / sh);
                    DebugLog::Log("[QRCodeForScreen] 输出" + std::to_string(i) + "尺寸变化, 已自适应: " + std::to_string(sw) + "x" + std::to_string(sh));
                }
                else
                {
                    DebugLog::Log("[QRCodeForScreen] 输出" + std::to_string(i) + "重初始化失败, 下帧重试");
                    continue;
                }
            }
            if (cap->getFrame(100) != 0)
            {
                continue; // 无新帧或复制异常, 跳过(防止空指针崩溃)
            }
            cap->copyFrameToBuffer(&buffers[i], capsW[i] * capsH[i] * 4);
            cv::Mat img;
            cv::resize(cv::Mat(capsH[i], capsW[i], CV_8UC4, buffers[i]), img,
                       cv::Size(static_cast<int>(capsW[i] * scales[i]), static_cast<int>(capsH[i] * scales[i])));
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
            cap->doneWithFrame();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(DELAYED));
    }
    for (auto* b : buffers)
    {
        delete[] b;
    }
}

void QRCodeForScreen::LoginBH3BiliBili()
{
    QThreadPool threadPool;
    threadPool.setMaxThreadCount(threadNumber);
    std::mutex mtx;
    // ===== 多屏自我识别: 枚举所有可复制的显示器输出, 每个输出独立捕获, 二维码在哪个屏幕都能扫到 =====
    std::vector<std::unique_ptr<ScreenShotDXGI>> captures;
    std::vector<int> capsW, capsH;
    const int outCount = ScreenShotDXGI::GetOutputCount();
    for (int idx = 0; idx < outCount; ++idx)
    {
        auto cap = std::make_unique<ScreenShotDXGI>();
        int cw{ 0 }, ch{ 0 };
        cap->InitDevice();
        if (cap->InitDupl(static_cast<UINT>(idx), cw, ch))
        {
            captures.push_back(std::move(cap));
            capsW.push_back(cw);
            capsH.push_back(ch);
            DebugLog::Log("[QRCodeForScreen] 监视输出" + std::to_string(idx) + ": " + std::to_string(cw) + "x" + std::to_string(ch));
        }
        else
        {
            DebugLog::Log("[QRCodeForScreen] 输出" + std::to_string(idx) + "无法复制, 跳过");
        }
    }
    if (captures.empty())
    {
        DebugLog::Log("[QRCodeForScreen] 未找到可复制的显示器输出(检查远程/虚拟屏占用)");
    }

    // 每个输出独立缓冲与等比缩放比例
    std::vector<uint8_t*> buffers;
    std::vector<double> scales;
    for (size_t i = 0; i < captures.size(); ++i)
    {
        buffers.push_back(new UCHAR[capsW[i] * capsH[i] * 4]);
        scales.push_back(std::min(1280.0 / capsW[i], 720.0 / capsH[i]));
    }

    while (m_stop.load())
    {
        for (size_t i = 0; i < captures.size(); ++i)
        {
            auto& cap = captures[i];
            // 分辨率自适应: 输出尺寸变化时重新初始化
            int sw{ 0 }, sh{ 0 };
            cap->GetOutputSize(sw, sh);
            if (sw != capsW[i] || sh != capsH[i])
            {
                if (cap->InitDevice() && cap->InitDupl(static_cast<UINT>(i), sw, sh))
                {
                    capsW[i] = sw;
                    capsH[i] = sh;
                    delete[] buffers[i];
                    buffers[i] = new UCHAR[sw * sh * 4];
                    scales[i] = std::min(1280.0 / sw, 720.0 / sh);
                    DebugLog::Log("[QRCodeForScreen] 输出" + std::to_string(i) + "尺寸变化, 已自适应: " + std::to_string(sw) + "x" + std::to_string(sh));
                }
                else
                {
                    DebugLog::Log("[QRCodeForScreen] 输出" + std::to_string(i) + "重初始化失败, 下帧重试");
                    continue;
                }
            }
            if (cap->getFrame(100) != 0)
            {
                continue; // 无新帧或复制异常, 跳过(防止空指针崩溃)
            }
            cap->copyFrameToBuffer(&buffers[i], capsW[i] * capsH[i] * 4);
            cv::Mat img;
            cv::resize(cv::Mat(capsH[i], capsW[i], CV_8UC4, buffers[i]), img,
                       cv::Size(static_cast<int>(capsW[i] * scales[i]), static_cast<int>(capsH[i] * scales[i])));
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
            cap->doneWithFrame();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(DELAYED));
    }
    for (auto* b : buffers)
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