// ViewHitTest.h - "what is under this screen point?" for an Explorer file list.
//
// Uses UI Automation, which is the supported out-of-process way to hit-test
// Explorer's DirectUI view. Must be created and used on an STA thread.
#pragma once

#include <windows.h>
// objbase.h must precede uiautomation.h: the project defines
// WIN32_LEAN_AND_MEAN, so windows.h does not pull in the OLE headers that
// define the `interface` macro UIAutomationCore.h relies on.
#include <objbase.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <string>

namespace ee {

enum class ViewHit {
    None,        // not the file list (nav pane, toolbar, preview pane, another app...)
    EmptySpace,  // inside the file list but not on an item
    Item,        // on a file or folder row
    Tab,         // on one of the window's tabs
};

struct HitResult {
    ViewHit kind = ViewHit::None;
    std::wstring item_name;  // display name, only when kind == Item
    // Logical row bounds. A row reports its full width even when the view is
    // narrower, so intersect with |view_rect| before using this to place UI.
    RECT item_rect{};
    RECT view_rect{};        // the items view, when it was reached
    bool tab_selected = false;  // only meaningful when kind == Tab
};

class ViewHitTester {
public:
    HRESULT Initialize();
    void Reset();

    HitResult Test(POINT screen_pt);

    // Human-readable ancestor chain at a point, for the diagnostics menu item.
    std::wstring DescribeChain(POINT screen_pt);

private:
    Microsoft::WRL::ComPtr<IUIAutomation> uia_;
    Microsoft::WRL::ComPtr<IUIAutomationTreeWalker> walker_;
};

}  // namespace ee
