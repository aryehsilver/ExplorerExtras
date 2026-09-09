#include "DarkMode.h"

#include "ShellItems.h"  // AppsUseDarkTheme

namespace ee {
namespace {

enum class PreferredAppMode {
    Default = 0,
    AllowDark = 1,
    ForceDark = 2,
    ForceLight = 3,
};

using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI*)();
using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);

SetPreferredAppModeFn g_set_mode = nullptr;
FlushMenuThemesFn g_flush = nullptr;
AllowDarkModeForWindowFn g_allow_for_window = nullptr;

void Resolve() {
    static bool resolved = false;
    if (resolved) return;
    resolved = true;

    // Never freed on purpose: uxtheme stays loaded for the life of the process
    // anyway, and the pointers must outlive every menu we show.
    const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;

    // Ordinals, because these exports have no names.
    g_set_mode =
        reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    g_flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    g_allow_for_window =
        reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
}

}  // namespace

void ApplyPreferredMenuTheme() {
    Resolve();
    if (!g_set_mode) return;

    // Read the setting each time rather than caching it, so switching theme
    // while running is picked up by the next menu.
    g_set_mode(AppsUseDarkTheme() ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight);
    if (g_flush) g_flush();
}

void AllowDarkModeForWindow(HWND window) {
    Resolve();
    if (g_allow_for_window && window) g_allow_for_window(window, AppsUseDarkTheme());
}

}  // namespace ee
