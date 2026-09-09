// MouseHook.h - global low-level mouse hook.
//
// The hook lives in this process only; nothing is injected into explorer.exe.
// The callback runs on the input path, so it does nothing but arithmetic:
// it recognises the gesture and posts it to another window for handling.
// It never swallows an event - CallNextHookEx is always invoked.
#pragma once

#include <windows.h>

namespace ee {

enum class Gesture {
    LeftClick,        // every press, so open tips can dismiss themselves
    LeftDoubleClick,
};

// Posted as lParam. The receiving window takes ownership and deletes it.
struct GestureEvent {
    POINT pt{};          // physical screen coordinates
    Gesture gesture{};
};

class MouseHook {
public:
    MouseHook() = default;
    ~MouseHook();

    MouseHook(const MouseHook&) = delete;
    MouseHook& operator=(const MouseHook&) = delete;

    // Installs the hook on the calling thread, which must run a message loop.
    // Recognised gestures are posted to |target| as |message| with
    // lParam = GestureEvent*.
    bool Install(HWND target, UINT message);
    void Uninstall();
    bool IsInstalled() const { return hook_ != nullptr; }

private:
    static LRESULT CALLBACK HookProc(int code, WPARAM wparam, LPARAM lparam);
    void OnLeftButtonDown(const MSLLHOOKSTRUCT& info);
    void Post(POINT pt, Gesture gesture);

    static MouseHook* instance_;

    HHOOK hook_ = nullptr;
    HWND target_ = nullptr;
    UINT message_ = 0;

    // Double-click recognition state.
    DWORD last_down_time_ = 0;
    POINT last_down_pt_{};
    bool click_pending_ = false;
};

}  // namespace ee
