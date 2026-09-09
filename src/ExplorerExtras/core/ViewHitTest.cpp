#include "ViewHitTest.h"

#include <objbase.h>
#include <oleauto.h>

#include <cstdio>

#include "Logging.h"

using Microsoft::WRL::ComPtr;

namespace ee {
namespace {

// Explorer's items view. Landing on this element itself - rather than on one of
// its ListItem children - is what "empty space" means.
constexpr wchar_t kItemsViewClass[] = L"UIItemsView";

// Chrome that lives inside or beside the items view and already has its own
// double-click behaviour (auto-fit a column, collapse a group). Treat a hit on
// any of these as "not empty space" so we never steal that gesture.
constexpr const wchar_t* kRejectClasses[] = {
    L"UIColumnHeader",
    L"UIGroupHeader",
};

// A row is nested a few levels below the cell that ElementFromPoint returns
// (UIProperty -> ListItem), so the walk stays shallow.
constexpr int kMaxWalkDepth = 10;

std::wstring GetClassNameOf(IUIAutomationElement* element) {
    BSTR raw = nullptr;
    if (FAILED(element->get_CurrentClassName(&raw)) || !raw) return {};
    std::wstring value(raw, SysStringLen(raw));
    SysFreeString(raw);
    return value;
}

std::wstring GetNameOf(IUIAutomationElement* element) {
    BSTR raw = nullptr;
    if (FAILED(element->get_CurrentName(&raw)) || !raw) return {};
    std::wstring value(raw, SysStringLen(raw));
    SysFreeString(raw);
    return value;
}

bool EqualsOrdinal(const std::wstring& a, const wchar_t* b) {
    return CompareStringOrdinal(a.c_str(), -1, b, -1, FALSE) == CSTR_EQUAL;
}

bool IsRejectedClass(const std::wstring& class_name) {
    for (const wchar_t* reject : kRejectClasses) {
        if (EqualsOrdinal(class_name, reject)) return true;
    }
    return false;
}

bool IsRejectedControlType(CONTROLTYPEID type) {
    return type == UIA_ScrollBarControlTypeId || type == UIA_HeaderControlTypeId ||
           type == UIA_HeaderItemControlTypeId;
}

}  // namespace

HRESULT ViewHitTester::Initialize() {
    // __uuidof(CUIAutomation) rather than CLSID_CUIAutomation: the coclass is
    // declared in the header, so this needs no import library.
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&uia_));
    if (FAILED(hr)) {
        EE_ERR(L"CoCreateInstance(CUIAutomation) failed, hr=0x%08X", hr);
        return hr;
    }
    hr = uia_->get_ControlViewWalker(&walker_);
    if (FAILED(hr)) {
        EE_ERR(L"get_ControlViewWalker failed, hr=0x%08X", hr);
        uia_.Reset();
        return hr;
    }
    return S_OK;
}

void ViewHitTester::Reset() {
    walker_.Reset();
    uia_.Reset();
}

HitResult ViewHitTester::Test(POINT screen_pt) {
    HitResult result;
    if (!uia_ || !walker_) return result;

    ComPtr<IUIAutomationElement> element;
    if (FAILED(uia_->ElementFromPoint(screen_pt, &element)) || !element) return result;

    // ElementFromPoint returns the deepest element - over a row that is the
    // "Name" cell, not the row - so walk up looking for whichever comes first:
    // a ListItem (on an item) or the items view (empty space).
    ComPtr<IUIAutomationElement> current = element;
    for (int depth = 0; depth < kMaxWalkDepth && current; ++depth) {
        CONTROLTYPEID control_type = 0;
        current->get_CurrentControlType(&control_type);
        const std::wstring class_name = GetClassNameOf(current.Get());

        if (IsRejectedClass(class_name) || IsRejectedControlType(control_type)) {
            return result;  // None
        }

        // Windows 11's tab strip is a XAML TabView; its items surface as
        // TabItem, and the name is the folder the tab is showing.
        if (control_type == UIA_TabItemControlTypeId) {
            result.kind = ViewHit::Tab;
            result.item_name = GetNameOf(current.Get());
            current->get_CurrentBoundingRectangle(&result.item_rect);

            // The active tab is already on screen, so it has nothing to offer.
            ComPtr<IUIAutomationSelectionItemPattern> selection;
            if (SUCCEEDED(current->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                                                       IID_PPV_ARGS(&selection))) &&
                selection) {
                BOOL selected = FALSE;
                if (SUCCEEDED(selection->get_CurrentIsSelected(&selected))) {
                    result.tab_selected = selected != FALSE;
                }
            }
            return result;
        }

        if (control_type == UIA_ListItemControlTypeId) {
            result.kind = ViewHit::Item;
            result.item_name = GetNameOf(current.Get());
            current->get_CurrentBoundingRectangle(&result.item_rect);

            // Keep climbing to the items view so callers can clamp the row to
            // what is actually on screen.
            ComPtr<IUIAutomationElement> above = current;
            for (int extra = 0; extra < kMaxWalkDepth; ++extra) {
                ComPtr<IUIAutomationElement> parent;
                if (FAILED(walker_->GetParentElement(above.Get(), &parent)) || !parent) break;
                above = parent;
                if (EqualsOrdinal(GetClassNameOf(above.Get()), kItemsViewClass)) {
                    above->get_CurrentBoundingRectangle(&result.view_rect);
                    break;
                }
            }
            return result;
        }

        if (EqualsOrdinal(class_name, kItemsViewClass)) {
            result.kind = ViewHit::EmptySpace;
            current->get_CurrentBoundingRectangle(&result.view_rect);
            return result;
        }

        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker_->GetParentElement(current.Get(), &parent)) || !parent) break;
        current = parent;
    }

    return result;  // None
}

std::wstring ViewHitTester::DescribeChain(POINT screen_pt) {
    if (!uia_ || !walker_) return L"(UI Automation not initialised)";

    ComPtr<IUIAutomationElement> element;
    if (FAILED(uia_->ElementFromPoint(screen_pt, &element)) || !element) {
        return L"(no element at point)";
    }

    std::wstring out;
    ComPtr<IUIAutomationElement> current = element;
    for (int depth = 0; depth < kMaxWalkDepth && current; ++depth) {
        CONTROLTYPEID control_type = 0;
        current->get_CurrentControlType(&control_type);
        RECT bounds{};
        current->get_CurrentBoundingRectangle(&bounds);

        wchar_t line[512];
        _snwprintf_s(line, _TRUNCATE, L"  [%d] type=%d class='%s' name='%s' rect=(%ld,%ld,%ld,%ld)\r\n",
                     depth, static_cast<int>(control_type), GetClassNameOf(current.Get()).c_str(),
                     GetNameOf(current.Get()).c_str(), bounds.left, bounds.top, bounds.right,
                     bounds.bottom);
        out += line;

        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker_->GetParentElement(current.Get(), &parent)) || !parent) break;
        current = parent;
    }
    return out;
}

}  // namespace ee
