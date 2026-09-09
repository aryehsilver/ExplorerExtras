// ThumbnailLoader.h - off-thread thumbnail extraction.
//
// IShellItemImageFactory::GetImage can take seconds for a video with no cached
// thumbnail. Doing that on the worker would freeze the tip, so it runs on its
// own STA thread. Only the most recent request matters: an older one is dropped
// the moment a newer arrives, which is what hovering down a list produces.
#pragma once

#include <windows.h>

#include <condition_variable>
#include <mutex>
#include <string>

namespace ee {

class ThumbnailLoader {
public:
    ThumbnailLoader() = default;
    ~ThumbnailLoader();

    ThumbnailLoader(const ThumbnailLoader&) = delete;
    ThumbnailLoader& operator=(const ThumbnailLoader&) = delete;

    // Results arrive as |message| with wParam = token, lParam = HBITMAP (which
    // may be null when the file has no thumbnail). The receiver owns the bitmap.
    bool Start(HWND notify, UINT message);
    void Stop();

    // |token| lets the receiver discard results it no longer wants.
    void Request(const std::wstring& path, int max_edge, uint64_t token);

private:
    static DWORD WINAPI ThreadMain(LPVOID param);
    void Run();

    HWND notify_ = nullptr;
    UINT message_ = 0;
    HANDLE thread_ = nullptr;

    std::mutex mutex_;
    std::condition_variable signal_;
    std::wstring pending_path_;
    int pending_edge_ = 0;
    uint64_t pending_token_ = 0;
    bool has_pending_ = false;
    bool stopping_ = false;
};

}  // namespace ee
