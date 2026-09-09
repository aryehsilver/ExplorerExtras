#include "NavigateUpFeature.h"

#include "../core/ExplorerSession.h"
#include "../core/Logging.h"
#include "../core/Settings.h"

namespace ee {
namespace {

double ElapsedMs(LARGE_INTEGER start) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return (now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
}

}  // namespace

void NavigateUpFeature::OnGesture(const GestureEvent& event) {
    if (event.gesture != Gesture::LeftDoubleClick) return;
    if (!Config().navigateUpOnDoubleClick.load(std::memory_order_relaxed)) return;
    if (!tester_) return;

    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);

    // Cheapest test first: is this even an Explorer window? Roughly 0.2ms, and
    // it rejects every double-click in every other application.
    const HWND under_cursor = WindowFromPoint(event.pt);
    if (!under_cursor) return;
    const HWND frame = GetAncestor(under_cursor, GA_ROOT);
    if (!IsExplorerWindow(frame)) return;

    // Then UI Automation (~1-2ms): empty space, or a row / header / scrollbar?
    const HitResult hit = tester_->Test(event.pt);
    if (hit.kind != ViewHit::EmptySpace) return;

    // Which tab is under the pointer. Derived from the window tree rather than
    // from view visibility: in a multi-tab window several background views are
    // WS_VISIBLE at once, and driving one of those fails with E_FAIL.
    const HWND tab_window = FindTabWindow(under_cursor);

    // Only now the cross-process COM enumeration, once we know we will act.
    const auto tab = ResolveActiveTab(frame, tab_window);
    if (!tab) {
        EE_WARN(L"empty-space double-click on frame=0x%p tab=0x%p but no browser resolved",
                frame, tab_window);
        return;
    }

    const std::wstring folder = GetCurrentFolder(*tab);
    const HRESULT hr = NavigateUp(*tab);
    if (FAILED(hr)) {
        // Expected at a namespace root - there is nowhere above Desktop.
        EE_INFO(L"navigate up declined from '%s', hr=0x%08X (exact=%d, %.2fms)", folder.c_str(), hr,
                tab->matched_pointer_tab ? 1 : 0, ElapsedMs(start));
        return;
    }
    EE_INFO(L"navigated up from '%s' (exact=%d, %.2fms)", folder.c_str(),
            tab->matched_pointer_tab ? 1 : 0, ElapsedMs(start));
}

}  // namespace ee
