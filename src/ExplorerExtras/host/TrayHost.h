// TrayHost.h - the standalone host: tray icon, settings menu, process lifetime.
//
// This is the only layer that would be replaced by a PowertoyModuleIface
// implementation. It owns the hook and the worker; the features below it know
// nothing about how they were started.
#pragma once

#include <windows.h>
#include <shellapi.h>

#include "../core/KeyboardHook.h"
#include "../core/MouseHook.h"
#include "../core/Settings.h"
#include "../core/Worker.h"

namespace ee {

class TrayHost {
public:
    bool Start(HINSTANCE instance);
    void Stop();
    int RunMessageLoop();

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    void OnCommand(UINT id);
    void ShowContextMenu();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowBalloon(const wchar_t* title, const wchar_t* text);
    void PersistAndApply();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    UINT taskbar_created_message_ = 0;
    NOTIFYICONDATAW icon_{};
    bool icon_added_ = false;

    Settings settings_;
    Worker worker_;
    MouseHook hook_;
    KeyboardHook keyboard_hook_;
};

}  // namespace ee
