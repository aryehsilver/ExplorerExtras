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
    // Immediate children, not recursive. counted is false where we did not
    // look (a network path); capped is true where we stopped early, so the
    // figures shown are a floor rather than a total.
    bool counted = false;
    bool capped = false;
    int child_folders = 0;
    int child_files = 0;
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

// An icon that has not been looked up yet. Whoever draws the row resolves it.
inline constexpr int kIconDeferred = -2;

// Folders first, then files, each in Explorer's natural sort order. Stops after
// |max_entries|; |truncated| reports whether more existed.
//
// With |defer_icons|, rows come back marked kIconDeferred instead: an icon
// costs a file open, and for an executable an icon resource is parsed out of
// it, so a folder of binaries takes seconds to read. Worth it when the listing
// is far larger than the part that will ever be drawn.
std::vector<ShellEntry> EnumerateFolder(const std::wstring& folder_path, size_t max_entries,
                                        bool* truncated, bool defer_icons = false);

// The row's icon, for a listing that deferred them.
int IconIndexFor(const std::wstring& path);

// The shared system small-icon image list. Never destroy it.
// The count annotation for a folder row: "3 folders, 12 files", or
// "2000+ items" where counting was cut short. Empty for files, for folders we
// did not count, and for empty folders.
std::wstring DescribeChildCount(const ShellEntry& entry);

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
