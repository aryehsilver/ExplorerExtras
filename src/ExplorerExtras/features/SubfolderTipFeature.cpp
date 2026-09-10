#include "SubfolderTipFeature.h"

#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>

#include "../core/ExplorerSession.h"
#include "../core/KeyboardHook.h"
#include "../core/Logging.h"
#include "../core/Settings.h"
#include "../core/ShellItems.h"
#include "../core/ShellMenu.h"

namespace ee {
namespace {

constexpr DWORD kDwellMs = 400;    // rest this long before anything appears
constexpr DWORD kGraceMs = 800;    // pointer may stray this long before dismissal
// Straying a few pixels on the way from a row to what it opened is not
// leaving. Every rectangle the pointer is allowed to rest in is grown by this,
// and so is the corridor between them.
constexpr int kSlopDip = 16;
constexpr size_t kMaxEntries = 300;
// A filter searches the whole folder, not the part that fits: typing a name
// that is there and being told there are no matches is worse than a pause.
// Only ever paid once per tip, and only for a folder big enough to truncate.
constexpr size_t kMaxFilterEntries = 20000;
constexpr int kPreviewEdge = 480;  // longest side of the thumbnail, in pixels
constexpr int kMaxProbeAttempts = 8;

// Only the leading part of a tab opens its drop-down, so the rest of the tab
// stays a plain click target. The tinted zone shows which part that is.
constexpr double kTabTriggerFraction = 1.0 / 3.0;
constexpr int kTabStripDip = 60;  // a tab strip is never lower than this

RECT TabTriggerZone(const RECT& tab) {
    RECT zone = tab;
    zone.right = tab.left + static_cast<LONG>((tab.right - tab.left) * kTabTriggerFraction);
    return zone;
}

// What a key would type, without a keyboard layout's shift state: the filter
// matches case insensitively, so an unshifted character is enough.
wchar_t FilterChar(DWORD vk) {
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return static_cast<wchar_t>(L'0' + (vk - VK_NUMPAD0));
    }
    if (vk == VK_SPACE) return L' ';
    const UINT mapped = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR);
    if (mapped == 0) return 0;
    if (mapped & 0x80000000) return 0;  // a dead key has no character of its own
    const wchar_t ch = static_cast<wchar_t>(mapped & 0xFFFF);
    return iswprint(ch) ? ch : 0;
}

double ElapsedMs(LARGE_INTEGER start) {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (now.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
}

// The gap the pointer has to cross between a row and the popup it opened.
// Almost nobody crosses it in a straight line - the pointer clips a corner,
// and on an exact hit test the popup vanishes mid-journey. False when the two
// rectangles touch or overlap, where there is nothing to cross.
bool Corridor(const RECT& from, const RECT& to, RECT* out) {
    const LONG left = std::min(from.left, to.left);
    const LONG right = std::max(from.right, to.right);
    const LONG top = std::min(from.top, to.top);
    const LONG bottom = std::max(from.bottom, to.bottom);

    if (to.left >= from.right) {
        *out = RECT{from.right, top, to.left, bottom};
    } else if (to.right <= from.left) {
        *out = RECT{to.right, top, from.left, bottom};
    } else if (to.top >= from.bottom) {
        *out = RECT{left, from.bottom, right, to.top};
    } else if (to.bottom <= from.top) {
        *out = RECT{left, to.bottom, right, from.top};
    } else {
        return false;
    }
    return true;
}

}  // namespace

void SubfolderTipFeature::Initialize(HINSTANCE instance, ViewHitTester* tester, HWND notify_window,
                                     UINT thumbnail_message) {
    instance_ = instance;
    tester_ = tester;
    thumbnails_.Start(notify_window, thumbnail_message);

    preview_.SetDragCallback([this] {
        if (preview_path_.empty()) return;
        modal_ = true;
        DragShellItem(preview_.Handle(), preview_path_);
        modal_ = false;
        pending_dismiss_ = true;  // the pointer is somewhere else entirely now
    });

    preview_.SetContextMenuCallback([this] {
        if (preview_path_.empty()) return;
        POINT cursor{};
        GetCursorPos(&cursor);
        modal_ = true;
        ShowShellContextMenu(preview_.Handle(), preview_path_, cursor);
        modal_ = false;
        pending_dismiss_ = true;
    });
}

