#include "PreviewWindow.h"

#include <dwmapi.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "../core/Logging.h"
#include "../core/ShellItems.h"

namespace ee {
namespace {

constexpr wchar_t kPreviewClassName[] = L"ExplorerExtras.Preview";
constexpr wchar_t kMediaHostClassName[] = L"ExplorerExtras.MediaHost";

bool EnsureMediaHostClass(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = kMediaHostClassName;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    registered = RegisterClassExW(&wc) != 0;
    return registered;
}
constexpr UINT_PTR kMediaTimerId = 1;
constexpr UINT kMediaTickMs = 200;

// Roomy enough to actually read a page of a PDF or a screenful of code.
constexpr int kHandlerWidthDip = 560;
constexpr int kHandlerHeightDip = 680;
constexpr int kVideoWidthDip = 560;
constexpr int kVideoHeightDip = 315;
constexpr int kAudioWidthDip = 380;
constexpr int kAudioHeightDip = 8;  // no album art: the strip is the whole UI
constexpr int kAudioArtDip = 220;
constexpr int kControlsDip = 26;
constexpr int kFooterDip = 38;

// Transport strip layout: [button][elapsed][bar][remaining]. Fixed widths keep
// the hit rectangles identical to what is painted, with no measuring involved.
constexpr int kButtonDip = 22;
constexpr int kTimeDip = 54;
constexpr int kGapDip = 6;

int Scale(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

// Measured, not assumed: none of WS_EX_NOACTIVATE, WS_EX_TOPMOST or
// WS_EX_TOOLWINDOW has any bearing on whether a hosted preview renders. Each
// was tested in isolation against a reproducible hang and made no difference.

std::wstring FormatClock(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) return L"--:--";
    const int total = static_cast<int>(seconds);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;

    wchar_t buffer[32];
    if (hours > 0) {
        _snwprintf_s(buffer, _TRUNCATE, L"%d:%02d:%02d", hours, minutes, secs);
    } else {
        _snwprintf_s(buffer, _TRUNCATE, L"%d:%02d", minutes, secs);
    }
    return buffer;
}

}  // namespace

PreviewWindow::~PreviewWindow() {
    Hide();
}

bool PreviewWindow::EnsureClassRegistered(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PreviewWindow::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kPreviewClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    registered = RegisterClassExW(&wc) != 0;
    return registered;
}

void PreviewWindow::Hide() {
    // Release the handler and stop playback before the window they drew into
    // goes away - otherwise audio keeps going after the preview disappears.
    if (window_) KillTimer(window_, kMediaTimerId);
    // Unload, not Close: the handler object is kept alive between previews so
    // the next file of the same type reuses it instead of restarting it.
    handler_host_.Unload();
    media_.Close();  // must go before the window it renders into
    hosting_ = false;
    media_mode_ = false;
    handler_mode_ = false;

    if (video_host_) {
        DestroyWindow(video_host_);
        video_host_ = nullptr;
    }

    if (window_) {
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (bitmap_) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    if (small_font_) {
        DeleteObject(small_font_);
        small_font_ = nullptr;
    }
    caption_.clear();
    detail_.clear();
}

void PreviewWindow::Dismiss() {
    if (window_ && handler_mode_) {
        // Keep the window and the browser inside it; only drop the document.
        handler_host_.Unload();
        ShowWindow(window_, SW_HIDE);
        return;
    }
    Hide();
}

void PreviewWindow::Shutdown() {
    Hide();
    handler_host_.Close();
}

bool PreviewWindow::ContainsPoint(POINT screen_pt) const {
    if (!Visible()) return false;
    RECT rect{};
    GetWindowRect(window_, &rect);
    return PtInRect(&rect, screen_pt) != FALSE;
}

RECT PreviewWindow::ContentRect() const {
    return RECT{pad_, pad_, pad_ + content_.cx, pad_ + content_.cy};
}

RECT PreviewWindow::ControlsRect() const {
    RECT client{};
    GetClientRect(window_, &client);
    const int top = pad_ + content_.cy;
    return RECT{pad_, top, client.right - pad_, top + controls_height_};
}

RECT PreviewWindow::PlayButtonRect() const {
    const RECT controls = ControlsRect();
    return RECT{controls.left, controls.top, controls.left + Scale(kButtonDip, dpi_),
                controls.bottom};
}

RECT PreviewWindow::ProgressBarRect() const {
    const RECT controls = ControlsRect();
    const int left = controls.left + Scale(kButtonDip, dpi_) + Scale(kGapDip, dpi_) +
                     Scale(kTimeDip, dpi_) + Scale(kGapDip, dpi_);
    const int right = controls.right - Scale(kTimeDip, dpi_) - Scale(kGapDip, dpi_);
    return RECT{left, controls.top, std::max(left, right), controls.bottom};
}

RECT PreviewWindow::FooterRect() const {
    RECT client{};
    GetClientRect(window_, &client);
    return RECT{pad_, client.bottom - footer_height_, client.right - pad_, client.bottom};
}

bool PreviewWindow::CreateFrame(HINSTANCE instance, const std::wstring& path, SIZE content,
                                const RECT& avoid, int controls_dip) {
    if (!EnsureClassRegistered(instance)) return false;

    caption_ = PathFindFileNameW(path.c_str());
    detail_ = FileFactsLine(path);
    dark_ = AppsUseDarkTheme();

    window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                              kPreviewClassName, L"", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0, 0,
                              nullptr, nullptr, instance, this);
    if (!window_) return false;

    dpi_ = GetDpiForWindow(window_);
    pad_ = Scale(8, dpi_);
    footer_height_ = Scale(kFooterDip, dpi_);
    controls_height_ = controls_dip > 0 ? Scale(controls_dip, dpi_) : 0;
    content_ = content;

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
        font_ = CreateFontIndirectW(&metrics.lfMenuFont);
        // Not named "small": rpcndr.h defines that as a macro for char.
        LOGFONTW compact = metrics.lfMenuFont;
        compact.lfHeight = static_cast<LONG>(compact.lfHeight * 0.88);
        small_font_ = CreateFontIndirectW(&compact);
    }
    if (!font_) font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (!small_font_) small_font_ = font_;

    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    const int width = content_.cx + pad_ * 2;
    const int height = content_.cy + pad_ + controls_height_ + footer_height_;

