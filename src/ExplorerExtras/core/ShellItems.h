// ShellItems.h - reading folder contents for the subfolder tip.
//
// STA thread only, like everything else that touches the shell.
#pragma once

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>

#include <string>
#include <vector>

namespace ee {

struct ShellEntry {
    std::wstring display_name;
    std::wstring parsing_path;
    bool is_folder = false;
    // Folders only. An empty folder gets no expand chevron, because offering to
    // open something with nothing in it is a lie.
    bool has_children = false;
    int icon_index = -1;  // index into SystemSmallImageList()
};

// Resolves |child_display_name| inside |folder_path|, reporting its full path
// and whether it is a folder. Fails for names that do not round-trip, which is
// why a file whose extension Explorer hides simply gets no preview rather than
// the wrong one.
bool ResolveChild(const std::wstring& folder_path, const std::wstring& child_display_name,
                  std::wstring* child_path, bool* is_folder);

// Convenience wrapper: succeeds only for folders.
bool ResolveChildFolder(const std::wstring& folder_path, const std::wstring& child_display_name,
                        std::wstring* child_path);

// Folders first, then files, each in Explorer's natural sort order. Stops after
// |max_entries|; |truncated| reports whether more existed.
std::vector<ShellEntry> EnumerateFolder(const std::wstring& folder_path, size_t max_entries,
                                        bool* truncated);

// The shared system small-icon image list. Never destroy it.
// Shell thumbnail for a file, or null when it has none. Caller owns the bitmap.
// Synchronous, so only for things known to be quick — embedded album art is,
// video frames are not (those go through ThumbnailLoader).
HBITMAP LoadThumbnail(const std::wstring& path, int max_edge);

// One line of facts for the preview footer: size, created and modified, in the
// user's own locale formats. Empty when the file cannot be read.
std::wstring FileFactsLine(const std::wstring& path);

HIMAGELIST SystemSmallImageList();

bool AppsUseDarkTheme();

}  // namespace ee