void SubfolderTipFeature::Shutdown() {
    Dismiss();
    preview_.Shutdown();  // releases the retained handler on this thread
    thumbnails_.Stop();
}

void SubfolderTipFeature::UpdateKeyboardCapture() {
    // Only borrow the arrow keys while there is a list to drive.
    KeyboardCaptureEnabled().store(!chain_.empty(), std::memory_order_relaxed);
}

void SubfolderTipFeature::OnTick(POINT cursor, DWORD still_ms) {
    // A context menu or a drag pumps this timer from inside its own modal loop.
    if (modal_) return;

    // Activation destroys the window whose message handler asked for it, so the
    // teardown is deferred to here.
    if (pending_dismiss_) {
        pending_dismiss_ = false;
        Dismiss();
        return;
    }

    if (!Config().enabled.load(std::memory_order_relaxed) ||
        !Config().subfolderTips.load(std::memory_order_relaxed)) {
        if (!chain_.empty() || preview_.Visible()) Dismiss();
        return;
    }

    UpdateDragState(cursor);

    // The tint follows the pointer immediately, not after the dwell - it is
    // what explains why the drop-down is about to appear.
    UpdateHighlight(cursor);

    // Resting anywhere in Explorer that is not one of our own windows opens
    // what is under the pointer - including while something else is already
    // showing, so moving along a list of files swaps the preview rather than
    // waiting for the old one to time out first.
    if (!PointerInsideChain(cursor) && !preview_.ContainsPoint(cursor) && still_ms >= kDwellMs) {
        TryOpenAt(cursor);
    }

    if (chain_.empty() && !preview_.Visible()) return;

    if (PointerInSafeZone(cursor)) {
        outside_ms_ = 0;
        return;
    }

    outside_ms_ += kTipTickMs;
    if (outside_ms_ >= kGraceMs) Dismiss();
}

