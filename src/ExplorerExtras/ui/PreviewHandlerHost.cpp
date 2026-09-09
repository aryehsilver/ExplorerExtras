#include "PreviewHandlerHost.h"

#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>

#include "../core/Logging.h"

using Microsoft::WRL::ComPtr;

namespace ee {
namespace {

// The shell extension category for preview handlers.
constexpr wchar_t kPreviewHandlerIid[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";

}  // namespace

PreviewHandlerHost::~PreviewHandlerHost() {
    Close();
}

bool PreviewHandlerHost::FindHandler(const std::wstring& path, CLSID* clsid) {
    const wchar_t* extension = PathFindExtensionW(path.c_str());
    if (!extension || !*extension) return false;

    wchar_t buffer[64]{};
    DWORD size = ARRAYSIZE(buffer);
    // AssocQueryString walks the ProgID and SystemFileAssociations chain for us.
    if (FAILED(AssocQueryStringW(ASSOCF_INIT_DEFAULTTOSTAR, ASSOCSTR_SHELLEXTENSION, extension,
                                 kPreviewHandlerIid, buffer, &size))) {
        return false;
    }
    return SUCCEEDED(CLSIDFromString(buffer, clsid));
}

bool PreviewHandlerHost::Open(HWND parent, const RECT& rect, const std::wstring& path) {
    CLSID clsid{};
    if (!FindHandler(path, &clsid)) {
        Close();
        return false;
    }

    // A different file type needs a different handler; the current one is of
    // no further use.
    if (handler_ && !IsEqualGUID(clsid, clsid_)) Close();

    HRESULT hr = S_OK;

    if (handler_) {
        // Reuse it. Several of these handlers are WebView2-backed and take
        // seconds to start; creating a fresh one per hover pays that every
        // time, and tearing one down while it is still initialising is what
        // leaves a preview stuck on its loading screen forever.
        Unload();
    } else {
        hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&handler_));
        if (FAILED(hr) || !handler_) {
            EE_INFO(L"preview handler CoCreateInstance failed hr=0x%08X", hr);
            return false;
        }
        clsid_ = clsid;
        EE_INFO(L"preview handler created");
    }

    // Handlers implement exactly one of these three; try them in the order the
    // documentation recommends.
    bool initialized = false;

    ComPtr<IInitializeWithFile> with_file;
    if (SUCCEEDED(handler_.As(&with_file)) && with_file) {
        initialized = SUCCEEDED(with_file->Initialize(path.c_str(), STGM_READ));
    }

    if (!initialized) {
        ComPtr<IInitializeWithStream> with_stream;
        if (SUCCEEDED(handler_.As(&with_stream)) && with_stream) {
            ComPtr<IStream> stream;
            if (SUCCEEDED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE,
                                                 FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream))) {
                initialized = SUCCEEDED(with_stream->Initialize(stream.Get(), STGM_READ));
            }
        }
    }

    if (!initialized) {
        ComPtr<IInitializeWithItem> with_item;
        ComPtr<IShellItem> item;
        if (SUCCEEDED(handler_.As(&with_item)) && with_item &&
            SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            initialized = SUCCEEDED(with_item->Initialize(item.Get(), STGM_READ));
        }
    }

    if (!initialized) {
        EE_INFO(L"preview handler would not initialise for '%s'", path.c_str());
        Close();
        return false;
    }

    RECT bounds = rect;
    hr = handler_->SetWindow(parent, &bounds);
    if (FAILED(hr)) {
        Close();
        return false;
    }
    handler_->SetRect(&bounds);

    hr = handler_->DoPreview();
    if (FAILED(hr)) {
        EE_INFO(L"preview handler DoPreview failed hr=0x%08X", hr);
        Close();
        return false;
    }
    loaded_ = true;
    return true;
}

void PreviewHandlerHost::Resize(const RECT& rect) {
    if (!handler_) return;
    RECT bounds = rect;
    handler_->SetRect(&bounds);
}

void PreviewHandlerHost::Unload() {
    if (!handler_ || !loaded_) return;
    handler_->Unload();
    loaded_ = false;
}

void PreviewHandlerHost::Close() {
    if (!handler_) return;
    handler_->Unload();
    handler_.Reset();
    clsid_ = CLSID{};
    loaded_ = false;
}

}  // namespace ee
