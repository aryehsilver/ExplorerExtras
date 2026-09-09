#include "Logging.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "Paths.h"

namespace ee::log {
namespace {

constexpr DWORD kMaxLogBytes = 1u << 20;  // 1 MiB, truncated at startup

std::mutex g_mutex;
std::wstring g_path;
bool g_started = false;

void AppendUtf8(const std::wstring& text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::vector<char> utf8(static_cast<size_t>(bytes));
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), bytes,
                        nullptr, nullptr);

    const HANDLE file = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
}

}  // namespace

void Init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_started) return;
    const std::wstring& dir = AppDataDir();
    if (dir.empty()) return;
    g_path = dir + L"\\ExplorerExtras.log";

    // Keep the file bounded; start fresh once it grows past the cap.
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &info) &&
        info.nFileSizeHigh == 0 && info.nFileSizeLow > kMaxLogBytes) {
        DeleteFileW(g_path.c_str());
    }
    g_started = true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_started = false;
    g_path.clear();
}

const std::wstring& FilePath() {
    return g_path;
}

void Write(const wchar_t* level, const wchar_t* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_started || g_path.empty()) return;

    wchar_t message[1024];
    va_list args;
    va_start(args, fmt);
    const int n = _vsnwprintf_s(message, _TRUNCATE, fmt, args);
    va_end(args);
    if (n < 0) wcscpy_s(message, L"<log format error>");

    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t line[1280];
    _snwprintf_s(line, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%5lu] %s %s\r\n", st.wYear,
                 st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 GetCurrentThreadId(), level, message);

    AppendUtf8(line);
}

}  // namespace ee::log