void SubfolderTipFeature::TryOpenAt(POINT cursor) {
    if (!tester_) return;

    // A stationary pointer keeps satisfying the dwell, so this runs on every
    // tick. Probe each position once - a UIA hit test is not free - and only
    // go again where the answer was unusable.
    if (cursor.x == last_probe_pt_.x && cursor.y == last_probe_pt_.y) {
        if (probe_settled_) return;
    } else {
        last_probe_pt_ = cursor;
        probe_attempts_ = 0;
        probe_settled_ = false;
    }
    if (++probe_attempts_ >= kMaxProbeAttempts) probe_settled_ = true;

    const HWND under_cursor = WindowFromPoint(cursor);
    if (!under_cursor) return;
    const HWND frame = GetAncestor(under_cursor, GA_ROOT);
    if (!IsExplorerWindow(frame)) return;

    const HitResult hit = tester_->Test(cursor);
    // Anything except a row that would not give its name is a real answer.
    if (hit.kind != ViewHit::Item || !hit.item_name.empty()) probe_settled_ = true;

    // A window tab drops its folder down, the same list, anchored beneath.
    if (hit.kind == ViewHit::Tab) {
        // The active tab is already showing; and only the leading third opens,
        // so hovering on the way to a click does nothing.
        if (hit.tab_selected) return;
        const RECT zone = TabTriggerZone(hit.item_rect);
        if (!PtInRect(&zone, cursor)) return;
        TryOpenTab(frame, hit, zone);
        return;
    }

    // A row whose name did not come back: Explorer's automation provider is
    // busy, which it reliably is while a drag is under way, and the point is
    // left open to the remaining attempts rather than written off after one.
    if (hit.kind != ViewHit::Item || hit.item_name.empty()) {
        if (hit.kind == ViewHit::Item && probe_settled_) {
            EE_INFO(L"probe: the row at (%ld,%ld) would not name itself in %d attempts", cursor.x,
                    cursor.y, probe_attempts_);
        }
        return;
    }

    const auto tab = ResolveActiveTab(frame, FindTabWindow(under_cursor));
    if (!tab) return;

    const std::wstring folder = GetCurrentFolder(*tab);
    if (folder.empty()) return;

    std::wstring child_path;
    bool is_folder = false;
    if (!ResolveChild(folder, hit.item_name, &child_path, &is_folder)) {
        EE_INFO(L"hover: could not resolve '%s' in '%s'", hit.item_name.c_str(), folder.c_str());
        return;
    }
    if (child_path == source_path_) return;  // already showing for this row

    // Something else is under the pointer now, so what is on screen belongs to
    // where the pointer has been rather than where it is.
    if (!chain_.empty() || preview_.Visible()) {
        Dismiss();
        last_probe_pt_ = cursor;  // Dismiss clears it, and this point is probed
    }
    EE_INFO(L"hover: '%s' folder=%d", child_path.c_str(), is_folder ? 1 : 0);

    // The row reports its logical width, which can exceed the visible view, so
    // clamp before anchoring or the popup lands off to the side of nothing.
    RECT anchor = hit.item_rect;
    if (hit.view_rect.right > hit.view_rect.left) {
        anchor.left = std::max(anchor.left, hit.view_rect.left);
        anchor.right = std::min(anchor.right, hit.view_rect.right);
    }

    source_row_ = anchor;
    source_path_ = child_path;
    source_tab_ = *tab;
    outside_ms_ = 0;

    if (!is_folder) {
        // Mid-drag a preview is worse than nothing: a drop cannot land in one,
        // and it would cover whatever the drag was heading for.
        if (!dragging_) RequestPreview(child_path, anchor);
        return;
    }

    LARGE_INTEGER enumerate_start;
    QueryPerformanceCounter(&enumerate_start);
    bool truncated = false;
    std::vector<ShellEntry> entries = EnumerateFolder(child_path, kMaxEntries, &truncated);
    EE_INFO(L"enumerated %zu entries of '%s' in %.0fms", entries.size(), child_path.c_str(),
            ElapsedMs(enumerate_start));
    if (entries.empty() && !truncated) return;  // empty folder: nothing to show

    auto tip = std::make_unique<TipWindow>();
    if (!tip->Create(instance_, std::move(entries), truncated, anchor)) return;

    tip->SetFolder(child_path);
    Wire(tip.get());
    chain_.push_back(std::move(tip));
    UpdateKeyboardCapture();
}

// Watched rather than hooked: the mouse hook stays off the input path, and a
// drag is only ever a button that went down somewhere and a pointer that has
// since travelled. Whether it is a real OLE drag or a rubber-band selection
// does not matter - neither wants a preview opening under it.
void SubfolderTipFeature::UpdateDragState(POINT cursor) {
    const bool held = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (!held) {
        dragging_ = false;
        button_down_ = false;
        return;
    }
    if (!button_down_) {
        button_down_ = true;
        button_origin_ = cursor;
        return;
    }
    if (!dragging_ && (std::abs(cursor.x - button_origin_.x) > GetSystemMetrics(SM_CXDRAG) ||
                       std::abs(cursor.y - button_origin_.y) > GetSystemMetrics(SM_CYDRAG))) {
        dragging_ = true;
        EE_INFO(L"drag in progress: previews suppressed, tips still open");
    }
}

