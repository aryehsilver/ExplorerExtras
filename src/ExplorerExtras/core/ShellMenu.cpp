#include "ShellMenu.h"

#include <objbase.h>
#include <shellapi.h>  // CMIC_MASK_* expand to SEE_MASK_*
#include <shlobj.h>
#include <wrl/client.h>

#include "DarkMode.h"
#include "Logging.h"

using Microsoft::WRL::ComPtr;

namespace ee {
namespace {

constexpr wchar_t kMenuOwnerClass[] = L"ExplorerExtras.MenuOwner";
constexpr UINT kFirstCommandId = 1;

// The shell's own menu extensions draw themselves, and they need their owner
// window to forward the ownerdraw messages back to them.
IContextMenu2* g_menu2 = nullptr;
IContextMenu3* g_menu3 = nullptr;

LRESULT CALLBACK MenuOwnerProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_INITMENUPOPUP:
        case WM_DRAWITEM:
        case WM_MEASUREITEM:
        case WM_MENUCHAR:
            if (g_menu3) {
                LRESULT result = 0;
                if (SUCCEEDED(g_menu3->HandleMenuMsg2(message, wparam, lparam, &result))) {
                    return result;
                }
            } else if (g_menu2) {
                if (SUCCEEDED(g_menu2->HandleMenuMsg(message, wparam, lparam))) return 0;
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND CreateMenuOwner() {
    static bool registered = false;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = MenuOwnerProc;
        wc.hInstance = instance;
        wc.lpszClassName = kMenuOwnerClass;
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) return nullptr;
    }
    return CreateWindowExW(WS_EX_TOOLWINDOW, kMenuOwnerClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr,
                           nullptr, instance, nullptr);
}

// Binds |path| to its parent folder and asks for |riid| on the item.
HRESULT ItemUIObject(const std::wstring& path, REFIID riid, void** out) {
    *out = nullptr;

    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr);
    if (FAILED(hr)) return hr;

    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    hr = SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child);
    if (SUCCEEDED(hr)) {
        hr = parent->GetUIObjectOf(nullptr, 1, &child, riid, nullptr, out);
    }

    CoTaskMemFree(pidl);
    return hr;
}

}  // namespace

void ShowShellContextMenu(HWND owner, const std::wstring& path, POINT screen_pt) {
    if (path.empty()) return;

    // Before the menu is built, so its items are themed like Explorer's own.
    ApplyPreferredMenuTheme();

    ComPtr<IContextMenu> menu;
    if (FAILED(ItemUIObject(path, IID_IContextMenu, reinterpret_cast<void**>(menu.GetAddressOf()))) ||
        !menu) {
        return;
    }

    const HMENU popup = CreatePopupMenu();
    if (!popup) return;

    if (FAILED(menu->QueryContextMenu(popup, 0, kFirstCommandId, 0x7FFF, CMF_NORMAL))) {
        DestroyMenu(popup);
        return;
    }

    const HWND menu_owner = CreateMenuOwner();
    if (!menu_owner) {
        DestroyMenu(popup);
        return;
    }
    AllowDarkModeForWindow(menu_owner);

    // Raw pointers, because the owner window proc reaches them from a callback.
    menu->QueryInterface(IID_PPV_ARGS(&g_menu2));
    menu->QueryInterface(IID_PPV_ARGS(&g_menu3));

    // A popup menu only behaves - dismissing on an outside click - when its
    // owner is the foreground window.
    SetForegroundWindow(menu_owner);
    const int command = TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_pt.x,
                                         screen_pt.y, menu_owner, nullptr);
    PostMessageW(menu_owner, WM_NULL, 0, 0);

    if (g_menu2) {
        g_menu2->Release();
        g_menu2 = nullptr;
    }
    if (g_menu3) {
        g_menu3->Release();
        g_menu3 = nullptr;
    }

    if (command > 0) {
        CMINVOKECOMMANDINFOEX info{};
        info.cbSize = sizeof(info);
        info.fMask = CMIC_MASK_UNICODE;
        info.hwnd = owner;
        info.lpVerb = MAKEINTRESOURCEA(command - kFirstCommandId);
        info.lpVerbW = MAKEINTRESOURCEW(command - kFirstCommandId);
        info.nShow = SW_SHOWNORMAL;
        const HRESULT hr = menu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
        if (FAILED(hr)) EE_INFO(L"context menu command failed hr=0x%08X", hr);
    }

    DestroyMenu(popup);
    DestroyWindow(menu_owner);
}

void DragShellItem(HWND owner, const std::wstring& path) {
    if (path.empty()) return;

    ComPtr<IDataObject> data;
    if (FAILED(ItemUIObject(path, IID_IDataObject,
                            reinterpret_cast<void**>(data.GetAddressOf()))) ||
        !data) {
        return;
    }

    // SHDoDragDrop supplies the drag image and a default drop source, so the
    // drag looks and behaves exactly like one started from Explorer.
    DWORD effect = DROPEFFECT_NONE;
    SHDoDragDrop(owner, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK,
                 &effect);
}

}  // namespace ee
