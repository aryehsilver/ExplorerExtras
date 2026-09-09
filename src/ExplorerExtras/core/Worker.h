// Worker.h - the STA thread that owns all COM and UI Automation work.
//
// The mouse hook runs on the input path and must return immediately, so it only
// recognises a gesture and posts it here. Nothing that can block ever runs on
// the hook thread.
#pragma once

#include <windows.h>

#include "../features/NavigateUpFeature.h"
#include "../features/SubfolderTipFeature.h"
#include "ViewHitTest.h"

namespace ee {

inline constexpr UINT kMsgGesture = WM_APP + 1;      // lParam = GestureEvent*
inline constexpr UINT kMsgDiagnostics = WM_APP + 2;  // lParam = POINT*
inline constexpr UINT kMsgKey = WM_APP + 3;          // wParam = virtual key code
inline constexpr UINT kMsgThumbnail = WM_APP + 4;    // wParam = token, lParam = HBITMAP

class Worker {
public:
    Worker() = default;
    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool Start();
    void Stop();

    // Gesture events are posted here by the hook.
    HWND MessageWindow() const { return window_; }

    // Dumps the UI Automation ancestor chain at |pt| to the log.
    void RequestDiagnostics(POINT pt);

private:
    static DWORD WINAPI ThreadMain(LPVOID param);
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void Run();
    void OnTick();

    HANDLE thread_ = nullptr;
    HANDLE ready_ = nullptr;
    DWORD thread_id_ = 0;
    HWND window_ = nullptr;

    ViewHitTester hit_tester_;
    NavigateUpFeature navigate_up_;
    SubfolderTipFeature subfolder_tip_;

    POINT last_tick_pt_{};
    DWORD still_ms_ = 0;
};

}  // namespace ee