    const POINT at = PlaceBeside(avoid, width, height);
    SetWindowPos(window_, HWND_TOPMOST, at.x, at.y, width, height, SWP_NOACTIVATE);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    return true;
}

POINT PreviewWindow::PlaceBeside(const RECT& avoid, int width, int height) const {
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    const POINT probe{avoid.right, avoid.top};
    GetMonitorInfoW(MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST), &monitor);

    int x = avoid.right + Scale(4, dpi_);
    if (x + width > monitor.rcWork.right) x = avoid.left - width - Scale(4, dpi_);
    x = std::clamp<int>(x, monitor.rcWork.left,
                        std::max<int>(monitor.rcWork.left, monitor.rcWork.right - width));

    int y = avoid.top;
    if (y + height > monitor.rcWork.bottom) y = monitor.rcWork.bottom - height;
    y = std::max<int>(y, monitor.rcWork.top);
    return POINT{x, y};
}

bool PreviewWindow::Show(HINSTANCE instance, HBITMAP bitmap, const std::wstring& path,
                         const RECT& avoid) {
    Hide();
    if (!bitmap) return false;

    BITMAP info{};
    if (GetObjectW(bitmap, sizeof(info), &info) == 0 || info.bmWidth <= 0 || info.bmHeight <= 0) {
        DeleteObject(bitmap);
        return false;
    }

    if (!CreateFrame(instance, path, SIZE{info.bmWidth, info.bmHeight}, avoid, 0)) {
        DeleteObject(bitmap);
        Hide();
        return false;
    }
    bitmap_ = bitmap;
    bitmap_size_ = SIZE{info.bmWidth, info.bmHeight};
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

bool PreviewWindow::ShowHandler(HINSTANCE instance, const std::wstring& path, const RECT& avoid) {
    CLSID clsid{};
    if (!PreviewHandlerHost::FindHandler(path, &clsid)) {
        Hide();
        return false;
    }

    if (window_ && handler_mode_) {
        // A handler preview is already on screen. Keep the window - and with it
        // the WebView2 the handler parked inside - and just retarget it. Every
        // destroy/recreate re-parents that browser, which is what eventually
        // leaves one stuck on its loading screen.
        handler_host_.Unload();
        caption_ = PathFindFileNameW(path.c_str());
        detail_ = FileFactsLine(path);

        RECT current{};
        GetWindowRect(window_, &current);
        const POINT at =
            PlaceBeside(avoid, current.right - current.left, current.bottom - current.top);
        SetWindowPos(window_, HWND_TOPMOST, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(window_, SW_SHOWNOACTIVATE);  // it may have been dismissed
        InvalidateRect(window_, nullptr, FALSE);
    } else {
        Hide();

        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        const POINT probe{avoid.right, avoid.top};
        GetMonitorInfoW(MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST), &monitor);

        const UINT dpi = GetDpiForSystem();
        SIZE content{Scale(kHandlerWidthDip, dpi), Scale(kHandlerHeightDip, dpi)};
        content.cx = std::min<LONG>(content.cx, (monitor.rcWork.right - monitor.rcWork.left) / 2);
        content.cy = std::min<LONG>(content.cy, monitor.rcWork.bottom - monitor.rcWork.top - 120);

        if (!CreateFrame(instance, path, content, avoid, 0)) return false;
    }

    if (!handler_host_.Open(window_, ContentRect(), path)) {
        Hide();
        return false;
    }
    hosting_ = true;
    handler_mode_ = true;
    return true;
}

bool PreviewWindow::ShowMedia(HINSTANCE instance, const std::wstring& path, const RECT& avoid,
                              bool autoplay) {
    Hide();
    if (!MediaPreview::IsMedia(path)) return false;

    const UINT dpi = GetDpiForSystem();
    const bool audio_only = MediaPreview::IsAudio(path);

    // Audio has no picture of its own, but most tracks carry album art. Where
    // there is none the strip stays slim rather than showing a black box.
    HBITMAP art = nullptr;
    SIZE art_size{};
    SIZE content{};
    if (audio_only) {
        art = LoadThumbnail(path, Scale(kAudioArtDip, dpi));
        BITMAP info{};
        if (art && GetObjectW(art, sizeof(info), &info) != 0 && info.bmWidth > 0) {
            art_size = SIZE{info.bmWidth, info.bmHeight};
            // Keep the box wide enough for the transport strip and footer.
            content = SIZE{std::max<LONG>(art_size.cx, Scale(kAudioWidthDip, dpi)), art_size.cy};
        } else {
            if (art) {
                DeleteObject(art);
                art = nullptr;
            }
            content = SIZE{Scale(kAudioWidthDip, dpi), Scale(kAudioHeightDip, dpi)};
        }
    } else {
        content = SIZE{Scale(kVideoWidthDip, dpi), Scale(kVideoHeightDip, dpi)};
    }

    if (!CreateFrame(instance, path, content, avoid, kControlsDip)) {
        if (art) DeleteObject(art);
        return false;
    }

    const RECT host = ContentRect();

    if (audio_only) {
        // No video stream, so the engine paints nothing; we own the content
        // area and draw the album art into it ourselves.
        bitmap_ = art;
        bitmap_size_ = art_size;
        hosting_ = art == nullptr;
        if (!media_.Open(window_, host, path, autoplay)) {
            Hide();
            return false;
        }
    } else {
        // Hand the engine a child sized to the content area, never the whole
        // window, or it paints over the transport strip and the footer.
        if (!EnsureMediaHostClass(instance)) {
            Hide();
            return false;
        }
        video_host_ =
            CreateWindowExW(0, kMediaHostClassName, L"", WS_CHILD | WS_VISIBLE, host.left, host.top,
                            host.right - host.left, host.bottom - host.top, window_, nullptr,
                            instance, nullptr);
        if (!video_host_) {
            Hide();
            return false;
        }

        RECT child{0, 0, host.right - host.left, host.bottom - host.top};
        if (!media_.Open(video_host_, child, path, autoplay)) {
            Hide();
            return false;
        }
        hosting_ = true;
    }

    media_mode_ = true;
    SetTimer(window_, kMediaTimerId, kMediaTickMs, nullptr);
    return true;
}

void PreviewWindow::OnClick(POINT client_pt) {
    if (!media_mode_) return;

    const RECT button = PlayButtonRect();
    const RECT content = ContentRect();

    // The button, or the picture itself. Anywhere else on the strip does
    // nothing - an invisible hit target is worse than no hit target.
    if (PtInRect(&button, client_pt) || PtInRect(&content, client_pt)) {
        media_.TogglePause();
        const RECT controls = ControlsRect();
        InvalidateRect(window_, &controls, FALSE);
        return;
    }

    const RECT bar = ProgressBarRect();
    if (!PtInRect(&bar, client_pt)) return;

    double position = 0, duration = 0;
    if (!media_.GetTimes(&position, &duration) || !std::isfinite(duration) || duration <= 0) return;

    const int width = bar.right - bar.left;
    if (width <= 0) return;

    const double fraction =
        std::clamp(static_cast<double>(client_pt.x - bar.left) / width, 0.0, 1.0);
    media_.Seek(fraction * duration);
}

void PreviewWindow::OnPaint() {
    PAINTSTRUCT ps{};
    const HDC dc = BeginPaint(window_, &ps);

    RECT client{};
    GetClientRect(window_, &client);

    const COLORREF background = dark_ ? RGB(43, 43, 43) : RGB(249, 249, 249);
    const COLORREF border = dark_ ? RGB(85, 85, 85) : RGB(200, 200, 200);
    const COLORREF text = dark_ ? RGB(240, 240, 240) : RGB(26, 26, 26);
    const COLORREF dim = dark_ ? RGB(155, 155, 155) : RGB(110, 110, 110);
    const COLORREF track = dark_ ? RGB(70, 70, 70) : RGB(214, 214, 214);
    const COLORREF fill_colour = dark_ ? RGB(96, 165, 250) : RGB(0, 103, 192);

    const HBRUSH fill = CreateSolidBrush(background);
    FillRect(dc, &client, fill);
    DeleteObject(fill);

    const HBRUSH frame = CreateSolidBrush(border);
    FrameRect(dc, &client, frame);
    DeleteObject(frame);

    // In hosted modes the handler or the media engine paints its own surface;
    // only the chrome belongs to us.
    if (!hosting_ && bitmap_) {
        const HDC memory = CreateCompatibleDC(dc);
        const HGDIOBJ previous = SelectObject(memory, bitmap_);
        // Centred: album art is usually narrower than the box, which is sized
        // for the transport strip rather than for the picture.
        const int x = pad_ + std::max<LONG>(0, (content_.cx - bitmap_size_.cx) / 2);
        BitBlt(dc, x, pad_, bitmap_size_.cx, bitmap_size_.cy, memory, 0, 0, SRCCOPY);
        SelectObject(memory, previous);
        DeleteDC(memory);
    }

    SetBkMode(dc, TRANSPARENT);

    if (media_mode_) {
        double position = 0, duration = 0;
        media_.GetTimes(&position, &duration);
        const bool known = std::isfinite(duration) && duration > 0;

        const bool paused = media_.IsPaused();
        const RECT controls = ControlsRect();
        const RECT button = PlayButtonRect();
        const RECT bar_area = ProgressBarRect();

        // Play / pause glyph, drawn rather than glyphed so it needs no font.
        {
            const int cx = (button.left + button.right) / 2;
            const int cy = (button.top + button.bottom) / 2;
            const int glyph = Scale(9, dpi_);
            const HBRUSH glyph_brush = CreateSolidBrush(text);
            if (paused) {
                const POINT triangle[3] = {{cx - glyph / 3, cy - glyph / 2},
                                           {cx + glyph / 2, cy},
                                           {cx - glyph / 3, cy + glyph / 2}};
                const HGDIOBJ old_brush = SelectObject(dc, glyph_brush);
                const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
                Polygon(dc, triangle, 3);
                SelectObject(dc, old_pen);
                SelectObject(dc, old_brush);
            } else {
                const int bar_w = std::max(2, Scale(3, dpi_));
                RECT left_bar{cx - glyph / 2, cy - glyph / 2, cx - glyph / 2 + bar_w, cy + glyph / 2};
                RECT right_bar{cx + glyph / 2 - bar_w, cy - glyph / 2, cx + glyph / 2, cy + glyph / 2};
                FillRect(dc, &left_bar, glyph_brush);
                FillRect(dc, &right_bar, glyph_brush);
            }
            DeleteObject(glyph_brush);
        }

        const HGDIOBJ previous_font = SelectObject(dc, small_font_);
        SetTextColor(dc, dim);

        const int time_width = Scale(kTimeDip, dpi_);
        RECT left_text{button.right + Scale(kGapDip, dpi_), controls.top,
                       button.right + Scale(kGapDip, dpi_) + time_width, controls.bottom};
        DrawTextW(dc, FormatClock(position).c_str(), -1, &left_text,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

        const std::wstring remaining =
            known ? (L"-" + FormatClock(std::max(0.0, duration - position))) : L"--:--";
        RECT right_text{controls.right - time_width, controls.top, controls.right, controls.bottom};
        DrawTextW(dc, remaining.c_str(), -1, &right_text,
                  DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        SelectObject(dc, previous_font);

        const int bar_height = std::max(2, Scale(4, dpi_));
        const int centre = (controls.top + controls.bottom) / 2;
        if (bar_area.right > bar_area.left) {
            RECT bar{bar_area.left, centre - bar_height / 2, bar_area.right,
                     centre - bar_height / 2 + bar_height};
            const HBRUSH track_brush = CreateSolidBrush(track);
            FillRect(dc, &bar, track_brush);
            DeleteObject(track_brush);

            if (known) {
                const double fraction = std::clamp(position / duration, 0.0, 1.0);
                RECT done = bar;
                done.right = bar.left + static_cast<int>((bar.right - bar.left) * fraction);
                if (done.right > done.left) {
                    const HBRUSH done_brush = CreateSolidBrush(fill_colour);
                    FillRect(dc, &done, done_brush);
                    DeleteObject(done_brush);
                }
            }
        }
    }

    // Footer: name on top, facts beneath.
    const RECT footer = FooterRect();
    if (!caption_.empty()) {
        const HGDIOBJ previous_font = SelectObject(dc, font_);
        SetTextColor(dc, text);
        RECT name{footer.left, footer.top, footer.right, footer.top + footer_height_ / 2};
        DrawTextW(dc, caption_.c_str(), -1, &name,
                  DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, previous_font);
    }
    if (!detail_.empty()) {
        const HGDIOBJ previous_font = SelectObject(dc, small_font_);
        SetTextColor(dc, dim);
        RECT facts{footer.left, footer.top + footer_height_ / 2, footer.right, footer.bottom};
        DrawTextW(dc, detail_.c_str(), -1, &facts,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, previous_font);
    }

    EndPaint(window_, &ps);
}

LRESULT CALLBACK PreviewWindow::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wparam, lparam);
    }

    auto* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) return DefWindowProcW(window, message, wparam, lparam);

    switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            self->OnPaint();
            return 0;

        case WM_LBUTTONDOWN:
            self->drag_origin_ = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            self->maybe_drag_ = true;
            return 0;

        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (self->maybe_drag_ && (wparam & MK_LBUTTON)) {
                if (std::abs(pt.x - self->drag_origin_.x) > GetSystemMetrics(SM_CXDRAG) ||
                    std::abs(pt.y - self->drag_origin_.y) > GetSystemMetrics(SM_CYDRAG)) {
                    self->maybe_drag_ = false;
                    if (self->on_drag_) self->on_drag_();
                }
            }
            return 0;
        }

        case WM_LBUTTONUP:
            self->maybe_drag_ = false;
            self->OnClick(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;

        case WM_RBUTTONUP:
            if (self->on_context_) self->on_context_();
            return 0;

        case WM_TIMER:
            if (wparam == kMediaTimerId && self->media_mode_) {
                // Only the transport strip changes; leave the video alone.
                const RECT controls = self->ControlsRect();
                InvalidateRect(window, &controls, FALSE);
            }
            return 0;

        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

}  // namespace ee
