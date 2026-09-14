#include "TrayHost.h"

#include <windowsx.h>

#include <cstring>
#include <cwchar>
#include <string>

#include "../core/DarkMode.h"
#include "../core/ExplorerSession.h"
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

// A dozen is a menu; more is a list that wants searching instead.
constexpr size_t kMaxRecentShown = 12;
constexpr UINT kDiagnosticsDelayMs = 3000;

}  // namespace

bool TrayHost::Start(HINSTANCE instance) {
    instance_ = instance;

    settings_ = LoadSettings();
    ApplyToConfig(settings_);
    Recents().Load();

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
        // The settings window is a window of controls rather than a dialog, so
        // Tab, the arrow keys, Space and Escape only work if it is offered the
        // message first.
        const HWND settings = settings_window_.Window();
        if (settings && IsDialogMessageW(settings, &msg)) continue;
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

    // Deliberately short. Everything that is a preference lives in the settings
    // window now; what is left is the master switch, the folders you might want
    // back, and the two ways out.
    AppendMenuW(menu, MF_STRING | (settings_.enabled ? MF_CHECKED : MF_UNCHECKED), IDM_ENABLED,
                L"Enabled");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendRecentMenu(menu);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    // A menu shown from a tray icon renders light unless the process says
    // otherwise, which looks wrong beside every other menu on a dark desktop.
    ApplyPreferredMenuTheme();
    AllowDarkModeForWindow(window_);

    // Required so the menu dismisses when the user clicks elsewhere.
    SetForegroundWindow(window_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window_, nullptr);
    PostMessageW(window_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void TrayHost::AppendRecentMenu(HMENU menu) {
    recent_.clear();
    if (!settings_.rememberRecentFolders) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Recent folders (off)");
        return;
    }

    // Built fresh each time the menu opens. Enumerating what is open is a
    // cross-process call, but this is a menu the user asked for by hand.
    recent_ = Recents().List(OpenFolders(), kMaxRecentShown);

    const HMENU submenu = CreatePopupMenu();
    if (!submenu) return;

    if (recent_.empty()) {
        // "Nothing yet" is a lie when the list is full and every one of them is
        // on screen, which is exactly the state after a busy morning.
        const bool anything_remembered = !Recents().List({}, 1).empty();
        AppendMenuW(submenu, MF_STRING | MF_GRAYED, 0,
                    anything_remembered ? L"All of them are open" : L"Nothing yet");
    } else {
        for (size_t i = 0; i < recent_.size(); ++i) {
            // Name first, then where it lives, right aligned: the same shape
            // as a menu with shortcut keys, and it reads as one column of
            // names rather than a wall of paths.
            std::wstring text = recent_[i].display;
            if (!recent_[i].context.empty()) text += L"\t" + recent_[i].context;
            AppendMenuW(submenu, MF_STRING, IDM_RECENT_FIRST + static_cast<UINT>(i), text.c_str());
        }
    }
    // Nothing else in here: forgetting the list and turning it off are both
    // settings, and this submenu is for going somewhere.

    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu),
                L"Recent folders\tCtrl: open in this tab");
}

void TrayHost::ShowSettings() {
    settings_window_.Show(instance_, window_, &settings_);
}

void TrayHost::PersistAndApply() {
    SaveSettings(settings_);
    ApplyToConfig(settings_);
    // Whichever surface made the change, the other one should not be showing
    // the old answer.
    settings_window_.Refresh();
}

void TrayHost::OnCommand(UINT id) {

    if (id >= IDM_RECENT_FIRST && id <= IDM_RECENT_LAST) {
        const size_t index = id - IDM_RECENT_FIRST;
        if (index >= recent_.size()) return;
        // Ctrl means "go there in the tab I am in" rather than opening a
        // window - the closest thing to reopening a tab that works from out
        // here, since the shell will not hand out its new-tab verb.
        const bool in_front_tab = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        PostMessageW(worker_.MessageWindow(), kMsgOpenFolder, in_front_tab ? 1 : 0,
                     reinterpret_cast<LPARAM>(new std::wstring(recent_[index].path)));
        return;
    }

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

        case IDM_FOLDER_COUNTS:
            settings_.folderItemCounts = !settings_.folderItemCounts;
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

        case IDM_MEDIA_AUTOPLAY:
            settings_.mediaAutoPlay = !settings_.mediaAutoPlay;
            PersistAndApply();
            break;

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

        case IDM_REMEMBER_RECENT:
            settings_.rememberRecentFolders = !settings_.rememberRecentFolders;
            PersistAndApply();
            break;

        case IDM_CLEAR_RECENT:
            Recents().Clear();
            Recents().Save();
            break;

        case IDM_SETTINGS:
            ShowSettings();
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
                // One right-click produces three notifications - button down,
                // button up, then WM_CONTEXTMENU - and answering two of them
                // showed the menu twice. The second TrackPopupMenu waited in
                // the queue while the first was up, so the menu reappeared the
                // instant a command was chosen and the first one closed: it
                // read as a menu that would not go away.
                //
                // WM_CONTEXTMENU alone is the one to answer. Version 4 sends it
                // for the keyboard's menu key as well, so nothing is lost.
                case WM_CONTEXTMENU:
                    self->ShowContextMenu();
                    return 0;

                // A left click on a tray icon is expected to open the thing.
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    self->ShowSettings();
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