void SubfolderTipFeature::UpdateHighlight(POINT cursor) {
    // While anything is open, mark what it came from. The pointer has moved
    // away into the popup by then, so Explorer's own hover highlight is gone
    // and without this the source row looks unrelated to what is on screen.
    if (!chain_.empty() || preview_.Visible()) {
        if (from_tab_) {
            highlight_.Show(instance_, tab_zone_, HighlightStyle::TabTrigger);
        } else {
            highlight_.Show(instance_, source_row_, HighlightStyle::Row);
        }
        return;
    }
    if (!tester_ || !Config().subfolderTips.load(std::memory_order_relaxed)) {
        highlight_.Hide();
        return;
    }

    const HWND under_cursor = WindowFromPoint(cursor);
    const HWND frame = under_cursor ? GetAncestor(under_cursor, GA_ROOT) : nullptr;
    if (!IsExplorerWindow(frame)) {
        highlight_.Hide();
        return;
    }

    // Cheap gate before spending a UIA hit test: tab strips live at the top of
    // the frame, so anywhere lower cannot be one.
    RECT frame_rect{};
    GetWindowRect(frame, &frame_rect);
    if (cursor.y > frame_rect.top + MulDiv(kTabStripDip, GetDpiForWindow(frame), 96)) {
        highlight_.Hide();
        return;
    }

    const HitResult hit = tester_->Test(cursor);
    if (hit.kind != ViewHit::Tab || hit.tab_selected) {
        highlight_.Hide();
        return;
    }

    const RECT zone = TabTriggerZone(hit.item_rect);
    if (!PtInRect(&zone, cursor)) {
        highlight_.Hide();
        return;
    }
    highlight_.Show(instance_, zone, HighlightStyle::TabTrigger);
}

void SubfolderTipFeature::TryOpenTab(HWND frame, const HitResult& hit, const RECT& zone) {
    if (hit.item_name.empty()) return;

    const std::wstring folder = FindTabFolder(frame, hit.item_name);
    if (folder.empty()) {
        EE_INFO(L"tab hover: no folder matched tab '%s'", hit.item_name.c_str());
        return;
    }
    if (folder == source_path_) return;
    if (!chain_.empty() || preview_.Visible()) {
        POINT cursor{};
        GetCursorPos(&cursor);
        Dismiss();
        last_probe_pt_ = cursor;  // Dismiss clears it, and this point is probed
    }

    bool truncated = false;
    std::vector<ShellEntry> entries = EnumerateFolder(folder, kMaxEntries, &truncated);
    if (entries.empty() && !truncated) return;

    auto tip = std::make_unique<TipWindow>();
    if (!tip->Create(instance_, std::move(entries), truncated, hit.item_rect,
                     TipPlacement::Below)) {
        return;
    }

    tip->SetFolder(folder);
    Wire(tip.get());
    chain_.push_back(std::move(tip));

    source_row_ = hit.item_rect;
    source_path_ = folder;
    tab_zone_ = zone;
    from_tab_ = true;
    // Used only as the site for "open in new tab"; the visible view is the
    // right answer here since the pointer is on the tab strip, not in a view.
    if (const auto active = ResolveActiveTab(frame, nullptr)) source_tab_ = *active;
    outside_ms_ = 0;
    UpdateKeyboardCapture();
    EE_INFO(L"tab hover: '%s' -> '%s'", hit.item_name.c_str(), folder.c_str());
}

void SubfolderTipFeature::OpenChild(size_t parent_depth, const std::wstring& folder_path,
                                    const RECT& anchor) {
    CloseFrom(parent_depth + 1);

    bool truncated = false;
    std::vector<ShellEntry> entries = EnumerateFolder(folder_path, kMaxEntries, &truncated);
    if (entries.empty() && !truncated) return;

    auto tip = std::make_unique<TipWindow>();
    if (!tip->Create(instance_, std::move(entries), truncated, anchor)) return;

    tip->SetFolder(folder_path);
    Wire(tip.get());
    chain_.push_back(std::move(tip));
    outside_ms_ = 0;
    UpdateKeyboardCapture();
}

