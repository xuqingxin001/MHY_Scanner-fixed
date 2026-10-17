#include "WindowMain.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <tlhelp32.h>
#include <exception>
#include <mutex>
#include <signal.h>
#include <stdlib.h>

#include <QMessageBox>
#include <QStyleHints>
#include <opencv2/core/utils/logger.hpp>

#pragma comment(lib, "dbghelp.lib")

// _vsnwprintf_s 完整支持 %p/%llX/%ls 等格式(wvsprintfW 不支持64位地址导致崩溃报告地址乱码)
#pragma warning(push)
#pragma warning(disable : 4996)

static std::mutex mtx;

static bool isOpen()
{
    HANDLE hMutex = CreateMutex(NULL, true, TEXT("T08lJ8CJmJiyoxdV"));
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// 崩溃报告输出目录: exe 所在目录(而非进程工作目录), 保证用户能找到 dmp/log
static void GetExeDirectory(WCHAR* dir, size_t size)
{
    WCHAR exe[MAX_PATH]{ 0 };
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR* slash = wcsrchr(exe, L'\\');
    if (slash != nullptr)
    {
        *(slash + 1) = L'\0';
    }
    wcsncpy_s(dir, size, exe, _TRUNCATE);
}

// 崩溃报告 .log 以 UTF-8 写入(文件开头写BOM), 保证记事本/常见编辑器正常识别中文
static void LogWrite(HANDLE hFile, const WCHAR* text)
{
    char utf8[8192]{ 0 };
    const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL);
    if (len > 1)
    {
        DWORD written = 0;
        WriteFile(hFile, utf8, static_cast<DWORD>(len - 1), &written, nullptr);
    }
}

static void LogWriteFmt(HANDLE hFile, const WCHAR* fmt, ...)
{
    WCHAR buf[2048]{ 0 };
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    LogWrite(hFile, buf);
}

// StackWalk64 读内存回调
static BOOL WINAPI ReadProcMem(HANDLE, DWORD64 addr, PVOID buf, DWORD size, DWORD* bytesRead)
{
    SIZE_T read = 0;
    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), buf, size, &read))
    {
        *bytesRead = static_cast<DWORD>(read);
        return TRUE;
    }
    *bytesRead = 0;
    return FALSE;
}

