#include "ShellItems.h"

#include <shellapi.h>
#include <shlguid.h>  // BHID_EnumItems
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>

#include "Logging.h"
#include "Settings.h"

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

// Counting stops here. A handful of folders on any machine hold six figures of
// entries, and a tip listing hundreds of folders must not pay for that; past
// this point the exact number tells the user nothing anyway.
constexpr int kMaxCountedChildren = 2000;

// Immediate children only. Recursive totals were considered and rejected: they
// mean walking whole trees, which is why Explorer itself does not show folder
// sizes in a details view.
void CountChildren(const std::wstring& path, ShellEntry* entry) {
    std::wstring pattern = path;
    if (pattern.empty()) return;
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found,
                                           FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE) return;

    entry->counted = true;
    do {
        const wchar_t* name = found.cFileName;
        if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) continue;

        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ++entry->child_folders;
        } else {
            ++entry->child_files;
        }

        if (entry->child_folders + entry->child_files >= kMaxCountedChildren) {
            entry->capped = true;
            break;
        }
    } while (FindNextFileW(search, &found));

    FindClose(search);
}

}  // namespace

int IconIndexFor(const std::wstring& path) {
    SHFILEINFOW info{};
    const DWORD_PTR result = SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                                            SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    return result ? info.iIcon : -1;
}

std::wstring DescribeChildCount(const ShellEntry& entry) {
    if (!entry.is_folder || !entry.counted) return {};

    const int folders = entry.child_folders;
    const int files = entry.child_files;
    if (folders + files == 0) return {};

    wchar_t buffer[64];
    if (entry.capped) {
        // The breakdown would be misleading once counting stopped early.
        _snwprintf_s(buffer, _TRUNCATE, L"%d+ items", folders + files);
    } else if (folders > 0 && files > 0) {
        _snwprintf_s(buffer, _TRUNCATE, L"%d %s, %d %s", folders,
                     folders == 1 ? L"folder" : L"folders", files, files == 1 ? L"file" : L"files");
    } else if (folders > 0) {
        _snwprintf_s(buffer, _TRUNCATE, L"%d %s", folders, folders == 1 ? L"folder" : L"folders");
    } else {
        _snwprintf_s(buffer, _TRUNCATE, L"%d %s", files, files == 1 ? L"file" : L"files");
    }
    return buffer;
}

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

std::wstring ResolveCrumb(const std::wstring& current_folder, const std::wstring& crumb_name) {
    if (current_folder.empty() || crumb_name.empty()) return {};

    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(current_folder.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        return {};
    }

    // The first time round is the current folder itself. Deliberately included:
    // matching it means the pointer is on the last crumb, and returning its own
    // path lets the caller recognise that and do nothing.
    for (int level = 0; level < 16 && item; ++level) {
        if (DisplayNameOf(item.Get(), SIGDN_NORMALDISPLAY) == crumb_name) {
            return DisplayNameOf(item.Get(), SIGDN_DESKTOPABSOLUTEPARSING);
        }
        ComPtr<IShellItem> parent;
        if (FAILED(item->GetParent(&parent)) || !parent) break;
        item = std::move(parent);
    }
    return {};
}

namespace {

// The pinned entries at the top of the navigation pane. They are shortcuts to
// folders that live somewhere else entirely - "Downloads" there is not a child
// of anything the pane shows above it - so a name that leads nowhere in the
// namespace is looked for here before giving up.
std::wstring ResolvePinned(const std::wstring& name) {
    // Quick access, which is what Home lists.
    constexpr wchar_t kQuickAccess[] = L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}";

    ComPtr<IShellItem> quick_access;
    if (FAILED(SHCreateItemFromParsingName(kQuickAccess, nullptr, IID_PPV_ARGS(&quick_access)))) {
        return {};
    }

    ComPtr<IEnumShellItems> enumerator;
    if (FAILED(quick_access->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&enumerator))) ||
        !enumerator) {
        return {};
    }

    ComPtr<IShellItem> child;
    while (enumerator->Next(1, child.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        const std::wstring display = DisplayNameOf(child.Get(), SIGDN_NORMALDISPLAY);
        if (display.empty()) continue;

        // The pane calls a pinned entry "Downloads (pinned)" - the state is
        // part of the accessible name. Matched from the folder's side rather
        // than by cutting a suffix off the pane's, which would mean knowing
        // what that suffix is in every language Windows ships.
        const bool same = display == name;
        const bool prefixed = name.size() > display.size() &&
                              name.compare(0, display.size(), display) == 0 &&
                              name[display.size()] == L' ';
        if (same || prefixed) return DisplayNameOf(child.Get(), SIGDN_DESKTOPABSOLUTEPARSING);
    }
    return {};
}

}  // namespace

