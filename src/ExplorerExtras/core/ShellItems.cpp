#include "ShellItems.h"

#include <shellapi.h>
#include <shlguid.h>  // BHID_EnumItems
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>

#include "Logging.h"

using Microsoft::WRL::ComPtr;

namespace ee {
namespace {

std::wstring DisplayNameOf(IShellItem* item, SIGDN form) {
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(form, &raw)) || !raw) return {};
    std::wstring value(raw);
    CoTaskMemFree(raw);
    return value;
}

int IconIndexFor(const std::wstring& path) {
    SHFILEINFOW info{};
    const DWORD_PTR result = SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                                            SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    return result ? info.iIcon : -1;
}

}  // namespace

HBITMAP LoadThumbnail(const std::wstring& path, int max_edge) {
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }
    HBITMAP bitmap = nullptr;
    const SIZE size{max_edge, max_edge};
    if (FAILED(factory->GetImage(size, SIIGBF_THUMBNAILONLY, &bitmap))) return nullptr;
    return bitmap;
}

namespace {

std::wstring FormatStamp(const FILETIME& stamp, bool with_time) {
    FILETIME local{};
    SYSTEMTIME parts{};
    if (!FileTimeToLocalFileTime(&stamp, &local) || !FileTimeToSystemTime(&local, &parts)) {
        return {};
    }

    wchar_t date[64]{};
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &parts, nullptr, date,
                        ARRAYSIZE(date), nullptr) == 0) {
        return {};
    }
    if (!with_time) return date;

    wchar_t clock[64]{};
    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &parts, nullptr, clock,
                        ARRAYSIZE(clock)) == 0) {
        return date;
    }
    return std::wstring(date) + L" " + clock;
}

}  // namespace

std::wstring FileFactsLine(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return {};

    ULARGE_INTEGER size{};
    size.HighPart = info.nFileSizeHigh;
    size.LowPart = info.nFileSizeLow;

    std::wstring line;
    wchar_t pretty[64]{};
    if (SUCCEEDED(StrFormatByteSizeEx(size.QuadPart, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT,
                                      pretty, ARRAYSIZE(pretty)))) {
        line = pretty;
    }

    const std::wstring created = FormatStamp(info.ftCreationTime, false);
    const std::wstring modified = FormatStamp(info.ftLastWriteTime, true);

    if (!created.empty()) {
        if (!line.empty()) line += L"   \x2022   ";
        line += L"Created " + created;
    }
    if (!modified.empty()) {
        if (!line.empty()) line += L"   \x2022   ";
        line += L"Modified " + modified;
    }
    return line;
}

HIMAGELIST SystemSmallImageList() {
    static HIMAGELIST list = [] {
        SHFILEINFOW info{};
        // Any path works; the call returns the shared system image list.
        return reinterpret_cast<HIMAGELIST>(
            SHGetFileInfoW(L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &info, sizeof(info),
                           SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));
    }();
    return list;
}

bool AppsUseDarkTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return false;
    }
    return value == 0;
}

bool ResolveChild(const std::wstring& folder_path, const std::wstring& child_display_name,
                  std::wstring* child_path, bool* is_folder) {
    if (folder_path.empty() || child_display_name.empty()) return false;
    // Only filesystem locations: a virtual one has no path to combine with.
    if (folder_path.find(L'\\') == std::wstring::npos) return false;

    std::wstring combined = folder_path;
    if (combined.back() != L'\\') combined += L'\\';
    combined += child_display_name;

    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(combined.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        return false;
    }

    SFGAOF attributes = 0;
    if (FAILED(item->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &attributes))) return false;

    // SFGAO_STREAM excludes zip files and the like, which present as folders but
    // are slow to enumerate and rarely what the user is after.
    if (is_folder) *is_folder = (attributes & SFGAO_FOLDER) && !(attributes & SFGAO_STREAM);

    if (child_path) {
        *child_path = DisplayNameOf(item.Get(), SIGDN_DESKTOPABSOLUTEPARSING);
        if (child_path->empty()) return false;
    }
    return true;
}

bool ResolveChildFolder(const std::wstring& folder_path, const std::wstring& child_display_name,
                        std::wstring* child_path) {
    bool is_folder = false;
    return ResolveChild(folder_path, child_display_name, child_path, &is_folder) && is_folder;
}

std::vector<ShellEntry> EnumerateFolder(const std::wstring& folder_path, size_t max_entries,
                                        bool* truncated) {
    std::vector<ShellEntry> entries;
    if (truncated) *truncated = false;
    if (folder_path.empty()) return entries;

    ComPtr<IShellItem> folder;
    if (FAILED(SHCreateItemFromParsingName(folder_path.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        return entries;
    }

    ComPtr<IEnumShellItems> enumerator;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&enumerator))) ||
        !enumerator) {
        return entries;
    }

    ComPtr<IShellItem> child;
    while (enumerator->Next(1, child.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        if (entries.size() >= max_entries) {
            if (truncated) *truncated = true;
            break;
        }

        SFGAOF attributes = 0;
        child->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &attributes);

        ShellEntry entry;
        entry.is_folder = (attributes & SFGAO_FOLDER) && !(attributes & SFGAO_STREAM);
        entry.display_name = DisplayNameOf(child.Get(), SIGDN_NORMALDISPLAY);
        entry.parsing_path = DisplayNameOf(child.Get(), SIGDN_DESKTOPABSOLUTEPARSING);
        if (entry.display_name.empty()) continue;
        entry.icon_index = entry.parsing_path.empty() ? -1 : IconIndexFor(entry.parsing_path);

        if (entry.is_folder && !entry.parsing_path.empty()) {
            // One FindFirstFile per folder. Skipped for UNC paths, where the
            // round trip is not worth it just to decide whether to draw an arrow.
            const bool unc = entry.parsing_path.rfind(L"\\\\", 0) == 0;
            entry.has_children = unc || !PathIsDirectoryEmptyW(entry.parsing_path.c_str());
        }

        entries.push_back(std::move(entry));
    }

    // Folders first, then files - each in Explorer's natural ordering, so "10"
    // sorts after "9" rather than after "1".
    std::stable_sort(entries.begin(), entries.end(), [](const ShellEntry& a, const ShellEntry& b) {
        if (a.is_folder != b.is_folder) return a.is_folder;
        return StrCmpLogicalW(a.display_name.c_str(), b.display_name.c_str()) < 0;
    });

    return entries;
}

}  // namespace ee
