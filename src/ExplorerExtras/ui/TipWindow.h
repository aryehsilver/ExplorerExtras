// TipWindow.h - one level of the subfolder tip.
//
// A non-activating popup: it must never take focus from Explorer, so it uses
// WS_EX_NOACTIVATE and answers WM_MOUSEACTIVATE with MA_NOACTIVATE. Clicks are
// still delivered, which is all the tip needs.
//
// A TipWindow knows nothing about cascading. It reports which row the pointer
// is on and which row was clicked; SubfolderTipFeature owns the chain.
#pragma once

#include <windows.h>

#include <functional>
#include <vector>

#include "../core/ShellItems.h"

namespace ee {

enum class TipPlacement {
    RightOf,  // beside a row, as a submenu would open
    Below,    // under a window tab, as a drop-down would
};

class TipWindow {
public:
    using RowFn = std::function<void(TipWindow*, int)>;
    using SelfFn = std::function<void(TipWindow*)>;

    TipWindow() = default;
    ~TipWindow();

    TipWindow(const TipWindow&) = delete;
    TipWindow& operator=(const TipWindow&) = delete;

    static bool EnsureClassRegistered(HINSTANCE instance);

    // |avoid| is the rectangle the popup must not cover - the hovered row, or
    // the parent popup. The popup opens to its right, flipping left when there
    // is no room on that side of the monitor.
    bool Create(HINSTANCE instance, std::vector<ShellEntry> entries, bool truncated,
                const RECT& avoid, TipPlacement placement = TipPlacement::RightOf);

    HWND Handle() const { return window_; }
    const std::vector<ShellEntry>& entries() const { return entries_; }

    RECT WindowRect() const;
    bool ContainsPoint(POINT screen_pt) const;
    RECT RowRect(int index) const;  // screen coordinates

    // Fired after a dwell when the pointer rests on a folder row.
    void SetExpandCallback(RowFn fn) { on_expand_ = std::move(fn); }
    // Fired after a dwell when the pointer rests on a file row.
    void SetPreviewCallback(RowFn fn) { on_preview_ = std::move(fn); }
    // Fired immediately whenever the hovered row changes.
    void SetHoverChangedCallback(SelfFn fn) { on_hover_changed_ = std::move(fn); }
    // Fired on click.
    void SetActivateCallback(RowFn fn) { on_activate_ = std::move(fn); }
    // Fired on right-click: the row wants Explorer's own context menu.
    void SetContextMenuCallback(RowFn fn) { on_context_ = std::move(fn); }
    // Fired once the pointer has dragged a row past the system threshold.
    void SetDragCallback(RowFn fn) { on_drag_ = std::move(fn); }

    void ClearHover();

    // Keyboard-driven selection. Unlike hovering, this never starts the dwell
    // timers - arrow keys move, and only Right/Enter act.
    void SetSelection(int index);
    void MoveSelection(int delta);
    int Selection() const { return hover_; }
    const ShellEntry* Selected() const;

private:
    void EnsureVisible(int index);

    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    void OnPaint();
    void OnMouseMove(POINT client_pt);
    void OnWheel(int delta);
    int RowAtClient(POINT client_pt) const;
    SIZE Measure(const RECT& work_area);
    void BuildFont();

    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<ShellEntry> entries_;
    bool truncated_ = false;
    bool dark_ = false;

    UINT dpi_ = 96;
    int row_height_ = 22;
    int icon_size_ = 16;
    int pad_ = 8;
    int text_left_ = 32;
    int chevron_ = 16;
    int count_column_ = 0;  // width reserved for the folder item counts
    int visible_rows_ = 0;
    int scroll_ = 0;
    int hover_ = -1;

    RowFn on_expand_;
    RowFn on_preview_;
    RowFn on_activate_;
    RowFn on_context_;
    RowFn on_drag_;
    SelfFn on_hover_changed_;
    int pending_row_ = -1;

    POINT drag_origin_{};
    bool maybe_drag_ = false;
};

}  // namespace ee
