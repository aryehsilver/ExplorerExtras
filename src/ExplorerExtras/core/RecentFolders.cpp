#include "RecentFolders.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <strsafe.h>

#include <algorithm>
#include <mutex>

#include "Logging.h"
#include "Paths.h"

namespace ee {
namespace {

// Remembered, and shown. More than a dozen in a menu is a scroll bar, and a
// list longer than that is a search problem rather than a menu.
constexpr size_t kMaxRemembered = 40;
// Room for the deepest path a menu should carry before it is shortened.
constexpr int kContextChars = 44;

std::mutex g_lock;

std::wstring ListPath() {
    const std::wstring& dir = AppDataDir();
    if (dir.empty()) return {};
    return dir + L"\\recent.txt";
}

bool SameFolder(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// What a folder calls itself, which is not always its last path component:
// a drive root is "Local Disk (C:)" and a library or a OneDrive root has a name
// of its own.
std::wstring DisplayNameOf(const std::wstring& path) {
    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_DIRECTORY, &info, sizeof(info),
                       SHGFI_DISPLAYNAME | SHGFI_USEFILEATTRIBUTES)) {
        if (info.szDisplayName[0] != L'\0') return info.szDisplayName;
    }
    const wchar_t* leaf = PathFindFileNameW(path.c_str());
    return leaf && *leaf ? leaf : path;
}

// The folder above, shortened with ellipses in the middle the way the shell
// does it, so a deep path does not set the width of the whole menu.
std::wstring ContextOf(const std::wstring& path) {
    wchar_t parent[MAX_PATH]{};
    if (FAILED(StringCchCopyW(parent, ARRAYSIZE(parent), path.c_str()))) return {};
    if (!PathRemoveFileSpecW(parent) || parent[0] == L'\0') return {};
    if (SameFolder(parent, path)) return {};

    wchar_t compact[MAX_PATH]{};
    if (PathCompactPathExW(compact, parent, kContextChars, 0)) return compact;
    return parent;
}

}  // namespace

RecentFolders& Recents() {
    static RecentFolders recents;
    return recents;
}

void RecentFolders::Note(const std::wstring& path) {
    if (path.empty()) return;
    // Only filesystem places: a virtual folder cannot be reopened by path, and
    // half of them are a different thing every time anyway.
    if (path.size() < 3 || path.find(L'\\') == std::wstring::npos) return;

    std::lock_guard<std::mutex> guard(g_lock);
    if (!paths_.empty() && SameFolder(paths_.front(), path)) return;  // already the newest

    paths_.erase(std::remove_if(paths_.begin(), paths_.end(),
                                [&path](const std::wstring& seen) { return SameFolder(seen, path); }),
                 paths_.end());
    paths_.insert(paths_.begin(), path);
    if (paths_.size() > kMaxRemembered) paths_.resize(kMaxRemembered);
    dirty_ = true;
}

std::vector<RecentFolder> RecentFolders::List(const std::vector<std::wstring>& open_now,
                                              size_t limit) const {
    std::lock_guard<std::mutex> guard(g_lock);

    std::vector<RecentFolder> result;
    for (const std::wstring& path : paths_) {
        if (result.size() >= limit) break;

        const bool open = std::any_of(
            open_now.begin(), open_now.end(),
            [&path](const std::wstring& shown) { return SameFolder(shown, path); });
        if (open) continue;

        // A folder that has been deleted or is on a drive that has gone away
        // has no business in a list of places to go back to.
        if (!PathIsDirectoryW(path.c_str())) continue;

        result.push_back(RecentFolder{path, DisplayNameOf(path), ContextOf(path)});
    }
    return result;
}

void RecentFolders::Clear() {
    std::lock_guard<std::mutex> guard(g_lock);
    paths_.clear();
    dirty_ = true;
}

void RecentFolders::Load() {
    const std::wstring path = ListPath();
    if (path.empty()) return;

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER size{};
    std::wstring text;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 2 && size.QuadPart < 64 * 1024) {
        std::vector<wchar_t> buffer(static_cast<size_t>(size.QuadPart) / sizeof(wchar_t) + 1, L'\0');
        DWORD read = 0;
        if (ReadFile(file, buffer.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr)) {
            text.assign(buffer.data(), read / sizeof(wchar_t));
        }
    }
    CloseHandle(file);
    if (text.empty()) return;
    if (text.front() == 0xFEFF) text.erase(text.begin());  // the byte order mark

    std::lock_guard<std::mutex> guard(g_lock);
    paths_.clear();
    size_t start = 0;
    while (start < text.size() && paths_.size() < kMaxRemembered) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (!line.empty()) paths_.push_back(std::move(line));
        start = end + 1;
    }
    dirty_ = false;
    EE_INFO(L"recent folders: %zu remembered", paths_.size());
}

void RecentFolders::Save() {
    std::wstring text;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (!dirty_) return;
        dirty_ = false;
        text.push_back(static_cast<wchar_t>(0xFEFF));
        for (const std::wstring& path : paths_) {
            text += path;
            text += L"\r\n";
        }
    }

    const std::wstring path = ListPath();
    if (path.empty()) return;

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written,
              nullptr);
    CloseHandle(file);
}

}  // namespace ee
