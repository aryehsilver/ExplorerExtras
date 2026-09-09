#include "Settings.h"

#include <windows.h>

#include <string>

#include "Paths.h"

namespace ee {
namespace {

constexpr wchar_t kSection[] = L"ExplorerExtras";

std::wstring IniPath() {
    const std::wstring& dir = AppDataDir();
    if (dir.empty()) return {};
    return dir + L"\\settings.ini";
}

bool ReadBool(const std::wstring& path, const wchar_t* key, bool fallback) {
    return GetPrivateProfileIntW(kSection, key, fallback ? 1 : 0, path.c_str()) != 0;
}

void WriteBool(const std::wstring& path, const wchar_t* key, bool value) {
    WritePrivateProfileStringW(kSection, key, value ? L"1" : L"0", path.c_str());
}

}  // namespace

Settings LoadSettings() {
    Settings s;
    const std::wstring path = IniPath();
    if (path.empty()) return s;
    s.enabled = ReadBool(path, L"Enabled", s.enabled);
    s.navigateUpOnDoubleClick = ReadBool(path, L"NavigateUpOnDoubleClick", s.navigateUpOnDoubleClick);
    s.subfolderTips = ReadBool(path, L"SubfolderTips", s.subfolderTips);
    s.folderItemCounts = ReadBool(path, L"FolderItemCounts", s.folderItemCounts);
    s.filePreviews = ReadBool(path, L"FilePreviews", s.filePreviews);
    s.mediaPlayback = ReadBool(path, L"MediaPlayback", s.mediaPlayback);
    s.mediaAutoPlay = ReadBool(path, L"MediaAutoPlay", s.mediaAutoPlay);
    s.runAtStartup = ReadBool(path, L"RunAtStartup", s.runAtStartup);
    return s;
}

void SaveSettings(const Settings& settings) {
    const std::wstring path = IniPath();
    if (path.empty()) return;
    WriteBool(path, L"Enabled", settings.enabled);
    WriteBool(path, L"NavigateUpOnDoubleClick", settings.navigateUpOnDoubleClick);
    WriteBool(path, L"SubfolderTips", settings.subfolderTips);
    WriteBool(path, L"FolderItemCounts", settings.folderItemCounts);
    WriteBool(path, L"FilePreviews", settings.filePreviews);
    WriteBool(path, L"MediaPlayback", settings.mediaPlayback);
    WriteBool(path, L"MediaAutoPlay", settings.mediaAutoPlay);
    WriteBool(path, L"RunAtStartup", settings.runAtStartup);
}

RuntimeConfig& Config() {
    static RuntimeConfig config;
    return config;
}

void ApplyToConfig(const Settings& settings) {
    Config().enabled.store(settings.enabled, std::memory_order_relaxed);
    Config().navigateUpOnDoubleClick.store(settings.navigateUpOnDoubleClick, std::memory_order_relaxed);
    Config().subfolderTips.store(settings.subfolderTips, std::memory_order_relaxed);
    Config().folderItemCounts.store(settings.folderItemCounts, std::memory_order_relaxed);
    Config().filePreviews.store(settings.filePreviews, std::memory_order_relaxed);
    Config().mediaPlayback.store(settings.mediaPlayback, std::memory_order_relaxed);
    Config().mediaAutoPlay.store(settings.mediaAutoPlay, std::memory_order_relaxed);
}

}  // namespace ee
