// ShellMenu.h - Explorer's own context menu and drag-and-drop, for our lists.
//
// Both go through the item's IContextMenu / IDataObject, so what the user gets
// is the real shell menu with every installed extension in it, and a real OLE
// drag that any drop target understands.
//
// Requires OleInitialize on the calling thread, not just CoInitializeEx.
#pragma once

#include <windows.h>

#include <string>

namespace ee {

// Shows the shell context menu for |path| at |screen_pt| and invokes whatever
// the user picks. Blocks until the menu closes.
void ShowShellContextMenu(HWND owner, const std::wstring& path, POINT screen_pt);

// Starts an OLE drag of |path|. Blocks for the duration of the drag.
void DragShellItem(HWND owner, const std::wstring& path);

}  // namespace ee
