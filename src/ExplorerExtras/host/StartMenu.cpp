#include "StartMenu.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <string>

#include "../core/Logging.h"
#include "../core/Paths.h"

namespace ee {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kShortcutName[] = L"Explorer Extras.lnk";
constexpr wchar_t kDescription[] = L"Subfolder tips, previews and recent folders for File Explorer";

std::wstring ShortcutPath() {
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, &programs))) {
        return {};
    }
    std::wstring path = std::wstring(programs) + L"\\" + kShortcutName;
    CoTaskMemFree(programs);
    return path;
}

// What the shortcut opens; empty when there is none, or it cannot be read.
std::wstring TargetOf(const std::wstring& shortcut) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&file)) || FAILED(file->Load(shortcut.c_str(), STGM_READ))) {
        return {};
    }
    wchar_t target[MAX_PATH]{};
    if (FAILED(link->GetPath(target, ARRAYSIZE(target), nullptr, SLGP_RAWPATH))) return {};
    return target;
}

bool Write(const std::wstring& shortcut, const std::wstring& exe) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&file))) {
        return false;
    }
    const size_t slash = exe.find_last_of(L'\\');
    link->SetPath(exe.c_str());
    if (slash != std::wstring::npos) link->SetWorkingDirectory(exe.substr(0, slash).c_str());
    link->SetDescription(kDescription);
    link->SetIconLocation(exe.c_str(), 0);
    if (FAILED(file->Save(shortcut.c_str(), TRUE))) return false;
    // So the Start menu picks it up now rather than on its next look round.
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, shortcut.c_str(), nullptr);
    return true;
}

}  // namespace

bool EnsureStartMenuShortcut(bool running_from_temp, bool made_before) {
    if (running_from_temp) return made_before;

    const std::wstring shortcut = ShortcutPath();
    if (shortcut.empty()) return made_before;
    const std::wstring& exe = ExecutablePath();

    if (GetFileAttributesW(shortcut.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (made_before) return true;  // removed by hand: that is an answer
        if (!Write(shortcut, exe)) {
            EE_WARN(L"start menu: could not create '%s'", shortcut.c_str());
            return false;
        }
        EE_INFO(L"start menu: created '%s'", shortcut.c_str());
        return true;
    }

    // There already, made here or by hand - either way it should open this copy.
    const std::wstring target = TargetOf(shortcut);
    if (CompareStringOrdinal(target.c_str(), -1, exe.c_str(), -1, TRUE) != CSTR_EQUAL) {
        if (Write(shortcut, exe)) {
            EE_INFO(L"start menu: repointed from '%s'", target.c_str());
        } else {
            EE_WARN(L"start menu: could not repoint '%s'", shortcut.c_str());
        }
    }
    return true;
}

}  // namespace ee
