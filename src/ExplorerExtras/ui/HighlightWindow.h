// HighlightWindow.h - the tint over the part of a tab that opens a drop-down.
//
// We cannot draw inside Explorer's XAML tab strip, so this is a translucent
// layered window laid over it. WS_EX_TRANSPARENT keeps it out of the way of the
// mouse entirely - the user is still clicking the tab underneath, not this.
#pragma once

#include <windows.h>

namespace ee {

enum class HighlightStyle {
    // Over the part of a tab that opens a drop-down: fades away to the right,
    // top-left corner cut to the tab's own radius.
    TabTrigger,
    // Over the row a tip was opened from, so it stays visibly the source while
    // the pointer is away in the popup: even tint, softly rounded.
    Row,
};

class HighlightWindow {
public:
    HighlightWindow() = default;
    ~HighlightWindow();

    HighlightWindow(const HighlightWindow&) = delete;
    HighlightWindow& operator=(const HighlightWindow&) = delete;

    void Show(HINSTANCE instance, const RECT& screen_rect, HighlightStyle style);
    void Hide();
    bool Visible() const { return window_ != nullptr; }

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    HWND window_ = nullptr;
    RECT bounds_{};
    HighlightStyle style_ = HighlightStyle::TabTrigger;
};

}  // namespace ee
