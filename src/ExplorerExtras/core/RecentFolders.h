// RecentFolders.h - where you have been, so you can go back.
//
// Explorer remembers recent *files* and pinned folders; it does not offer the
// folders you actually had open, and a closed tab is gone. This keeps a list of
// them, most recent first, and the tray menu offers it.
//
// Written to and read from two threads - the worker notices folders, the tray
// builds a menu out of them - so every method takes the lock. The list is small
// and touched a few times a minute; a mutex is the whole story.
#pragma once

#include <string>
#include <vector>

namespace ee {

struct RecentFolder {
    std::wstring path;     // parsing path, what gets opened
    std::wstring display;  // the folder's own name
    std::wstring context;  // where it lives, shortened for a menu
};

class RecentFolders {
public:
    // Pushes |path| to the front, deduplicated. A folder already at the front
    // is not written again, which is the common case while polling.
    void Note(const std::wstring& path);

    // Most recent first, at most |limit|, leaving out anything in |open_now|:
    // offering a folder that is already on screen is noise.
    std::vector<RecentFolder> List(const std::vector<std::wstring>& open_now, size_t limit) const;

    void Clear();

    // Beside the settings, in the same folder. Load on startup, save whenever
    // the list changed - it is a few hundred bytes.
    void Load();
    void Save();

private:
    mutable std::vector<std::wstring> paths_;  // most recent first
    bool dirty_ = false;
};

// The one list. Lives as long as the process.
RecentFolders& Recents();

}  // namespace ee