// 已加载模块列表(供 dmp 对照解析)
static void WriteModuleList(HANDLE hLog)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
    {
        return;
    }
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    LogWrite(hLog, L"---- 已加载模块(基址-大小 名称) ----\r\n");
    if (Module32FirstW(snap, &me))
    {
        do
        {
            LogWriteFmt(hLog, L"0x%08llX-%08llX %s\r\n",
                        static_cast<ULONGLONG>(reinterpret_cast<uintptr_t>(me.modBaseAddr)),
                        static_cast<ULONGLONG>(me.modBaseSize), me.szModule);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

// 调用栈(加载exe同目录PDB解析函数名; 无符号时输出 模块+偏移, 同样可定位崩溃函数)
static void WriteStackTrace(HANDLE hLog, _EXCEPTION_POINTERS* pEx)
{
    LogWrite(hLog, L"---- 调用栈(模块+函数+偏移) ----\r\n");
    CONTEXT ctx = *pEx->ContextRecord;
    STACKFRAME64 frame{};
    DWORD machine = 0;
#ifdef _M_X64
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
#else
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = ctx.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Esp;
    frame.AddrStack.Mode = AddrModeFlat;
#endif
    WCHAR exeDir[MAX_PATH]{ 0 };
    GetExeDirectory(exeDir, MAX_PATH);
    SymInitialize(GetCurrentProcess(), exeDir, TRUE); // 从 exe 目录加载同名 PDB 符号
    int count = 0;
    while (count < 32 && StackWalk64(machine, GetCurrentProcess(), GetCurrentThread(), &frame, &ctx,
                                     ReadProcMem, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
    {
        if (frame.AddrPC.Offset == 0)
        {
            break;
        }
        const DWORD64 base = SymGetModuleBase64(GetCurrentProcess(), frame.AddrPC.Offset);
        if (base != 0)
        {
            WCHAR modName[MAX_PATH]{ 0 };
            GetModuleFileNameW(reinterpret_cast<HMODULE>(base), modName, MAX_PATH);
            WCHAR* name = wcsrchr(modName, L'\\');
            if (name != nullptr)
            {
                ++name;
            }
            // 解析函数名(PDB 与 exe 同目录时输出 模块!函数+偏移)
            char funcBuf[1024]{ 0 };
            SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(funcBuf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 480;
            DWORD64 disp = 0;
            const bool hasFunc = SymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &disp, sym) != FALSE && sym->Name[0] != '\0';
            if (hasFunc)
            {
                LogWriteFmt(hLog, L"#%02d %s!%hs+0x%llX\r\n", count, name != nullptr ? name : modName,
                            sym->Name, static_cast<ULONGLONG>(disp));
            }
            else
            {
                LogWriteFmt(hLog, L"#%02d %s+0x%llX\r\n", count, name != nullptr ? name : modName,
                            static_cast<ULONGLONG>(frame.AddrPC.Offset - base));
            }
        }
        else
        {
            LogWriteFmt(hLog, L"#%02d 0x%llX\r\n", count, static_cast<ULONGLONG>(frame.AddrPC.Offset));
        }
        ++count;
    }
    SymCleanup(GetCurrentProcess());
}

static long ExceptionFilter(_EXCEPTION_POINTERS* ExceptionInfo)
{
    std::lock_guard<std::mutex> lock(mtx);
    WCHAR exeDir[MAX_PATH]{ 0 };
    GetExeDirectory(exeDir, MAX_PATH);
    SYSTEMTIME stLocalTime;
    GetLocalTime(&stLocalTime);
    WCHAR baseName[MAX_PATH]{ 0 };
    wsprintfW(baseName, L"%sMHY_Scanner-%04d%02d%02d-%02d%02d%02d",
              exeDir, stLocalTime.wYear, stLocalTime.wMonth, stLocalTime.wDay,
              stLocalTime.wHour, stLocalTime.wMinute, stLocalTime.wSecond);

    // 1) 二进制小型转储 .dmp
    WCHAR dmpPath[MAX_PATH]{ 0 };
    wsprintfW(dmpPath, L"%s.dmp", baseName);
    HANDLE hFile = CreateFileW(dmpPath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION ExInfo;
        ExInfo.ThreadId = GetCurrentThreadId();
        ExInfo.ExceptionPointers = ExceptionInfo;
        ExInfo.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &ExInfo, NULL, NULL);
        CloseHandle(hFile);
    }

    // 2) 可读文本崩溃报告 .log(异常信息+调用栈+模块列表)
    WCHAR logPath[MAX_PATH]{ 0 };
    wsprintfW(logPath, L"%s.log", baseName);
    HANDLE hLog = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hLog != INVALID_HANDLE_VALUE)
    {
        // UTF-8 BOM: 记事本据此正确识别编码
        const BYTE bom[3] = { 0xEF, 0xBB, 0xBF };
        DWORD bomWritten = 0;
        WriteFile(hLog, bom, sizeof(bom), &bomWritten, nullptr);
        LogWriteFmt(hLog, L"MHY_Scanner 崩溃报告\r\n时间: %04d-%02d-%02d %02d:%02d:%02d\r\n",
                    stLocalTime.wYear, stLocalTime.wMonth, stLocalTime.wDay,
                    stLocalTime.wHour, stLocalTime.wMinute, stLocalTime.wSecond);
        LogWriteFmt(hLog, L"崩溃线程ID: %lu\r\n", GetCurrentThreadId());
        if (ExceptionInfo != nullptr && ExceptionInfo->ExceptionRecord != nullptr)
        {
            const DWORD code = ExceptionInfo->ExceptionRecord->ExceptionCode;
            LPCWSTR codeDesc = L"";
            switch (code)
            {
            case 0xC0000005: codeDesc = L"访问违例(空指针/越界)"; break;
            case 0xC0000094:
            case 0xC0000095: codeDesc = L"整数溢出/除零"; break;
            case 0xC00000FD: codeDesc = L"栈溢出"; break;
            case 0xC0000409: codeDesc = L"安全检查失败(__fastfail)"; break;
            case 0xE06D7363: codeDesc = L"未捕获C++异常"; break;
            default: break;
            }
            LogWriteFmt(hLog, L"异常代码: 0x%08X %s\r\n", code, codeDesc);
            LogWriteFmt(hLog, L"异常地址: 0x%08llX\r\n",
                        static_cast<ULONGLONG>(reinterpret_cast<uintptr_t>(ExceptionInfo->ExceptionRecord->ExceptionAddress)));
            if (code == 0xC0000005 && ExceptionInfo->ExceptionRecord->NumberParameters >= 2)
            {
                LogWriteFmt(hLog, L"访问类型: %s, 访问地址: 0x%08llX\r\n",
                            ExceptionInfo->ExceptionRecord->ExceptionInformation[0] ? L"写" : L"读",
                            static_cast<ULONGLONG>(ExceptionInfo->ExceptionRecord->ExceptionInformation[1]));
            }
            LogWriteFmt(hLog, L"异常标志: 0x%X\r\n", ExceptionInfo->ExceptionRecord->ExceptionFlags);
        }
        WriteStackTrace(hLog, ExceptionInfo);
        WriteModuleList(hLog);
        CloseHandle(hLog);
    }

    // 3) 弹窗提示报告位置
    WCHAR msg[MAX_PATH * 2]{ 0 };
    wsprintfW(msg, L"程序意外崩溃, 已输出崩溃报告:\r\n%s\r\n\r\n请把 .log 文件发给开发者分析。", baseName);
    MessageBoxW(NULL, msg, L"意外崩溃", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}
#pragma warning(pop)

// 无异常指针时构造上下文, 复用同一报告流程(供 terminate/abort/CRT错误等兜底调用)
static void ReportCrashNoContext(DWORD code)
{
    std::lock_guard<std::mutex> lock(mtx);
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD er{};
    er.ExceptionCode = code;
#ifdef _M_X64
    er.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
#else
    er.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Eip);
#endif
    EXCEPTION_POINTERS ep{ &er, &ctx };
    ExceptionFilter(&ep);
}

// C++ 未捕获异常兜底(Qt 事件循环逃逸的异常会走 std::terminate), 伪装成 0xE06D7363 触发同一报告流程
static void TerminateHandler()
{
    ReportCrashNoContext(0xE06D7363); // MSVC C++ 异常
    _exit(3); // 直接退出, 避免再触发 abort signal 产生重复报告
}

// abort/qFatal/assert 等致命终止路径: 默认不经异常过滤器直接消失, 挂 signal 捕获后写报告
static void __cdecl AbortSignalHandler(int)
{
    ReportCrashNoContext(0xC0000409); // abort 类致命错误(安全检查失败/断言/Qt致命错误)
    _exit(3);
}

// 纯虚函数调用兜底(程序消失常见原因之一)
static void __cdecl PureCallHandler()
{
    ReportCrashNoContext(0xC0000409);
}

// CRT 参数校验失败兜底(常见于非法句柄/索引越界等)
static void __cdecl InvalidParamHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
    ReportCrashNoContext(0xC0000409);
}

int main(int argc, char* argv[])
{
    UINT utf8 = 65001;
    SetConsoleOutputCP(utf8);
    SetUnhandledExceptionFilter(ExceptionFilter);
    std::set_terminate(TerminateHandler);
    // abort/断言/Qt致命错误(qFatal)走 signal 兜底, 避免"直接消失无日志"
    signal(SIGABRT, AbortSignalHandler);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_purecall_handler(PureCallHandler);
    _set_invalid_parameter_handler(InvalidParamHandler);
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);
    QApplication a(argc, argv);
    QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
    if (isOpen())
    {
        HWND hwnd = FindWindowW(NULL, L"MHY扫码器");
        if (hwnd == NULL)
        {
            return 0;
        }
        if (GetForegroundWindow() == hwnd)
        {
            return 0;
        }
        ShowWindow(hwnd, SW_MINIMIZE);
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
        SetForegroundWindow(hwnd);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
        return -1;
    }
    WindowMain w;
    w.show();
    return a.exec();
}
