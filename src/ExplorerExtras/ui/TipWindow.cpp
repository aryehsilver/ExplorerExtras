#include "TipWindow.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <string>

#include "../core/Logging.h"

namespace ee {
namespace {

constexpr wchar_t kTipClassName[] = L"ExplorerExtras.Tip";
constexpr UINT_PTR kExpandTimerId = 1;
constexpr UINT kExpandDelayMs = 250;
constexpr int kMaxRows = 28;
constexpr int kMinWidthDip = 140;
constexpr int kMaxWidthDip = 420;
constexpr int kMaxWidthWithCountsDip = 560;
constexpr int kCountGapDip = 20;

int Scale(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

struct Palette {
    COLORREF background;
    COLORREF text;
    COLORREF dim;
    COLORREF hover;
    COLORREF border;
};

Palette PaletteFor(bool dark) {
    if (dark) {
        return {RGB(43, 43, 43), RGB(240, 240, 240), RGB(160, 160, 160), RGB(69, 69, 69),
                RGB(85, 85, 85)};
    }
    return {RGB(249, 249, 249), RGB(26, 26, 26), RGB(110, 110, 110), RGB(233, 233, 233),
            RGB(200, 200, 200)};
}

}  // namespace

TipWindow::~TipWindow() {
    if (window_) {
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
}

bool TipWindow::EnsureClassRegistered(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TipWindow::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kTipClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    registered = RegisterClassExW(&wc) != 0;
    return registered;
}

void TipWindow::BuildFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
        font_ = CreateFontIndirectW(&metrics.lfMenuFont);
    }
    if (!font_) font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

SIZE TipWindow::Measure(const RECT& work_area) {
    const HDC dc = GetDC(window_);
    const HGDIOBJ previous = SelectObject(dc, font_);

    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    row_height_ = std::max<int>(Scale(icon_size_, dpi_) + Scale(6, dpi_), tm.tmHeight + Scale(8, dpi_));

    int widest = 0;
    int widest_count = 0;
    for (const auto& entry : entries_) {
        SIZE extent{};
        GetTextExtentPoint32W(dc, entry.display_name.c_str(),
                              static_cast<int>(entry.display_name.size()), &extent);
        widest = std::max(widest, static_cast<int>(extent.cx));

        const std::wstring count = DescribeChildCount(entry);
        if (count.empty()) continue;
        SIZE count_extent{};
        GetTextExtentPoint32W(dc, count.c_str(), static_cast<int>(count.size()), &count_extent);
        widest_count = std::max(widest_count, static_cast<int>(count_extent.cx));
    }

    SelectObject(dc, previous);
    ReleaseDC(window_, dc);

    // A couple of pixels of slack: GetTextExtentPoint32W and DrawTextW do not
    // agree to the pixel, and an exactly-sized column ellipsises a name that
    // would otherwise fit.
    widest += Scale(6, dpi_);
    count_column_ = widest_count;
    const int gap = widest_count > 0 ? Scale(kCountGapDip, dpi_) : 0;
    // Allow a wider tip when there is a second column, so names are not
    // squeezed into ellipses to make room for the counts.
    const int max_width = Scale(widest_count > 0 ? kMaxWidthWithCountsDip : kMaxWidthDip, dpi_);
    const int width = std::clamp(text_left_ + widest + gap + widest_count + chevron_ + pad_,
                                 Scale(kMinWidthDip, dpi_), max_width);

    const int room = (work_area.bottom - work_area.top) / std::max(row_height_, 1) - 1;
    const int rows = static_cast<int>(rows_.size()) + (truncated_ ? 1 : 0);
    visible_rows_ = std::clamp(rows, 1, std::min(kMaxRows, std::max(room, 1)));

    return SIZE{width, visible_rows_ * row_height_ + 2};
}

bool TipWindow::Create(HINSTANCE instance, std::vector<ShellEntry> entries, bool truncated,
                       const RECT& avoid, TipPlacement placement) {
    if (!EnsureClassRegistered(instance)) return false;
    entries_ = std::move(entries);
    truncated_ = truncated;
    anchor_ = avoid;
    ApplyFilter();
    dark_ = AppsUseDarkTheme();
    if (entries_.empty() && !truncated_) return false;

    window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kTipClassName,
                              L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!window_) return false;

