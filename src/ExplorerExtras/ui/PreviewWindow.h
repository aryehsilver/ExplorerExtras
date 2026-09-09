// PreviewWindow.h - what the pointer resting on a file shows.
//
// Three modes, chosen by the caller in this order:
//   media    - audio and video, played through Media Foundation, with a
//              transport strip showing position, length and time remaining
//   handler  - the registered IPreviewHandler, giving scrollable PDFs and
//              syntax-highlighted code for free
//   bitmap   - a shell thumbnail, which is all an image needs
//
// Every mode gets the same footer: file name, size, created and modified.
// Non-activating, like TipWindow.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "MediaPreview.h"
#include "PreviewHandlerHost.h"

namespace ee {

class PreviewWindow {
public:
    PreviewWindow() = default;
    ~PreviewWindow();

    PreviewWindow(const PreviewWindow&) = delete;
    PreviewWindow& operator=(const PreviewWindow&) = delete;

    static bool EnsureClassRegistered(HINSTANCE instance);

    // Takes ownership of |bitmap|.
    bool Show(HINSTANCE instance, HBITMAP bitmap, const std::wstring& path, const RECT& avoid);

    // False when no handler is registered, or it refused, so the caller can
    // fall back to a thumbnail.
    bool ShowHandler(HINSTANCE instance, const std::wstring& path, const RECT& avoid);

    // Plays an audio or video file. Nothing registers a preview handler for
    // media, so this drives Media Foundation directly.
    bool ShowMedia(HINSTANCE instance, const std::wstring& path, const RECT& avoid);

    void Hide();

    bool Visible() const { return window_ != nullptr; }
    HWND Handle() const { return window_; }
    bool ContainsPoint(POINT screen_pt) const;

    using VoidFn = std::function<void()>;
    // Dragged past the system threshold: the previewed file becomes a shell
    // drag source, so it can be dropped anywhere Explorer accepts a file.
    void SetDragCallback(VoidFn fn) { on_drag_ = std::move(fn); }
    void SetContextMenuCallback(VoidFn fn) { on_context_ = std::move(fn); }

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    void OnPaint();
    void OnClick(POINT client_pt);
    bool CreateFrame(HINSTANCE instance, const std::wstring& path, SIZE content, const RECT& avoid,
                     int controls_dip);

    RECT ContentRect() const;
    RECT ControlsRect() const;
    RECT PlayButtonRect() const;
    RECT ProgressBarRect() const;
    RECT FooterRect() const;

    HWND window_ = nullptr;
    // The Media Engine paints over the whole window it is given, so video gets
    // its own child confined to the content area. Without it the engine covers
    // the transport strip and the footer.
    HWND video_host_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HFONT font_ = nullptr;
    HFONT small_font_ = nullptr;
    SIZE content_{};      // the content area
    SIZE bitmap_size_{};  // the bitmap itself, centred within it

    std::wstring caption_;  // file name
    std::wstring detail_;   // size, created, modified

    bool dark_ = false;
    bool hosting_ = false;     // handler or media paints its own surface
    bool media_mode_ = false;  // transport strip is live

    UINT dpi_ = 96;
    int pad_ = 8;
    int footer_height_ = 0;
    int controls_height_ = 0;

    VoidFn on_drag_;
    VoidFn on_context_;
    POINT drag_origin_{};
    bool maybe_drag_ = false;

    PreviewHandlerHost handler_host_;
    MediaPreview media_;
};

}  // namespace ee
