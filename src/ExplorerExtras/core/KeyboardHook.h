// KeyboardHook.h - arrow-key control of the hover tip.
//
// The tip never takes focus, so it receives no keyboard input of its own. This
// hook borrows just the navigation keys while a tip is on screen and hands them
// to the worker; every other key, and every key at all when no tip is showing,
// passes through untouched.
#pragma once

#include <windows.h>

#include <atomic>

namespace ee {

// Gate for the hook. Only while this is true is anything swallowed.
std::atomic<bool>& KeyboardCaptureEnabled();

class KeyboardHook {
public:
    KeyboardHook() = default;
    ~KeyboardHook();

    KeyboardHook(const KeyboardHook&) = delete;
    KeyboardHook& operator=(const KeyboardHook&) = delete;

    // Install on a thread with a message loop. Captured keys are posted to
    // |target| as |message| with wParam = virtual key code.
    bool Install(HWND target, UINT message);
    void Uninstall();

private:
    static LRESULT CALLBACK HookProc(int code, WPARAM wparam, LPARAM lparam);

    static KeyboardHook* instance_;

    HHOOK hook_ = nullptr;
    HWND target_ = nullptr;
    UINT message_ = 0;
    bool swallowed_[256]{};
};

}  // namespace ee
