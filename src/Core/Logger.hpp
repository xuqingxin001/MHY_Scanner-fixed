#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <filesystem>

#include <Windows.h>

// 轻量文件日志：写 MHY_Scanner_debug.log（与 exe 同目录），线程安全，UTF-8。
namespace DebugLog
{
inline std::mutex& LogMutex()
{
    static std::mutex m;
    return m;
}

inline std::string GetLogPath()
{
    static std::string path = [] {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::filesystem::path p(buf);
        return (p.parent_path() / "MHY_Scanner_debug.log").string();
    }();
    return path;
}

inline void Log(const std::string& msg)
{
    std::lock_guard lock(LogMutex());
    try
    {
        std::ofstream f(GetLogPath(), std::ios::app);
        if (!f)
        {
            return;
        }
        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &t);
        char buf[32]{};
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        f << "[" << buf << "] " << msg << "\n";
        f.flush();
    }
    catch (...)
    {
    }
}
}
