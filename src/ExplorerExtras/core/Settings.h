// Settings.h - persisted user options plus the lock-free view the hot paths read.
#pragma once

#include <atomic>

namespace ee {

struct Settings {
    bool enabled = true;
    bool navigateUpOnDoubleClick = true;
    bool subfolderTips = true;
    // Annotate folder rows with how many items they hold.
    bool folderItemCounts = true;
    bool filePreviews = true;
    bool mediaPlayback = true;
    // Only meaningful when mediaPlayback is on: whether hovering starts
    // playback, or opens the preview paused waiting for the play button.
    bool mediaAutoPlay = true;
    bool runAtStartup = true;
};

Settings LoadSettings();
void SaveSettings(const Settings& settings);

// Mirrors Settings for threads that must not touch the registry or disk.
// The mouse hook reads these on the input path, so they are atomics rather
// than a lock.
struct RuntimeConfig {
    std::atomic<bool> enabled{true};
    std::atomic<bool> navigateUpOnDoubleClick{true};
    std::atomic<bool> subfolderTips{true};
    std::atomic<bool> folderItemCounts{true};
    std::atomic<bool> filePreviews{true};
    std::atomic<bool> mediaPlayback{true};
    std::atomic<bool> mediaAutoPlay{true};
};

RuntimeConfig& Config();
void ApplyToConfig(const Settings& settings);

}  // namespace ee
