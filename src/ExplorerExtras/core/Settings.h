// Settings.h - persisted user options plus the lock-free view the hot paths read.
#pragma once

#include <atomic>

namespace ee {

struct Settings {
    bool enabled = true;
    bool navigateUpOnDoubleClick = true;
    bool subfolderTips = true;
    bool filePreviews = true;
    bool mediaPlayback = true;
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
    std::atomic<bool> filePreviews{true};
    std::atomic<bool> mediaPlayback{true};
};

RuntimeConfig& Config();
void ApplyToConfig(const Settings& settings);

}  // namespace ee
