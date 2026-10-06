// TrayHost.h - the standalone host: tray icon, settings menu, process lifetime.
//
// The only host-specific layer. It owns the hook and the worker; the features
// below it know nothing about how they were started.
#pragma once

#include <windows.h>
#include <shellapi.h>

#include <vector>

#include "../core/KeyboardHook.h"
#include "../core/MouseHook.h"
#include "../core/RecentFolders.h"
#include "../core/Settings.h"
#include "../core/Watchdog.h"
#include "../core/Worker.h"
#include "../ui/SettingsWindow.h"

namespace ee {

class TrayHost {
public:
    // |replacement| is true for a copy the watchdog started in place of one
    // that hung.
    bool Start(HINSTANCE instance, bool replacement);
    void Stop();
    int RunMessageLoop();

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void AppendRecentMenu(HMENU menu);

    void OnCommand(UINT id);
    void ShowContextMenu();
    void ShowSettings();
    // Makes Windows' startup entry say what the settings say, and says so in
    // the log either way.
    void ReconcileAutoStart();
    void UpdateStartupNote();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowBalloon(const wchar_t* title, const wchar_t* text);
    void PersistAndApply();
    // On the watchdog's thread, with one of ours stuck.
    void RestartAfterHang(const wchar_t* thread_name);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    UINT taskbar_created_message_ = 0;
    NOTIFYICONDATAW icon_{};
    bool icon_added_ = false;

    Settings settings_;
    Worker worker_;
    SettingsWindow settings_window_;

    // Rebuilt every time the tray menu opens; the command ids index into it.
    std::vector<RecentFolder> recent_;
    MouseHook hook_;
    KeyboardHook keyboard_hook_;

    Watchdog watchdog_;
    bool replacement_ = false;
    ULONGLONG started_at_ = 0;
};

}  // namespace ee
