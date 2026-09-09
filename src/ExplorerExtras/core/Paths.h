// Paths.h - filesystem locations owned by the app.
#pragma once

#include <string>

namespace ee {

// %LOCALAPPDATA%\ExplorerExtras, created on first call. Empty string on failure.
const std::wstring& AppDataDir();

// Full path of the running executable.
const std::wstring& ExecutablePath();

}  // namespace ee
