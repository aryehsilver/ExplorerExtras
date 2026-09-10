// ExplorerSession.h - talking to a live Explorer window through the shell.
//
// Windows 11 gives every tab its own entry in IShellWindows, and all of a
// window's tabs report the same top-level HWND. Each tab is a distinct
// ShellTabWindowClass child, and that is what IShellBrowser::GetWindow returns,
// so it is the key used to pick the tab the pointer is actually over.
//
// Everything here is cross-process COM and must run on an STA thread; from an
// MTA the calls return S_OK but hand back null handles.
#pragma once

#include <windows.h>
// See ViewHitTest.h: WIN32_LEAN_AND_MEAN means windows.h does not define the
// `interface` macro that the shell headers need.
#include <objbase.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <vector>

namespace ee {

struct ActiveTab {
    Microsoft::WRL::ComPtr<IShellBrowser> browser;
    HWND def_view = nullptr;    // SHELLDLL_DefView of this tab
    HWND tab_window = nullptr;  // ShellTabWindowClass of this tab
    bool matched_pointer_tab = false;
};

// True for a File Explorer frame window (not the desktop, not a common dialog).
bool IsExplorerWindow(HWND top_level);

// Nearest ShellTabWindowClass ancestor of |from|, or null if there is none.
HWND FindTabWindow(HWND from);

// Resolves the browser for the tab identified by |tab_window|. When
// |tab_window| is null (a shell with no tab windows) it falls back to the
// single visible view. A non-null |tab_window| that matches nothing yields
// nullopt rather than a guess - doing nothing beats driving the wrong tab.
std::optional<ActiveTab> ResolveActiveTab(HWND top_level, HWND tab_window);

// Parsing path of the tab's current folder.
std::wstring GetCurrentFolder(const ActiveTab& tab);

// The folder each of |top_level|'s tabs is showing, the frontmost first.
//
// For a hover on the frame's own chrome - the address bar, the tab strip -
// there is no tab window under the pointer to identify which tab it belongs
// to, and Windows is no help: with three tabs open, all three tab windows and
// all three views report themselves visible. Z-order is the only ordering on
// offer, so the answer is "probably the first, but here is the rest".
std::vector<std::wstring> TabFolders(HWND top_level);

// Folder shown by the tab labelled |tab_name| in |top_level|. Tabs are matched
// on their label, which is the folder's leaf name; where two tabs show
// same-named folders the first is taken.
std::wstring FindTabFolder(HWND top_level, const std::wstring& tab_name);

// Every tab of |top_level| as the shell reports it, for the diagnostics dump:
// which window each entry owns, which of them Windows calls visible, and what
// folder each is showing. This is the view that decides which tab a hover on
// the frame's own chrome belongs to.
std::wstring DescribeTabs(HWND top_level);

// Browses to the parent folder in place. Fails harmlessly at a namespace root.
HRESULT NavigateUp(const ActiveTab& tab);

// Opens |path| as a new tab in |tab|'s window.
//
// The shell registers an "opennewtab" verb, but it is marked
// OnlyInBrowserWindow: invoked bare through ShellExecute it has no browser to
// add a tab to and degrades to opening a whole new window. Giving the verb the
// tab's IShellBrowser as its site is what makes it produce a tab.
HRESULT OpenInNewTab(const ActiveTab& tab, const std::wstring& path);

}  // namespace ee
