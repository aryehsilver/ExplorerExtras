#include "Paths.h"

#include <windows.h>
#include <shlobj.h>

namespace ee {
namespace {

std::wstring ComputeExecutablePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

bool DirectoryIsWritable(const std::wstring& dir) {
    const std::wstring probe = dir + L"\\.ee-write-test";
    const HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

std::wstring ComputeLocalAppDataDir() {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return {};
    std::wstring dir(local);
    CoTaskMemFree(local);
    dir += L"\\ExplorerExtras";
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return {};
    }
    return dir;
}

std::wstring ComputeDataDir() {
    // Prefer the executable's own folder. It keeps the log and settings beside
    // the build where they are easy to find, and avoids burying diagnostics in
    // a hidden AppData path.
    const std::wstring& exe = ExecutablePath();
    const size_t slash = exe.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        std::wstring dir = exe.substr(0, slash);
        if (DirectoryIsWritable(dir)) return dir;
    }
    // An installed copy under Program Files is not writable; fall back.
    return ComputeLocalAppDataDir();
}

}  // namespace

const std::wstring& AppDataDir() {
    static const std::wstring dir = ComputeDataDir();
    return dir;
}

const std::wstring& ExecutablePath() {
    static const std::wstring path = ComputeExecutablePath();
    return path;
}

}  // namespace ee
