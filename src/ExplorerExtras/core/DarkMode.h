// DarkMode.h - makes a shell context menu follow the user's theme.
//
// Menus shown from an ordinary Win32 process render light regardless of the
// system setting; the dark menus seen inside Explorer come from the shell
// opting its own process in. There is no documented API for this, so it goes
// through uxtheme's undocumented ordinals.
//
// This is deliberately limited to appearance. Every lookup is guarded and every
// call optional, so on a build where the ordinals move or vanish the menus
// simply render light - nothing breaks, and no behaviour depends on it.
#pragma once

#include <windows.h>

namespace ee {

// Matches the process's menu rendering to the current apps theme. Cheap enough
// to call before each menu, which is also what picks up a theme change.
void ApplyPreferredMenuTheme();

// Opts one window into dark rendering, for the window that owns the menu.
void AllowDarkModeForWindow(HWND window);

}  // namespace ee
