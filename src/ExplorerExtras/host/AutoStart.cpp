#include "AutoStart.h"

#include <windows.h>

#include <string>

#include "../core/Logging.h"
#include "../core/Paths.h"

namespace ee {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"ExplorerExtras";

}  // namespace

bool IsAutoStartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status = RegQueryValueExW(key, kValueName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
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
    EE_INFO(L"auto-start %s", enabled ? L"enabled" : L"disabled");
    return true;
}

}  // namespace ee
