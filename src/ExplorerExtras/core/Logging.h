// Logging.h - small append-only UTF-8 file log, safe to call from any thread.
#pragma once

#include <sal.h>

#include <string>

namespace ee::log {

void Init();
void Shutdown();
void Write(const wchar_t* level, _Printf_format_string_ const wchar_t* fmt, ...);

// Path of the log file (may be empty if logging could not start).
const std::wstring& FilePath();

}  // namespace ee::log

#define EE_INFO(...) ::ee::log::Write(L"INFO", __VA_ARGS__)
#define EE_WARN(...) ::ee::log::Write(L"WARN", __VA_ARGS__)
#define EE_ERR(...)  ::ee::log::Write(L"ERR ", __VA_ARGS__)