void SubfolderTipFeature::RequestPreview(const std::wstring& path, const RECT& anchor) {
    if (!Config().filePreviews.load(std::memory_order_relaxed)) return;

    // Deliberately not hidden here: ShowHandler reuses the window it is already
    // in, and destroying it first would defeat that. Each path below takes the
    // old preview down itself.
    ++preview_token_;  // anything still in flight is now stale
    preview_anchor_ = anchor;
    preview_path_ = path;

    // Media first: nothing registers a preview handler for it, and a thumbnail
    // of a video is a still frame rather than a preview.
    if (Config().mediaPlayback.load(std::memory_order_relaxed) && MediaPreview::IsMedia(path)) {
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        const bool autoplay = Config().mediaAutoPlay.load(std::memory_order_relaxed);
        const bool opened = preview_.ShowMedia(instance_, path, anchor, autoplay);
        EE_INFO(L"preview: media for '%s' opened=%d autoplay=%d (%.0fms)", path.c_str(),
                opened ? 1 : 0, autoplay ? 1 : 0, ElapsedMs(start));
        if (opened) return;
    }

    // A registered handler gives a scrollable PDF or syntax-highlighted code.
    // Thumbnails are the fallback, and all an image needs.
    CLSID clsid{};
    if (PreviewHandlerHost::FindHandler(path, &clsid)) {
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        const bool shown = preview_.ShowHandler(instance_, path, anchor);
        EE_INFO(L"preview: handler for '%s' shown=%d (%.0fms)", path.c_str(), shown ? 1 : 0,
                ElapsedMs(start));
        if (shown) return;
    }

    // A thumbnail arrives asynchronously, so take whatever is on screen down
    // now rather than leaving a stale preview up while it is fetched.
    preview_.Hide();
    EE_INFO(L"preview: thumbnail for '%s' token=%llu", path.c_str(),
            static_cast<unsigned long long>(preview_token_));
    thumbnails_.Request(path, kPreviewEdge, preview_token_);
}

void SubfolderTipFeature::OnThumbnail(uint64_t token, HBITMAP bitmap) {
    EE_INFO(L"preview: thumbnail token=%llu (expecting %llu) bitmap=%s",
            static_cast<unsigned long long>(token),
            static_cast<unsigned long long>(preview_token_), bitmap ? L"yes" : L"null");

    if (token != preview_token_) {
        // The pointer moved on while this was being extracted.
        if (bitmap) DeleteObject(bitmap);
        return;
    }
    if (!bitmap) return;  // no thumbnail for this file type; show nothing

    const bool shown = preview_.Show(instance_, bitmap, preview_path_, preview_anchor_);
    EE_INFO(L"preview: window shown=%d", shown ? 1 : 0);
}

void SubfolderTipFeature::OnKey(DWORD virtual_key) {
    if (chain_.empty()) return;
    TipWindow* top = chain_.back().get();

    // A key is activity: whatever the pointer is doing, this is not abandonment.
    outside_ms_ = 0;

    switch (virtual_key) {
        case VK_DOWN:
            top->MoveSelection(1);
            break;

        case VK_UP:
            top->MoveSelection(-1);
            break;

        case VK_RIGHT: {
            const ShellEntry* selected = top->Selected();
            if (selected && selected->is_folder && !selected->parsing_path.empty()) {
                OpenChild(DepthOf(top), selected->parsing_path, top->RowRect(top->Selection()));
            }
            break;
        }

        case VK_LEFT:
            // Back to the parent level, or out altogether at the first one.
            if (chain_.size() > 1) {
                CloseFrom(chain_.size() - 1);
                UpdateKeyboardCapture();
            } else {
                Dismiss();
            }
            break;

        case VK_RETURN: {
            const ShellEntry* selected = top->Selected();
            if (selected) {
                Activate(*selected);
                pending_dismiss_ = true;
            }
            break;
        }

        case VK_ESCAPE:
            // A filter is undone first: Escape backs out one thing at a time.
            if (!top->Filter().empty()) {
                top->SetFilter(L"");
            } else {
                Dismiss();
            }
            break;

        default:
            OnFilterKey(virtual_key);
            break;
    }
}

