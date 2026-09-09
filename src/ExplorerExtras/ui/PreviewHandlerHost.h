// PreviewHandlerHost.h - hosts the same IPreviewHandler Explorer's preview pane uses.
//
// This is what makes a PDF scrollable and code syntax-highlighted rather than a
// flat thumbnail: the work is done by whatever handler is already registered
// for the file type. Nothing is reimplemented here.
//
// STA thread with a message pump only - handlers are usually out-of-process.
#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <string>

namespace ee {

class PreviewHandlerHost {
public:
    PreviewHandlerHost() = default;
    ~PreviewHandlerHost();

    PreviewHandlerHost(const PreviewHandlerHost&) = delete;
    PreviewHandlerHost& operator=(const PreviewHandlerHost&) = delete;

    // True when a preview handler is registered for this file's extension.
    static bool FindHandler(const std::wstring& path, CLSID* clsid);

    bool Open(HWND parent, const RECT& rect, const std::wstring& path);
    void Resize(const RECT& rect);

    // Drops the current document but keeps the handler object alive, so the
    // next preview of the same type reuses it. Use this between previews.
    void Unload();

    // Releases the handler entirely. Only at shutdown, or when the next file
    // needs a different handler.
    void Close();

    bool IsOpen() const { return handler_ != nullptr; }

private:
    Microsoft::WRL::ComPtr<IPreviewHandler> handler_;
    CLSID clsid_{};      // which handler handler_ is
    bool loaded_ = false;  // whether it currently holds a document
};

}  // namespace ee
