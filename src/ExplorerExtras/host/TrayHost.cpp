#include "TrayHost.h"

#include <windowsx.h>

#include <cstring>
#include <cwchar>
#include <string>

#include "../core/Logging.h"
#include "../core/Paths.h"
#include "../resource.h"
#include "AutoStart.h"

namespace ee {
namespace {

constexpr wchar_t kHostClassName[] = L"ExplorerExtras.Host";
constexpr wchar_t kAppTitle[] = L"Explorer Extras";
constexpr UINT kTrayCallbackMessage = WM_APP + 10;
constexpr UINT_PTR kDiagnosticsTimerId = 1;
constexpr UINT kDiagnosticsDelayMs = 3000;

}  // namespace

bool TrayHost::Start(HINSTANCE instance) {
    instance_ = instance;

    settings_ = LoadSettings();
    ApplyToConfig(settings_);

    // Make the registry match the saved preference. On a first run this is what
    // registers the app to start with Windows.
    if (IsAutoStartEnabled() != settings_.runAtStartup) {
        SetAutoStartEnabled(settings_.runAtStartup);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayHost::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kHostClassName;
    RegisterClassExW(&wc);

    // A real top-level window rather than HWND_MESSAGE: message-only windows do
    // not receive the TaskbarCreated broadcast, so the icon could never be
    // restored after Explorer restarts.
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClassName, kAppTitle, WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, instance, this);
    if (!window_) {
        EE_ERR(L"failed to create host window, gle=%lu", GetLastError());
        return false;
    }

    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");

    if (!worker_.Start()) return false;
    if (!hook_.Install(worker_.MessageWindow(), kMsgGesture)) return false;
    // Dormant until a tip is showing; see KeyboardCaptureEnabled().
    keyboard_hook_.Install(worker_.MessageWindow(), kMsgKey);

    AddTrayIcon();
    EE_INFO(L"host started (enabled=%d, navigateUp=%d, autoStart=%d)", settings_.enabled ? 1 : 0,
            settings_.navigateUpOnDoubleClick ? 1 : 0, settings_.runAtStartup ? 1 : 0);
    return true;
}

void TrayHost::Stop() {
    keyboard_hook_.Uninstall();
    hook_.Uninstall();
    worker_.Stop();
    RemoveTrayIcon();
    if (window_) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

int TrayHost::RunMessageLoop() {
    MSG msg;
    BOOL result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (result == -1) return 1;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void TrayHost::AddTrayIcon() {
    icon_ = {};
    icon_.cbSize = sizeof(icon_);
    icon_.hWnd = window_;
    icon_.uID = 1;
    icon_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    icon_.uCallbackMessage = kTrayCallbackMessage;
    icon_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
    if (!icon_.hIcon) icon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(icon_.szTip, kAppTitle);

    if (!Shell_NotifyIconW(NIM_ADD, &icon_)) {
        EE_WARN(L"Shell_NotifyIcon(NIM_ADD) failed, gle=%lu", GetLastError());
        return;
    }
    icon_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon_);
    icon_added_ = true;
}

void TrayHost::RemoveTrayIcon() {
    if (!icon_added_) return;
    Shell_NotifyIconW(NIM_DELETE, &icon_);
    icon_added_ = false;
}

void TrayHost::ShowBalloon(const wchar_t* title, const wchar_t* text) {
    if (!icon_added_) return;
    NOTIFYICONDATAW balloon = icon_;
    balloon.uFlags = NIF_INFO;
    balloon.dwInfoFlags = NIIF_INFO;
    wcscpy_s(balloon.szInfoTitle, title);
    wcscpy_s(balloon.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &balloon);
}

void TrayHost::ShowContextMenu() {
    POINT cursor{};
    GetCursorPos(&cursor);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING | (settings_.enabled ? MF_CHECKED : MF_UNCHECKED), IDM_ENABLED,
                L"Enabled");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
                MF_STRING | (settings_.navigateUpOnDoubleClick ? MF_CHECKED : MF_UNCHECKED) |
                    (settings_.enabled ? MF_ENABLED : MF_GRAYED),
                IDM_NAVIGATE_UP, L"Double-click empty space to go up");
    AppendMenuW(menu,
                MF_STRING | (settings_.subfolderTips ? MF_CHECKED : MF_UNCHECKED) |
                    (settings_.enabled ? MF_ENABLED : MF_GRAYED),
                IDM_SUBFOLDER_TIPS, L"Subfolder tips on hover");
    AppendMenuW(menu,
                MF_STRING | (settings_.filePreviews ? MF_CHECKED : MF_UNCHECKED) |
                    (settings_.enabled ? MF_ENABLED : MF_GRAYED),
                IDM_FILE_PREVIEWS, L"File previews on hover");
    AppendMenuW(menu,
                MF_STRING | (settings_.mediaPlayback ? MF_CHECKED : MF_UNCHECKED) |
                    (settings_.enabled && settings_.filePreviews ? MF_ENABLED : MF_GRAYED),
                IDM_MEDIA_PLAYBACK, L"Play audio and video in previews");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (settings_.runAtStartup ? MF_CHECKED : MF_UNCHECKED),
                IDM_RUN_AT_STARTUP, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, IDM_RESET_PREVIEWS, L"Reset file previews");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_LOG, L"Show log file in Explorer");
    AppendMenuW(menu, MF_STRING, IDM_COPY_LOG_PATH, L"Copy log path");
    AppendMenuW(menu, MF_STRING, IDM_DIAGNOSTICS, L"Log element under pointer\tin 3s");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    // Required so the menu dismisses when the user clicks elsewhere.
    SetForegroundWindow(window_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window_, nullptr);
    PostMessageW(window_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void TrayHost::PersistAndApply() {
    SaveSettings(settings_);
    ApplyToConfig(settings_);
}

void TrayHost::OnCommand(UINT id) {
    switch (id) {
        case IDM_ENABLED:
            settings_.enabled = !settings_.enabled;
            PersistAndApply();
            break;

        case IDM_NAVIGATE_UP:
            settings_.navigateUpOnDoubleClick = !settings_.navigateUpOnDoubleClick;
            PersistAndApply();
            break;

        case IDM_SUBFOLDER_TIPS:
            settings_.subfolderTips = !settings_.subfolderTips;
            PersistAndApply();
            break;

        case IDM_FILE_PREVIEWS:
            settings_.filePreviews = !settings_.filePreviews;
            PersistAndApply();
            break;

        case IDM_MEDIA_PLAYBACK:
            settings_.mediaPlayback = !settings_.mediaPlayback;
            PersistAndApply();
            break;

        case IDM_RUN_AT_STARTUP:
            settings_.runAtStartup = !settings_.runAtStartup;
            SetAutoStartEnabled(settings_.runAtStartup);
            PersistAndApply();
            break;

        case IDM_OPEN_LOG: {
            const std::wstring& path = log::FilePath();
            if (path.empty()) break;
            // Reveal the file rather than opening it in an editor. Opening it
            // relies on whatever handles .log, and a broken handler cannot be
            // detected: ShellExecute reports success (>32) even when the app
            // fails to start. Revealing it only needs Explorer, and lets the
            // user open it with whatever they actually have.
            const std::wstring args = L"/select,\"" + path + L"\"";
            ShellExecuteW(window_, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }

        case IDM_RESET_PREVIEWS:
            PostMessageW(worker_.MessageWindow(), kMsgResetPreview, 0, 0);
            ShowBalloon(kAppTitle, L"File previews reset. The next preview starts fresh.");
            break;

        case IDM_COPY_LOG_PATH: {
            // Guaranteed fallback: revealing the file depends on the shell
            // being able to resolve the folder, and the clipboard does not.
            const std::wstring& path = log::FilePath();
            if (path.empty() || !OpenClipboard(window_)) break;
            EmptyClipboard();
            const size_t bytes = (path.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                if (void* target = GlobalLock(memory)) {
                    memcpy(target, path.c_str(), bytes);
                    GlobalUnlock(memory);
                    if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
                } else {
                    GlobalFree(memory);
                }
            }
            CloseClipboard();
            ShowBalloon(kAppTitle, L"Log path copied to the clipboard.");
            break;
        }

        case IDM_DIAGNOSTICS:
            ShowBalloon(kAppTitle, L"Move the pointer over Explorer. Capturing in 3 seconds.");
            SetTimer(window_, kDiagnosticsTimerId, kDiagnosticsDelayMs, nullptr);
            break;

        case IDM_EXIT:
            DestroyWindow(window_);
            window_ = nullptr;
            break;

        default:
            break;
    }
}

LRESULT CALLBACK TrayHost::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wparam, lparam);
    }

    auto* self = reinterpret_cast<TrayHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) return DefWindowProcW(window, message, wparam, lparam);

    if (message == self->taskbar_created_message_ && self->taskbar_created_message_ != 0) {
        self->icon_added_ = false;
        self->AddTrayIcon();
        return 0;
    }

    switch (message) {
        case kTrayCallbackMessage:
            // With NOTIFYICON_VERSION_4 the event id arrives in the low word.
            switch (LOWORD(lparam)) {
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP:
                    self->ShowContextMenu();
                    return 0;
                default:
                    return 0;
            }

        case WM_COMMAND:
            self->OnCommand(LOWORD(wparam));
            return 0;

        case WM_TIMER:
            if (wparam == kDiagnosticsTimerId) {
                KillTimer(window, kDiagnosticsTimerId);
                POINT cursor{};
                GetCursorPos(&cursor);
                self->worker_.RequestDiagnostics(cursor);
                self->ShowBalloon(kAppTitle, L"Captured. See the log file.");
            }
            return 0;

        case WM_DESTROY:
            self->RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

}  // namespace ee