std::wstring ResolveNavItem(const std::vector<std::wstring>& ancestors, const std::wstring& name) {
    if (name.empty()) return {};

    // Walked with IShellFolder rather than IShellItem. The root of the shell
    // namespace - the thing that has This PC, Network and OneDrive under it -
    // is only reachable as SHGetDesktopFolder; every IShellItem spelling of
    // "desktop" resolves to the directory of that name instead, whose children
    // are the user's own files. Measured: enumerating the IShellItem version
    // returns the files on the desktop and no This PC at all.
    ComPtr<IShellFolder> folder;
    HRESULT hr = SHGetDesktopFolder(&folder);
    if (FAILED(hr)) {
        EE_INFO(L"nav: SHGetDesktopFolder failed hr=0x%08X", hr);
        return {};
    }

    std::vector<std::wstring> trail = ancestors;
    trail.push_back(name);

    // Built up as the walk descends, since a child id only means anything
    // relative to the folder it came from.
    PIDLIST_ABSOLUTE absolute = nullptr;
    auto release = [&absolute] {
        if (absolute) CoTaskMemFree(absolute);
        absolute = nullptr;
    };

    for (const std::wstring& step : trail) {
        if (step.empty()) {
            release();
            return {};
        }

        ComPtr<IEnumIDList> enumerator;
        if (FAILED(folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_INCLUDEHIDDEN,
                                       &enumerator)) ||
            !enumerator) {
            release();
            return {};
        }

        PITEMID_CHILD child = nullptr;
        PITEMID_CHILD matched = nullptr;
        ULONG fetched = 0;
        while (enumerator->Next(1, &child, &fetched) == S_OK && fetched == 1) {
            STRRET raw{};
            wchar_t* display = nullptr;
            if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_INFOLDER, &raw)) &&
                SUCCEEDED(StrRetToStrW(&raw, child, &display)) && display) {
                const bool hit = step == display;
                CoTaskMemFree(display);
                if (hit) {
                    matched = child;
                    break;
                }
            }
            CoTaskMemFree(child);
        }
        if (!matched) {
            release();
            // Only a top-level entry can be a pinned one; anything deeper was
            // reached through a parent that does exist in the namespace.
            if (trail.size() == 1) return ResolvePinned(name);
            return {};
        }

        PIDLIST_ABSOLUTE combined = ILCombine(absolute, matched);
        release();
        absolute = combined;

        ComPtr<IShellFolder> next;
        hr = folder->BindToObject(matched, nullptr, IID_PPV_ARGS(&next));
        CoTaskMemFree(matched);
        if (FAILED(hr) || !next) {
            // The last step does not have to be bindable to be the answer, but
            // anything mid-trail does.
            if (&step != &trail.back()) {
                release();
                return {};
            }
            break;
        }
        folder = std::move(next);
    }

    if (!absolute) return {};

    ComPtr<IShellItem> item;
    hr = SHCreateItemFromIDList(absolute, IID_PPV_ARGS(&item));
    release();
    if (FAILED(hr)) return {};

    return DisplayNameOf(item.Get(), SIGDN_DESKTOPABSOLUTEPARSING);
}

bool ResolveChildFolder(const std::wstring& folder_path, const std::wstring& child_display_name,
                        std::wstring* child_path) {
    bool is_folder = false;
    return ResolveChild(folder_path, child_display_name, child_path, &is_folder) && is_folder;
}

std::vector<ShellEntry> EnumerateFolder(const std::wstring& folder_path, size_t max_entries,
                                        bool* truncated, bool defer_icons) {
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
        if (entry.parsing_path.empty()) {
            entry.icon_index = -1;
        } else {
            entry.icon_index = defer_icons ? kIconDeferred : IconIndexFor(entry.parsing_path);
        }

        if (entry.is_folder && !entry.parsing_path.empty()) {
            // One directory pass per folder. Skipped for UNC paths, where the
            // round trip is not worth it just to annotate a row.
            const bool unc = entry.parsing_path.rfind(L"\\\\", 0) == 0;
            if (unc) {
                entry.has_children = true;  // assume so rather than pay to find out
            } else if (Config().folderItemCounts.load(std::memory_order_relaxed)) {
                CountChildren(entry.parsing_path, &entry);
                entry.has_children = entry.child_folders + entry.child_files > 0;
            } else {
                // Not showing counts: only ever ask whether it is empty, which
                // stops at the first entry instead of reading the directory.
                entry.has_children = !PathIsDirectoryEmptyW(entry.parsing_path.c_str());
            }
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
