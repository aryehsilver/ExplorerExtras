// MediaPreview.h - plays audio and video in the hover preview.
//
// Nothing registers an IPreviewHandler for media, so this uses Media
// Foundation's Media Engine directly. In windowed mode it renders video into a
// plain HWND and plays audio with no other dependency, which keeps the whole
// feature inside the platform.
#pragma once

#include <windows.h>
#include <mfmediaengine.h>
#include <wrl/client.h>

#include <string>

namespace ee {

// Released once at shutdown; MFStartup happens lazily on first use.
void ShutdownMediaFoundation();

class MediaPreview {
public:
    MediaPreview() = default;
    ~MediaPreview();

    MediaPreview(const MediaPreview&) = delete;
    MediaPreview& operator=(const MediaPreview&) = delete;

    static bool IsAudio(const std::wstring& path);
    static bool IsVideo(const std::wstring& path);
    static bool IsMedia(const std::wstring& path) { return IsAudio(path) || IsVideo(path); }

    bool Open(HWND parent, const RECT& rect, const std::wstring& path);
    void Close();
    bool IsOpen() const { return engine_ != nullptr; }

    // Playback position and length in seconds. Duration is not known until the
    // metadata loads, and is never known for a live stream, so it can come back
    // as NaN - callers must cope with that rather than assume a number.
    bool GetTimes(double* position, double* duration);
    void Seek(double seconds);
    bool IsPaused();
    void TogglePause();

private:
    Microsoft::WRL::ComPtr<IMFMediaEngine> engine_;
    Microsoft::WRL::ComPtr<IMFMediaEngineNotify> notify_;
};

}  // namespace ee