// Typing narrows the level the pointer is on. A long folder is otherwise only
// navigable by scrolling, and the tip has no other use for these keys.
void SubfolderTipFeature::OnFilterKey(DWORD virtual_key) {
    TipWindow* top = chain_.back().get();
    std::wstring filter = top->Filter();

    if (virtual_key == VK_BACK) {
        if (filter.empty()) return;
        filter.pop_back();
    } else {
        const wchar_t ch = FilterChar(virtual_key);
        if (!ch) return;
        if (ch == L' ' && filter.empty()) return;  // a leading space matches everything
        filter.push_back(ch);
    }

    // Whatever was open came from a row that may not survive the filter.
    CloseFrom(DepthOf(top) + 1);
    preview_.Dismiss();
    ++preview_token_;
    top->SetFilter(filter);
    UpdateKeyboardCapture();

    // A truncated listing was only ever the part that fit on screen. Once the
    // user is searching rather than browsing, read the rest of the folder, so
    // the answer is about the folder and not about the first 300 of it. After
    // the filter has been applied, not before: the keystroke has to land at
    // once, and this is the one slow thing a keystroke can set off.
    if (top->Truncated() && !filter.empty() && !top->Folder().empty()) {
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        bool truncated = false;
        std::vector<ShellEntry> full =
            EnumerateFolder(top->Folder(), kMaxFilterEntries, &truncated, /*defer_icons=*/true);
        EE_INFO(L"filter: re-read '%s' in full, %zu entries in %.0fms", top->Folder().c_str(),
                full.size(), ElapsedMs(start));
        if (!full.empty()) top->ReplaceEntries(std::move(full), truncated);
    }
}

