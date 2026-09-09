#include "HighlightWindow.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../core/ShellItems.h"

namespace ee {
namespace {

constexpr wchar_t kHighlightClassName[] = L"ExplorerExtras.Highlight";
constexpr double kPeakAlpha = 110.0;  // tab tint, at the leading edge
constexpr double kRowAlpha = 52.0;    // row tint, even across the whole row
constexpr int kTabRadiusDip = 8;      // matches the Windows 11 tab corner
constexpr int kRowRadiusDip = 4;      // matches a hovered row in the file list

bool EnsureClass(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &HighlightWindow::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kHighlightClassName;
    registered = RegisterClassExW(&wc) != 0;
    return registered;
}

}  // namespace

HighlightWindow::~HighlightWindow() {
    Hide();
}

void HighlightWindow::Hide() {
    if (!window_) return;
    DestroyWindow(window_);
    window_ = nullptr;
    bounds_ = RECT{};
}

void HighlightWindow::Show(HINSTANCE instance, const RECT& screen_rect, HighlightStyle style) {
    const int width = screen_rect.right - screen_rect.left;
    const int height = screen_rect.bottom - screen_rect.top;
    if (width <= 0 || height <= 0) {
        Hide();
        return;
    }
    // Already exactly where and how it belongs.
    if (window_ && style_ == style && EqualRect(&bounds_, &screen_rect)) return;
    style_ = style;

    if (!EnsureClass(instance)) return;

    if (!window_) {
        window_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            kHighlightClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        if (!window_) return;
    }
    bounds_ = screen_rect;

    // A per-pixel alpha ramp, so the tint is strongest at the leading edge and
    // fades out to the right. SetLayeredWindowAttributes cannot do this - it
    // only applies one alpha to the whole window - so the surface is drawn into
    // a premultiplied 32bpp DIB and handed over with UpdateLayeredWindow.
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    const HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    const HBITMAP surface = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!surface || !bits) {
        if (surface) DeleteObject(surface);
        ReleaseDC(nullptr, screen);
        return;
    }

    const COLORREF tint = AppsUseDarkTheme() ? RGB(120, 180, 255) : RGB(0, 103, 192);
    const int red = GetRValue(tint);
    const int green = GetGValue(tint);
    const int blue = GetBValue(tint);

    const bool tab = style == HighlightStyle::TabTrigger;

    std::vector<double> column_alpha(static_cast<size_t>(width));
    for (int x = 0; x < width; ++x) {
        if (!tab) {
            column_alpha[static_cast<size_t>(x)] = kRowAlpha;  // even across the row
            continue;
        }
        const double t = width > 1 ? static_cast<double>(x) / (width - 1) : 0.0;
        const double fade = (1.0 - t) * (1.0 - t);  // eased, so it lingers at the left
        column_alpha[static_cast<size_t>(x)] = kPeakAlpha * fade;
    }

    // A tab is rounded at the top and square at the bottom, and its tint sits
    // at the leading edge, so only the top-left corner needs cutting. A row is
    // rounded on all four.
    const UINT dpi = GetDpiForWindow(window_);
    const double radius = MulDiv(tab ? kTabRadiusDip : kRowRadiusDip, dpi, 96);

    auto* pixels = static_cast<BYTE*>(bits);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double alpha = column_alpha[static_cast<size_t>(x)];

            if (radius > 0) {
                // Distance from the nearest rounded corner's centre, for
                // whichever corners this style rounds.
                const bool left = x < radius;
                const bool right = !tab && x >= width - radius;
                const bool upper = y < radius;
                const bool lower = !tab && y >= height - radius;

                if ((left || right) && (upper || lower)) {
                    const double cx = left ? radius : width - radius;
                    const double cy = upper ? radius : height - radius;
                    const double dx = cx - x - 0.5;
                    const double dy = cy - y - 0.5;
                    const double distance = std::sqrt(dx * dx + dy * dy);
                    // Fractional coverage at the edge, so the curve is smooth.
                    alpha *= std::clamp(radius - distance + 0.5, 0.0, 1.0);
                }
            }

            const BYTE a = static_cast<BYTE>(alpha);
            BYTE* pixel = pixels + (static_cast<size_t>(y) * width + x) * 4;
            // Premultiplied, as AC_SRC_ALPHA requires.
            pixel[0] = static_cast<BYTE>(blue * a / 255);
            pixel[1] = static_cast<BYTE>(green * a / 255);
            pixel[2] = static_cast<BYTE>(red * a / 255);
            pixel[3] = a;
        }
    }

    const HDC memory = CreateCompatibleDC(screen);
    const HGDIOBJ previous = SelectObject(memory, surface);

    POINT position{screen_rect.left, screen_rect.top};
    SIZE size{width, height};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(window_, screen, &position, &size, memory, &source, 0, &blend, ULW_ALPHA);

    SelectObject(memory, previous);
    DeleteDC(memory);
    DeleteObject(surface);
    ReleaseDC(nullptr, screen);

    if (!IsWindowVisible(window_)) ShowWindow(window_, SW_SHOWNOACTIVATE);
    SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

LRESULT CALLBACK HighlightWindow::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace ee