    dpi_ = GetDpiForWindow(window_);
    icon_size_ = 16;
    pad_ = Scale(8, dpi_);
    text_left_ = Scale(32, dpi_);
    chevron_ = Scale(16, dpi_);
    BuildFont();

    // Rounded corners to match Windows 11 menus. Ignored on older builds.
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    const POINT probe{avoid.right, avoid.top};
    GetMonitorInfoW(MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST), &monitor);
    work_area_ = monitor.rcWork;

    const SIZE size = Measure(monitor.rcWork);

    int x = 0;
    int y = 0;
    if (placement == TipPlacement::Below) {
        // Hangs off the anchor's left edge, dropping down; flips above when
        // there is no room below.
        x = avoid.left;
        y = avoid.bottom;
        if (y + size.cy > monitor.rcWork.bottom) y = avoid.top - size.cy;
    } else {
        // Right of the anchor by preference, flipped to the left when that
        // would run off the monitor.
        x = avoid.right;
        if (x + size.cx > monitor.rcWork.right) x = avoid.left - size.cx;
        y = avoid.top;
        if (y + size.cy > monitor.rcWork.bottom) y = monitor.rcWork.bottom - size.cy;
    }

    x = std::clamp<int>(x, monitor.rcWork.left,
                        std::max<int>(monitor.rcWork.left, monitor.rcWork.right - size.cx));
    y = std::clamp<int>(y, monitor.rcWork.top,
                        std::max<int>(monitor.rcWork.top, monitor.rcWork.bottom - size.cy));

    // The top row starts highlighted so the keyboard has somewhere to begin.
    hover_ = rows_.empty() ? -1 : rows_.front();

    SetWindowPos(window_, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    footprint_ = RECT{x, y, x + size.cx, y + size.cy};
    return true;
}

RECT TipWindow::WindowRect() const {
    RECT rect{};
    if (window_) GetWindowRect(window_, &rect);
    return rect;
}

bool TipWindow::ContainsPoint(POINT screen_pt) const {
    if (!window_) return false;
    RECT rect{};
    GetWindowRect(window_, &rect);
    return PtInRect(&rect, screen_pt) != FALSE;
}

RECT TipWindow::RowRect(int index) const {
    RECT rect{};
    if (!window_) return rect;
    const int position = PositionOf(index);
    if (position < 0) return rect;  // filtered out: it is not on screen
    GetWindowRect(window_, &rect);
    const int top = rect.top + 1 + (position - scroll_) * row_height_;
    return RECT{rect.left, top, rect.right, top + row_height_};
}

int TipWindow::RowAtClient(POINT client_pt) const {
    if (client_pt.y < 1) return -1;
    RECT client{};
    GetClientRect(window_, &client);
    if (client_pt.y >= client.bottom - footer_height_ - 1) return -1;  // the filter footer
    return EntryAt(scroll_ + (client_pt.y - 1) / std::max(row_height_, 1));
}

int TipWindow::PositionOf(int entry_index) const {
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i] == entry_index) return static_cast<int>(i);
    }
    return -1;
}

int TipWindow::EntryAt(int position) const {
    if (position < 0 || position >= static_cast<int>(rows_.size())) return -1;
    return rows_[position];
}

void TipWindow::ApplyFilter() {
    rows_.clear();
    rows_.reserve(entries_.size());
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        // Substring rather than prefix: half of what is worth finding in a
        // folder full of similarly named files is in the middle of the name.
        if (filter_.empty() ||
            StrStrIW(entries_[i].display_name.c_str(), filter_.c_str()) != nullptr) {
            rows_.push_back(i);
        }
    }
}

