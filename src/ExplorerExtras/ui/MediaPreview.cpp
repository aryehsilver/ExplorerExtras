#include "MediaPreview.h"

#include <dxgiformat.h>
#include <mfapi.h>
#include <shlwapi.h>

#include <mutex>
#include <new>

#include "../core/Logging.h"

using Microsoft::WRL::ComPtr;

namespace ee {
namespace {

// A hover preview should be audible but not startling.
constexpr double kPreviewVolume = 0.5;

const wchar_t* const kAudioExtensions[] = {L".mp3", L".wav",  L".flac", L".m4a",
                                           L".aac", L".wma",  L".ogg",  L".opus"};
const wchar_t* const kVideoExtensions[] = {L".mp4", L".m4v", L".mov", L".wmv",
                                           L".avi", L".mkv", L".webm"};

std::mutex g_mf_mutex;
bool g_mf_started = false;

bool EnsureMediaFoundation() {
    std::lock_guard<std::mutex> lock(g_mf_mutex);
    if (g_mf_started) return true;
    const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        EE_ERR(L"MFStartup failed hr=0x%08X", hr);
        return false;
    }
    g_mf_started = true;
    return true;
}

bool ExtensionIn(const std::wstring& path, const wchar_t* const* list, size_t count) {
    const wchar_t* extension = PathFindExtensionW(path.c_str());
    if (!extension || !*extension) return false;
    for (size_t i = 0; i < count; ++i) {
        if (CompareStringOrdinal(extension, -1, list[i], -1, TRUE) == CSTR_EQUAL) return true;
    }
    return false;
}

// The Media Engine requires a callback even when there is nothing to react to:
// autoplay handles starting playback for us.
class EngineNotify : public IMFMediaEngineNotify {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG remaining = InterlockedDecrement(&refs_);
        if (remaining == 0) delete this;
        return remaining;
    }

    IFACEMETHODIMP EventNotify(DWORD event, DWORD_PTR, DWORD) override {
        if (event == MF_MEDIA_ENGINE_EVENT_ERROR) {
            EE_INFO(L"media engine reported an error");
        }
        return S_OK;
    }

private:
    LONG refs_ = 1;
};

}  // namespace

void ShutdownMediaFoundation() {
    std::lock_guard<std::mutex> lock(g_mf_mutex);
    if (!g_mf_started) return;
    MFShutdown();
    g_mf_started = false;
}

MediaPreview::~MediaPreview() {
    Close();
}

bool MediaPreview::IsAudio(const std::wstring& path) {
    return ExtensionIn(path, kAudioExtensions, ARRAYSIZE(kAudioExtensions));
}

bool MediaPreview::IsVideo(const std::wstring& path) {
    return ExtensionIn(path, kVideoExtensions, ARRAYSIZE(kVideoExtensions));
}

bool MediaPreview::Open(HWND parent, const RECT& rect, const std::wstring& path) {
    Close();
    if (!EnsureMediaFoundation()) return false;

    ComPtr<IMFMediaEngineClassFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        EE_INFO(L"media engine factory unavailable hr=0x%08X", hr);
        return false;
    }

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 3);
    if (FAILED(hr)) return false;

    auto* callback = new (std::nothrow) EngineNotify();
    if (!callback) return false;
    notify_.Attach(callback);  // created with one reference; ComPtr owns it

    attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify_.Get());
    attributes->SetUINT64(MF_MEDIA_ENGINE_PLAYBACK_HWND, reinterpret_cast<UINT64>(parent));
    attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);

    hr = factory->CreateInstance(0, attributes.Get(), &engine_);
    if (FAILED(hr) || !engine_) {
        EE_INFO(L"media engine creation failed hr=0x%08X", hr);
        Close();
        return false;
    }

    engine_->SetAutoPlay(TRUE);
    engine_->SetVolume(kPreviewVolume);

    BSTR url = SysAllocString(path.c_str());
    if (!url) {
        Close();
        return false;
    }
    hr = engine_->SetSource(url);
    SysFreeString(url);

    if (FAILED(hr)) {
        EE_INFO(L"media engine SetSource failed hr=0x%08X", hr);
        Close();
        return false;
    }

    UNREFERENCED_PARAMETER(rect);
    return true;
}

bool MediaPreview::GetTimes(double* position, double* duration) {
    if (!engine_) return false;
    if (position) *position = engine_->GetCurrentTime();
    if (duration) *duration = engine_->GetDuration();
    return true;
}

void MediaPreview::Seek(double seconds) {
    if (!engine_) return;
    engine_->SetCurrentTime(seconds);
}

bool MediaPreview::IsPaused() {
    return engine_ && engine_->IsPaused();
}

void MediaPreview::TogglePause() {
    if (!engine_) return;
    if (engine_->IsPaused()) {
        engine_->Play();
    } else {
        engine_->Pause();
    }
}

void MediaPreview::Close() {
    if (engine_) {
        engine_->Shutdown();
        engine_.Reset();
    }
    notify_.Reset();
}

}  // namespace ee
