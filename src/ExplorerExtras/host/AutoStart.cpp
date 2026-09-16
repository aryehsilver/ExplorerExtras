#include "AutoStart.h"

#include <windows.h>

#include <string>
#include <vector>

#include "../core/Logging.h"
#include "../core/Paths.h"

namespace ee {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// Where the Settings app and Task Manager record what the user has switched
// off. Undocumented, but it is the only place that answer exists, and reading
// it is the difference between reporting the truth and reporting the Run entry.
constexpr wchar_t kApprovedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kValueName[] = L"ExplorerExtras";

std::wstring ReadRunEntry() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return {};
    }

    DWORD type = 0;
    DWORD bytes = 0;
    std::wstring value;
    if (RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && bytes > sizeof(wchar_t)) {
        value.resize(bytes / sizeof(wchar_t));
        if (RegQueryValueExW(key, kValueName, nullptr, nullptr,
                             reinterpret_cast<BYTE*>(value.data()), &bytes) == ERROR_SUCCESS) {
            // The stored length includes the terminator, and may include more
            // than one if whoever wrote it was careless.
            while (!value.empty() && value.back() == L'\0') value.pop_back();
        } else {
            value.clear();
        }
    }
    RegCloseKey(key);
    return value;
}

// The path out of a command line: ours is one quoted path and nothing else,
// but an entry left by an older build - or by hand - may be bare.
std::wstring PathFromCommand(const std::wstring& command) {
    std::wstring path = command;
    if (path.size() >= 2 && path.front() == L'"') {
        const size_t closing = path.find(L'"', 1);
        path = closing == std::wstring::npos ? path.substr(1) : path.substr(1, closing - 1);
    }
    while (!path.empty() && path.back() == L' ') path.pop_back();
    return path;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// The approval value is twelve bytes: a flag word, then the time it was turned
// off. An odd flag means disabled - 2 and 6 are on, 3 and 7 are off.
bool ReadStartupBlock() {
    BYTE data[12]{};
    DWORD bytes = sizeof(data);
    DWORD type = 0;
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValueName,
                                        RRF_RT_REG_BINARY, &type, data, &bytes);
    if (status != ERROR_SUCCESS || bytes == 0) return false;  // no entry means allowed
    return (data[0] & 1) != 0;
}

// Double-clicking the executable inside the downloaded zip, rather than
// extracting it first, runs it out of a folder Windows made up and will delete.
// A Run entry pointing in there is a startup that works until it does not.
bool RunningFromTemp() {
    wchar_t temp[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
    if (length == 0 || length > MAX_PATH) return false;

    const std::wstring& exe = ExecutablePath();
    const std::wstring folder(temp, length);
    if (exe.size() <= folder.size()) return false;
    return CompareStringOrdinal(exe.c_str(), static_cast<int>(folder.size()), folder.c_str(),
                                static_cast<int>(folder.size()), TRUE) == CSTR_EQUAL;
}

}  // namespace

AutoStartState AutoStartStatus() {
    AutoStartState state;
    state.command = ReadRunEntry();
    state.registered = !state.command.empty();
    state.points_here = SamePath(PathFromCommand(state.command), ExecutablePath());
    state.blocked = ReadStartupBlock();
    state.temporary = RunningFromTemp();
    return state;
}

bool IsAutoStartEnabled() {
    const AutoStartState state = AutoStartStatus();
    return state.registered && state.points_here && !state.blocked;
}

bool SetAutoStartEnabled(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        EE_ERR(L"cannot open Run key for writing");
        return false;
    }

    LSTATUS status;
    if (enabled) {
        // Quoted so a path containing spaces still launches correctly.
        const std::wstring command = L"\"" + ExecutablePath() + L"\"";
        status = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kValueName);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);

    if (status != ERROR_SUCCESS) {
        EE_ERR(L"auto-start update failed, status=%ld", status);
        return false;
    }
    EE_INFO(L"auto-start %s for '%s'", enabled ? L"enabled" : L"disabled",
            ExecutablePath().c_str());
    return true;
}

bool ClearStartupBlock() {
    if (!ReadStartupBlock()) return true;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        EE_WARN(L"cannot open the startup approval key; Windows keeps blocking us");
        return false;
    }

    // Enabled, with no time of disabling - which is exactly what Windows writes
    // for an entry that has never been turned off.
    const BYTE enabled[12] = {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const LSTATUS status =
        RegSetValueExW(key, kValueName, 0, REG_BINARY, enabled, sizeof(enabled));
    RegCloseKey(key);

    if (status != ERROR_SUCCESS) {
        EE_WARN(L"could not clear the startup block, status=%ld", status);
        return false;
    }
    EE_INFO(L"cleared the Startup apps block");
    return true;
}

}  // namespace ee