// The column is only as wide as the counts actually on screen. Measured again
// whenever the listing changes, or a row that arrived later - "8 folders" where
// the first listing only ever said "3 files" - is drawn clipped.
void TipWindow::MeasureCountColumn() {
    if (!window_) return;

    const HDC dc = GetDC(window_);
    const HGDIOBJ previous = SelectObject(dc, font_);
    int widest = 0;
    for (const int index : rows_) {
        const std::wstring text = DescribeChildCount(entries_[index]);
        if (text.empty()) continue;
        SIZE extent{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
        widest = std::max(widest, static_cast<int>(extent.cx));
    }
    SelectObject(dc, previous);
    ReleaseDC(window_, dc);

    RECT client{};
    GetClientRect(window_, &client);
    // Not at the name's expense past a point: the name is what is being read.
    count_column_ = std::min<int>(widest, (client.right - client.left) * 45 / 100);
}

void TipWindow::SetFilter(const std::wstring& text) {
    filter_ = text;
    ApplyFilter();
    MeasureCountColumn();
    scroll_ = 0;
    // Land on the first match, so Enter and Right act on something without
    // any further keys. Selecting deliberately does not arm the dwell timers.
    hover_ = rows_.empty() ? -1 : rows_.front();
    pending_row_ = -1;
    if (window_) {
        KillTimer(window_, kExpandTimerId);
        Relayout();
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void TipWindow::ReplaceEntries(std::vector<ShellEntry> entries, bool truncated) {
    entries_ = std::move(entries);
    truncated_ = truncated;
    // Deliberately not re-measured: the window keeps the width it opened with,
    // since a tip that jumps sideways on a keystroke is worse than a name that
    // ellipsises.
    ApplyFilter();
    MeasureCountColumn();
    scroll_ = 0;
    hover_ = rows_.empty() ? -1 : rows_.front();
    pending_row_ = -1;
    if (window_) {
        KillTimer(window_, kExpandTimerId);
        Relayout();
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void TipWindow::Relayout() {
    // The truncation notice belongs to the unfiltered list; once a filter is
    // on, the tail it refers to has been searched too.
    const int listed = static_cast<int>(rows_.size()) + ((truncated_ && filter_.empty()) ? 1 : 0);
    const int room = (work_area_.bottom - work_area_.top) / std::max(row_height_, 1) - 1;
    visible_rows_ = std::clamp(std::max(listed, 1), 1, std::min(kMaxRows, std::max(room, 1)));
    footer_height_ = filter_.empty() ? 0 : row_height_;

    RECT rect{};
    GetWindowRect(window_, &rect);
    const int height = visible_rows_ * row_height_ + footer_height_ + 2;
    int y = rect.top;
    if (y + height > work_area_.bottom) {
        y = std::max<int>(work_area_.top, work_area_.bottom - height);
    }
    SetWindowPos(window_, HWND_TOPMOST, rect.left, y, rect.right - rect.left, height,
                 SWP_NOACTIVATE);

    const RECT placed{rect.left, y, rect.right, y + height};
    UnionRect(&footprint_, &footprint_, &placed);
}

void TipWindow::ClearHover() {
    if (hover_ == -1) return;
    hover_ = -1;
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void TipWindow::OnMouseMove(POINT client_pt) {
    const int row = RowAtClient(client_pt);

    if (row != hover_) {
        hover_ = row;
        InvalidateRect(window_, nullptr, FALSE);
        if (on_hover_changed_) on_hover_changed_(this);
    }

    // Arm the dwell on the row's own merits, not on the highlight changing.
    // The top row is already selected when a tip opens, so moving onto it is
    // not a change - yet it still has to expand or preview. Keying off
    // pending_row_ also makes a single-child folder reachable by hover alone.
    if (row != pending_row_) {
        KillTimer(window_, kExpandTimerId);
        pending_row_ = row;
        // A folder expands, a file previews - both after the same short dwell.
        if (row >= 0) SetTimer(window_, kExpandTimerId, kExpandDelayMs, nullptr);
    }

    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
    TrackMouseEvent(&track);
}

const ShellEntry* TipWindow::Selected() const {
    if (hover_ < 0 || hover_ >= static_cast<int>(entries_.size())) return nullptr;
    if (PositionOf(hover_) < 0) return nullptr;
    return &entries_[hover_];
}

void TipWindow::EnsureVisible(int index) {
    const int position = PositionOf(index);
    if (position < 0) return;
    if (position < scroll_) {
        scroll_ = position;
    } else if (position >= scroll_ + visible_rows_) {
        scroll_ = position - visible_rows_ + 1;
    }
    const int max_scroll = std::max(0, static_cast<int>(rows_.size()) - visible_rows_);
    scroll_ = std::clamp(scroll_, 0, max_scroll);
}

void TipWindow::SetSelection(int index) {
    if (rows_.empty()) return;
    int position = PositionOf(index);
    if (position < 0) position = 0;  // no longer listed: fall back to the top
    const int clamped = rows_[std::clamp(position, 0, static_cast<int>(rows_.size()) - 1)];
    if (clamped == hover_) return;

    hover_ = clamped;
    EnsureVisible(hover_);
    // Keyboard selection must not trigger the hover dwell; Right and Enter are
    // the only ways it acts.
    KillTimer(window_, kExpandTimerId);
    pending_row_ = -1;
    InvalidateRect(window_, nullptr, FALSE);
    if (on_hover_changed_) on_hover_changed_(this);
}

void TipWindow::MoveSelection(int delta) {
    if (rows_.empty()) return;
    const int position = PositionOf(hover_);
    const int next = std::clamp(position < 0 ? 0 : position + delta, 0,
                                static_cast<int>(rows_.size()) - 1);
    SetSelection(rows_[next]);
}

void TipWindow::OnWheel(int delta) {
    const int rows = static_cast<int>(rows_.size());
    if (rows <= visible_rows_) return;
    const int max_scroll = rows - visible_rows_;
    const int previous = scroll_;
    scroll_ = std::clamp(scroll_ - (delta / WHEEL_DELTA) * 3, 0, max_scroll);
    if (scroll_ != previous) {
        hover_ = -1;
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void TipWindow::OnPaint() {
    PAINTSTRUCT ps{};
    const HDC dc = BeginPaint(window_, &ps);

    RECT client{};
    GetClientRect(window_, &client);

    // Double buffered: these rows repaint on every pointer move.
    const HDC memory = CreateCompatibleDC(dc);
    const HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    const HGDIOBJ old_bitmap = SelectObject(memory, bitmap);

    const Palette palette = PaletteFor(dark_);

    const HBRUSH background = CreateSolidBrush(palette.background);
    FillRect(memory, &client, background);
    DeleteObject(background);

    const HBRUSH border = CreateSolidBrush(palette.border);
    FrameRect(memory, &client, border);
    DeleteObject(border);

    const HGDIOBJ old_font = SelectObject(memory, font_);
    SetBkMode(memory, TRANSPARENT);

    const int listed = static_cast<int>(rows_.size());
    for (int slot = 0; slot < visible_rows_; ++slot) {
        const int position = scroll_ + slot;
        RECT row{1, 1 + slot * row_height_, client.right - 1, 1 + (slot + 1) * row_height_};

        const int index = EntryAt(position);
        if (index < 0) {
            const wchar_t* note = nullptr;
            if (listed == 0 && position == 0) {
                note = L"no matches";
            } else if (truncated_ && filter_.empty() && position == listed) {
                note = L"more items not shown";
            }
            if (note) {
                SetTextColor(memory, palette.dim);
                RECT text = row;
                text.left += text_left_;
                DrawTextW(memory, note, -1, &text,
                          DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            continue;
        }

        ShellEntry& entry = entries_[index];
        if (entry.icon_index == kIconDeferred) {
            // Resolved as it is drawn rather than as the folder was read: at
            // most a screenful of rows ever needs one.
            entry.icon_index = entry.parsing_path.empty() ? -1 : IconIndexFor(entry.parsing_path);
        }

        if (index == hover_) {
            const HBRUSH highlight = CreateSolidBrush(palette.hover);
            FillRect(memory, &row, highlight);
            DeleteObject(highlight);
        }

        if (entry.icon_index >= 0) {
            ImageList_Draw(SystemSmallImageList(), entry.icon_index, memory, pad_,
                           row.top + (row_height_ - Scale(icon_size_, dpi_)) / 2, ILD_TRANSPARENT);
        }

        const std::wstring count = DescribeChildCount(entry);

        SetTextColor(memory, palette.text);
        RECT text = row;
        text.left += text_left_;
        text.right -= chevron_ + pad_;
        if (count_column_ > 0) text.right -= count_column_ + Scale(kCountGapDip, dpi_);
        DrawTextW(memory, entry.display_name.c_str(), -1, &text,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (!count.empty()) {
            SetTextColor(memory, palette.dim);
            RECT counts = row;
            counts.right = client.right - pad_ - chevron_;
            counts.left = counts.right - count_column_;
            DrawTextW(memory, count.c_str(), -1, &counts,
                      DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        }

        if (entry.is_folder && entry.has_children) {
            // A small chevron, drawn rather than glyphed so it needs no font.
            const HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(1, dpi_)), palette.dim);
            const HGDIOBJ old_pen = SelectObject(memory, pen);
            const int cx = client.right - pad_ - chevron_ / 2;
            const int cy = (row.top + row.bottom) / 2;
            const int arm = Scale(3, dpi_);
            const POINT chevron[3] = {{cx - arm / 2, cy - arm}, {cx + arm / 2, cy}, {cx - arm / 2, cy + arm}};
            Polyline(memory, chevron, 3);
            SelectObject(memory, old_pen);
            DeleteObject(pen);
        }
    }

    if (footer_height_ > 0) {
        // The tip never has focus, so nothing else would tell the user why the
        // list just shrank. This says what was typed and how much survived it.
        RECT footer{1, client.bottom - footer_height_ - 1, client.right - 1, client.bottom - 1};
        const HBRUSH band = CreateSolidBrush(palette.hover);
        FillRect(memory, &footer, band);
        DeleteObject(band);

        RECT text = footer;
        text.left += pad_;
        SetTextColor(memory, palette.text);
        const std::wstring typed = L"Filter: " + filter_;
        DrawTextW(memory, typed.c_str(), -1, &text,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        RECT tally = footer;
        tally.right -= pad_;
        SetTextColor(memory, palette.dim);
        const std::wstring counts =
            std::to_wstring(rows_.size()) + L" of " + std::to_wstring(entries_.size());
        DrawTextW(memory, counts.c_str(), -1, &tally,
                  DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }

    SelectObject(memory, old_font);
    BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);

    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(window_, &ps);
}

LRESULT CALLBACK TipWindow::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wparam, lparam);
    }

    auto* self = reinterpret_cast<TipWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) return DefWindowProcW(window, message, wparam, lparam);

    switch (message) {
        case WM_MOUSEACTIVATE:
            // Never take focus from Explorer.
            return MA_NOACTIVATE;

        case WM_PAINT:
            self->OnPaint();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_LBUTTONDOWN:
            self->drag_origin_ = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            self->maybe_drag_ = self->hover_ >= 0;
            return 0;

        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            // Held and moved past the system threshold: this is a drag, not a
            // click, and the row becomes a real shell drag source.
            if (self->maybe_drag_ && (wparam & MK_LBUTTON)) {
                if (std::abs(pt.x - self->drag_origin_.x) > GetSystemMetrics(SM_CXDRAG) ||
                    std::abs(pt.y - self->drag_origin_.y) > GetSystemMetrics(SM_CYDRAG)) {
                    self->maybe_drag_ = false;
                    const int row = self->hover_;
                    if (row >= 0 && self->on_drag_) self->on_drag_(self, row);
                    return 0;
                }
            }
            self->OnMouseMove(pt);
            return 0;
        }

        case WM_RBUTTONUP:
            if (self->hover_ >= 0 && self->on_context_) self->on_context_(self, self->hover_);
            return 0;

        case WM_MOUSELEAVE:
            KillTimer(window, kExpandTimerId);
            return 0;

        case WM_MOUSEWHEEL:
            self->OnWheel(GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;

        case WM_TIMER:
            if (wparam == kExpandTimerId) {
                KillTimer(window, kExpandTimerId);
                const int row = self->pending_row_;
                if (row >= 0 && row == self->hover_ && row < static_cast<int>(self->entries_.size())) {
                    if (self->entries_[row].is_folder) {
                        if (self->on_expand_) self->on_expand_(self, row);
                    } else if (self->on_preview_) {
                        self->on_preview_(self, row);
                    }
                }
            }
            return 0;

        case WM_LBUTTONUP:
            self->maybe_drag_ = false;
            if (self->hover_ >= 0 && self->on_activate_) self->on_activate_(self, self->hover_);
            return 0;

        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

}  // namespace ee
