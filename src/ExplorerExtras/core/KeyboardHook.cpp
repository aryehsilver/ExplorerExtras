#include "KeyboardHook.h"

#include "Logging.h"

namespace ee {
namespace {

bool IsNavigationKey(DWORD vk) {
    switch (vk) {
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_RETURN:
        case VK_ESCAPE:
            return true;
        default:
            return false;
    }
}

// Anything that can narrow a list of file names: letters, digits, and the
// punctuation that turns up in them. Backspace rubs out the last one.
bool IsFilterKey(DWORD vk) {
    if (vk >= 'A' && vk <= 'Z') return true;
    if (vk >= '0' && vk <= '9') return true;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return true;
    switch (vk) {
        case VK_SPACE:
        case VK_BACK:
        case VK_OEM_1:
        case VK_OEM_2:
        case VK_OEM_3:
        case VK_OEM_4:
        case VK_OEM_5:
        case VK_OEM_6:
        case VK_OEM_7:
        case VK_OEM_PLUS:
        case VK_OEM_COMMA:
        case VK_OEM_MINUS:
        case VK_OEM_PERIOD:
            return true;
        default:
            return false;
    }
}

// A tip can be on screen while the user is working in another application -
// hovering does not move focus. Only borrow keys when Explorer actually has
// focus, or we would steal the arrow keys from whatever they are typing in.
bool ExplorerHasFocus() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    wchar_t cls[64]{};
    if (GetClassNameW(foreground, cls, ARRAYSIZE(cls)) == 0) return false;
    return CompareStringOrdinal(cls, -1, L"CabinetWClass", -1, FALSE) == CSTR_EQUAL ||
           CompareStringOrdinal(cls, -1, L"ExploreWClass", -1, FALSE) == CSTR_EQUAL;
}

// A shortcut is never ours - Ctrl+C, Alt+Left and friends belong to Explorer.
bool ShortcutModifierHeld() {
    return (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000) ||
           (GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000);
}

// Shift matters for the arrow keys, which Explorer uses to extend a selection,
// but not for typing: the filter is matched case insensitively either way.
bool AnyModifierHeld() {
    return ShortcutModifierHeld() || (GetKeyState(VK_SHIFT) & 0x8000);
}

}  // namespace

std::atomic<bool>& KeyboardCaptureEnabled() {
    static std::atomic<bool> enabled{false};
    return enabled;
}

KeyboardHook* KeyboardHook::instance_ = nullptr;

KeyboardHook::~KeyboardHook() {
    Uninstall();
}

bool KeyboardHook::Install(HWND target, UINT message) {
    if (hook_) return true;
    target_ = target;
    message_ = message;
    instance_ = this;

    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &KeyboardHook::HookProc, GetModuleHandleW(nullptr), 0);
    if (!hook_) {
        EE_ERR(L"SetWindowsHookExW(WH_KEYBOARD_LL) failed, gle=%lu", GetLastError());
        instance_ = nullptr;
        return false;
    }
    EE_INFO(L"keyboard hook installed");
    return true;
}

void KeyboardHook::Uninstall() {
    if (!hook_) return;
    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    instance_ = nullptr;
    KeyboardCaptureEnabled().store(false, std::memory_order_relaxed);
    EE_INFO(L"keyboard hook removed");
}

LRESULT CALLBACK KeyboardHook::HookProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && instance_) {
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        const DWORD vk = info->vkCode;

        if (vk < 256) {
            const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
            const bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;

            const bool ours = (IsNavigationKey(vk) && !AnyModifierHeld()) ||
                              (IsFilterKey(vk) && !ShortcutModifierHeld());

            if (down && ours && KeyboardCaptureEnabled().load(std::memory_order_relaxed) &&
                ExplorerHasFocus()) {
                instance_->swallowed_[vk] = true;
                PostMessageW(instance_->target_, instance_->message_, vk, 0);
                return 1;  // swallow: Explorer must not also move its selection
            }

            // Release the matching key-up so applications never see a dangling
            // press for a key whose down we consumed.
            if (up && instance_->swallowed_[vk]) {
                instance_->swallowed_[vk] = false;
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

}  // namespace ee