void SubfolderTipFeature::Activate(const ShellEntry& entry) {
    if (entry.parsing_path.empty()) return;

    if (entry.is_folder) {
        // Needs the originating tab as its site, or the verb opens a whole new
        // window instead of a tab.
        const HRESULT hr = OpenInNewTab(source_tab_, entry.parsing_path);
        if (SUCCEEDED(hr)) return;
        EE_INFO(L"open in new tab failed hr=0x%08X, falling back to the default verb", hr);
    }

    ShellExecuteW(nullptr, nullptr, entry.parsing_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void SubfolderTipFeature::Wire(TipWindow* tip) {
    tip->SetHoverChangedCallback([this](TipWindow* source) {
        // Moving to a different row closes anything opened from the old one.
        CloseFrom(DepthOf(source) + 1);
        preview_.Dismiss();
        ++preview_token_;
        UpdateKeyboardCapture();
    });

    tip->SetExpandCallback([this](TipWindow* source, int index) {
        const ShellEntry& entry = source->entries()[index];
        if (!entry.is_folder || entry.parsing_path.empty()) return;
        OpenChild(DepthOf(source), entry.parsing_path, source->RowRect(index));
    });

    tip->SetPreviewCallback([this](TipWindow* source, int index) {
        const ShellEntry& entry = source->entries()[index];
        if (entry.parsing_path.empty()) return;
        RequestPreview(entry.parsing_path, source->RowRect(index));
    });

    tip->SetActivateCallback([this](TipWindow* source, int index) {
        Activate(source->entries()[index]);
        // Deferred: this runs inside |source|'s window procedure.
        pending_dismiss_ = true;
    });

    tip->SetContextMenuCallback([this](TipWindow* source, int index) {
        const ShellEntry& entry = source->entries()[index];
        if (entry.parsing_path.empty()) return;
        POINT cursor{};
        GetCursorPos(&cursor);
        modal_ = true;
        ShowShellContextMenu(source->Handle(), entry.parsing_path, cursor);
        modal_ = false;
        pending_dismiss_ = true;
    });

    tip->SetSpringCallback([this](TipWindow* source, int index) {
        // A drag resting on a folder opens it, so the file being carried can be
        // taken down through the levels and dropped at the bottom.
        const ShellEntry& entry = source->entries()[index];
        if (!entry.is_folder || entry.parsing_path.empty()) return;
        OpenChild(DepthOf(source), entry.parsing_path, source->RowRect(index));
    });

    tip->SetDroppedCallback([this](TipWindow*) {
        // Dropped: the pointer is wherever it let go and there is nothing left
        // to browse. Deferred, since this runs inside the drop.
        pending_dismiss_ = true;
    });

    tip->SetDragCallback([this](TipWindow* source, int index) {
        const ShellEntry& entry = source->entries()[index];
        if (entry.parsing_path.empty()) return;
        modal_ = true;
        DragShellItem(source->Handle(), entry.parsing_path);
        modal_ = false;
        pending_dismiss_ = true;
    });
}

void SubfolderTipFeature::CloseFrom(size_t depth) {
    while (chain_.size() > depth) chain_.pop_back();
}

size_t SubfolderTipFeature::DepthOf(const TipWindow* window) const {
    for (size_t i = 0; i < chain_.size(); ++i) {
        if (chain_[i].get() == window) return i;
    }
    return 0;
}

// Everything on screen, the routes between those things, and a margin round
// the lot. Anywhere else, and the pointer has genuinely gone somewhere else.
bool SubfolderTipFeature::PointerInSafeZone(POINT screen_pt) const {
    UINT dpi = 96;
    if (!chain_.empty()) {
        dpi = GetDpiForWindow(chain_.front()->Handle());
    } else if (preview_.Handle()) {
        dpi = GetDpiForWindow(preview_.Handle());
    }
    const int slop = MulDiv(kSlopDip, static_cast<int>(dpi), 96);

    const auto within = [slop, screen_pt](RECT rect) {
        InflateRect(&rect, slop, slop);
        return PtInRect(&rect, screen_pt) != FALSE;
    };

    if (!IsRectEmpty(&source_row_) && within(source_row_)) return true;

    RECT corridor{};
    for (const auto& tip : chain_) {
        // The footprint, not the window: filtering shrinks a tip, and the
        // pointer that was resting inside where it used to be has not moved.
        const RECT rect = tip->FootprintRect();
        if (within(rect)) return true;
        if (Corridor(tip->AnchorRect(), rect, &corridor) && within(corridor)) return true;
    }

    if (preview_.Visible()) {
        RECT rect{};
        GetWindowRect(preview_.Handle(), &rect);
        if (within(rect)) return true;
        if (Corridor(preview_anchor_, rect, &corridor) && within(corridor)) return true;
    }
    return false;
}

bool SubfolderTipFeature::PointerInsideChain(POINT screen_pt) const {
    for (const auto& tip : chain_) {
        if (tip->ContainsPoint(screen_pt)) return true;
    }
    return false;
}

void SubfolderTipFeature::OnExternalClick(POINT screen_pt) {
    if (modal_) return;
    if (chain_.empty() && !preview_.Visible()) return;
    // Our own windows handle their own clicks.
    if (PointerInsideChain(screen_pt) || preview_.ContainsPoint(screen_pt)) return;
    Dismiss();
}

void SubfolderTipFeature::ResetPreviewHandler() {
    preview_.Shutdown();
    EE_INFO(L"preview handler released on request");
}

void SubfolderTipFeature::Dismiss() {
    chain_.clear();
    preview_.Dismiss();
    highlight_.Hide();
    ++preview_token_;
    source_row_ = RECT{};
    source_path_.clear();
    tab_zone_ = RECT{};
    from_tab_ = false;
    last_probe_pt_ = POINT{-1, -1};
    outside_ms_ = 0;
    UpdateKeyboardCapture();
}

}  // namespace ee
