#include "ThumbnailLoader.h"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include "Logging.h"

using Microsoft::WRL::ComPtr;

namespace ee {

ThumbnailLoader::~ThumbnailLoader() {
    Stop();
}

bool ThumbnailLoader::Start(HWND notify, UINT message) {
    if (thread_) return true;
    notify_ = notify;
    message_ = message;
    thread_ = CreateThread(nullptr, 0, &ThumbnailLoader::ThreadMain, this, 0, nullptr);
    return thread_ != nullptr;
}

void ThumbnailLoader::Stop() {
    if (!thread_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    signal_.notify_all();
    WaitForSingleObject(thread_, 5000);
    CloseHandle(thread_);
    thread_ = nullptr;
}

void ThumbnailLoader::Request(const std::wstring& path, int max_edge, uint64_t token) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_path_ = path;
        pending_edge_ = max_edge;
        pending_token_ = token;
        has_pending_ = true;  // replaces any request not yet started
    }
    signal_.notify_one();
}

DWORD WINAPI ThumbnailLoader::ThreadMain(LPVOID param) {
    static_cast<ThumbnailLoader*>(param)->Run();
    return 0;
}

void ThumbnailLoader::Run() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        EE_ERR(L"thumbnail thread could not initialise COM");
        return;
    }

    for (;;) {
        std::wstring path;
        int edge = 0;
        uint64_t token = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            signal_.wait(lock, [this] { return has_pending_ || stopping_; });
            if (stopping_) break;
            path = pending_path_;
            edge = pending_edge_;
            token = pending_token_;
            has_pending_ = false;
        }

        HBITMAP bitmap = nullptr;
        ComPtr<IShellItemImageFactory> factory;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
            const SIZE size{edge, edge};
            // THUMBNAILONLY: an icon is not a preview, and falling back to one
            // would show a generic glyph for every unsupported file.
            if (FAILED(factory->GetImage(size, SIIGBF_THUMBNAILONLY, &bitmap))) {
                bitmap = nullptr;
            }
        }

        if (!PostMessageW(notify_, message_, static_cast<WPARAM>(token),
                          reinterpret_cast<LPARAM>(bitmap))) {
            if (bitmap) DeleteObject(bitmap);
        }
    }

    CoUninitialize();
}

}  // namespace ee
