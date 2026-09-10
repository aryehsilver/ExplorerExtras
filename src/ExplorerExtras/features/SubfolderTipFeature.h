// SubfolderTipFeature.h - hover a folder to drill into it, hover a file to see it.
//
// Owns the chain of popups and the file preview. Driven by a periodic tick
// rather than by mouse messages: the hook must stay off the input path, and a
// tick is also what detects the pointer *leaving*, which no single event gives.
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "../core/ExplorerSession.h"
#include "../core/ThumbnailLoader.h"
#include "../core/ViewHitTest.h"
#include "../ui/HighlightWindow.h"
#include "../ui/PreviewWindow.h"
#include "../ui/TipWindow.h"

namespace ee {

// How often SubfolderTipFeature::OnTick is expected to be called.
inline constexpr UINT kTipTickMs = 60;

class SubfolderTipFeature {
public:
    void Initialize(HINSTANCE instance, ViewHitTester* tester, HWND notify_window,
                    UINT thumbnail_message);
    void Shutdown();

    // |still_ms| is how long the pointer has been stationary at |cursor|.
    void OnTick(POINT cursor, DWORD still_ms);

    // Arrow keys, Enter, Escape and anything typed, forwarded by the keyboard
    // hook while a tip is showing.
    void OnKey(DWORD virtual_key);

    // Any click anywhere. A click outside our own windows means the user is
    // doing something else - including clicking the tab we are hanging off.
    void OnExternalClick(POINT screen_pt);

    // A finished thumbnail. Takes ownership of |bitmap|, which may be null.
    void OnThumbnail(uint64_t token, HBITMAP bitmap);

    void Dismiss();

    // Releases the retained preview handler so the next preview starts a fresh
    // one. Recovery for the case where the handler itself has wedged, which we
    // cannot detect from the outside - its calls all still succeed.
    void ResetPreviewHandler();

    bool IsShowing() const { return !chain_.empty(); }

private:
    void TryOpenAt(POINT cursor);
    void TryOpenTab(HWND frame, const HitResult& hit, const RECT& zone);
    void UpdateHighlight(POINT cursor);
    void OpenChild(size_t parent_depth, const std::wstring& folder_path, const RECT& anchor);
    void RequestPreview(const std::wstring& path, const RECT& anchor);
    void CloseFrom(size_t depth);
    size_t DepthOf(const TipWindow* window) const;
    bool PointerInsideChain(POINT screen_pt) const;
    // Where the pointer may be without the tip taking it as having left.
    bool PointerInSafeZone(POINT screen_pt) const;
    void Wire(TipWindow* tip);
    void Activate(const ShellEntry& entry);
    void UpdateKeyboardCapture();
    void OnFilterKey(DWORD virtual_key);
    void UpdateDragState(POINT cursor);

    HINSTANCE instance_ = nullptr;
    ViewHitTester* tester_ = nullptr;

    std::vector<std::unique_ptr<TipWindow>> chain_;
    PreviewWindow preview_;
    HighlightWindow highlight_;
    ThumbnailLoader thumbnails_;

    RECT tab_zone_{};      // the tinted strip of the tab a drop-down came from
    bool from_tab_ = false;
    // A context menu or drag runs its own modal loop, which still pumps our
    // timer; without this the tip would dismiss itself out from under it.
    bool modal_ = false;

    // Someone is dragging something, here or in Explorer. Tips still open, so
    // a drag can walk down into a subfolder and drop there; previews do not,
    // having nowhere for a drop to land and a habit of covering the target.
    bool dragging_ = false;
    bool button_down_ = false;
    POINT button_origin_{};

    RECT source_row_{};         // the Explorer row that opened everything
    std::wstring source_path_;  // what that row points at
    ActiveTab source_tab_;      // the tab it came from; the site for "open in new tab"
    POINT last_probe_pt_{-1, -1};
    int probe_attempts_ = 0;
    bool probe_settled_ = false;
    DWORD outside_ms_ = 0;
    bool pending_dismiss_ = false;

    uint64_t preview_token_ = 0;
    RECT preview_anchor_{};
    std::wstring preview_path_;
};

}  // namespace ee
