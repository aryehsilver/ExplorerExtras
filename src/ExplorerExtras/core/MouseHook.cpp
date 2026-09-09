#include "MouseHook.h"

#include <cstdlib>
#include <new>

#include "Logging.h"
#include "Settings.h"

namespace ee {

MouseHook* MouseHook::instance_ = nullptr;

MouseHook::~MouseHook() {
    Uninstall();
}

bool MouseHook::Install(HWND target, UINT message) {
    if (hook_) return true;
    target_ = target;
    message_ = message;
    instance_ = this;

    hook_ = SetWindowsHookExW(WH_MOUSE_LL, &MouseHook::HookProc, GetModuleHandleW(nullptr), 0);
    if (!hook_) {
        EE_ERR(L"SetWindowsHookExW(WH_MOUSE_LL) failed, gle=%lu", GetLastError());
        instance_ = nullptr;
        return false;
    }
    EE_INFO(L"mouse hook installed");
    return true;
}

void MouseHook::Uninstall() {
    if (!hook_) return;
    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    instance_ = nullptr;
    EE_INFO(L"mouse hook removed");
}

LRESULT CALLBACK MouseHook::HookProc(int code, WPARAM wparam, LPARAM lparam) {
    // Always pass the event on, whatever happens below. Explorer must see the
    // click exactly as the user made it.
    if (code == HC_ACTION && wparam == WM_LBUTTONDOWN && instance_) {
        instance_->OnLeftButtonDown(*reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam));
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void MouseHook::Post(POINT pt, Gesture gesture) {
    auto* event = new (std::nothrow) GestureEvent{pt, gesture};
    if (!event) return;
    if (!PostMessageW(target_, message_, 0, reinterpret_cast<LPARAM>(event))) {
        delete event;
    }
}

void MouseHook::OnLeftButtonDown(const MSLLHOOKSTRUCT& info) {
    const DWORD now = info.time;
    const int dx = std::abs(info.pt.x - last_down_pt_.x);
    const int dy = std::abs(info.pt.y - last_down_pt_.y);

    // SM_C*DOUBLECLK is the full width/height of the allowed rectangle, so the
    // permitted movement from the first click is half of it.
    const bool in_slop = dx * 2 <= GetSystemMetrics(SM_CXDOUBLECLK) &&
                         dy * 2 <= GetSystemMetrics(SM_CYDOUBLECLK);
    const bool in_time = (now - last_down_time_) <= GetDoubleClickTime();

    if (!Config().enabled.load(std::memory_order_relaxed)) return;

    // Every press, so anything on screen can decide to get out of the way.
    Post(info.pt, Gesture::LeftClick);

    if (click_pending_ && in_time && in_slop) {
        // Consume the pair so a triple-click does not fire a second time.
        click_pending_ = false;
        Post(info.pt, Gesture::LeftDoubleClick);
        return;
    }

    click_pending_ = true;
    last_down_time_ = now;
    last_down_pt_ = info.pt;
}

}  // namespace ee
