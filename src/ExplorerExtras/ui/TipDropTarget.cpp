#include "TipDropTarget.h"

#include "../core/Logging.h"
#include "../core/ShellItems.h"
#include "TipWindow.h"

using Microsoft::WRL::ComPtr;

namespace ee {

void TipDropTarget::Detach() {
    owner_ = nullptr;
    ReleaseTarget();
    data_.Reset();
}

HRESULT STDMETHODCALLTYPE TipDropTarget::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDropTarget)) {
        *object = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE TipDropTarget::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&references_));
}

ULONG STDMETHODCALLTYPE TipDropTarget::Release() {
    const LONG remaining = InterlockedDecrement(&references_);
    if (remaining == 0) delete this;
    return static_cast<ULONG>(remaining);
}

void TipDropTarget::ReleaseTarget() {
    if (target_ && entered_) target_->DragLeave();
    entered_ = false;
    target_.Reset();
    target_path_.clear();
}

void TipDropTarget::Retarget(POINT screen_pt, IDataObject* data, DWORD key_state, DWORD* effect) {
    if (!owner_ || !data) {
        *effect = DROPEFFECT_NONE;
        return;
    }

    // A folder row takes the drop itself; a file row or the padding between
    // them means the folder the tip is listing, which is what the user sees
    // the list as being.
    const int row = owner_->RowAtScreen(screen_pt);
    std::wstring path;
    bool on_row = false;
    if (row >= 0 && row < static_cast<int>(owner_->entries().size())) {
        const ShellEntry& entry = owner_->entries()[row];
        if (entry.is_folder && !entry.parsing_path.empty()) {
            path = entry.parsing_path;
            on_row = true;
        }
    }
    if (path.empty()) path = owner_->Folder();

    owner_->SetDropState(true, on_row ? row : -1);

    if (path.empty()) {
        ReleaseTarget();
        *effect = DROPEFFECT_NONE;
        return;
    }
    if (target_ && path == target_path_) return;  // same folder as the last move

    ReleaseTarget();

    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        *effect = DROPEFFECT_NONE;
        return;
    }
    // The shell's own drop target for the folder. Everything a drop means -
    // copy against move, what the modifier keys do, which drop handlers run -
    // is its business and not ours.
    if (FAILED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target_)))) {
        target_.Reset();
        *effect = DROPEFFECT_NONE;
        return;
    }

    target_path_ = path;
    const POINTL pt{screen_pt.x, screen_pt.y};
    if (SUCCEEDED(target_->DragEnter(data, key_state, pt, effect))) {
        entered_ = true;
    } else {
        target_.Reset();
        target_path_.clear();
        *effect = DROPEFFECT_NONE;
    }
}

HRESULT STDMETHODCALLTYPE TipDropTarget::DragEnter(IDataObject* data, DWORD key_state, POINTL pt,
                                                   DWORD* effect) {
    if (!effect) return E_POINTER;
    data_ = data;

    // The drag image is the source's, and it has to keep following the pointer
    // across our window or the drag looks like it has been dropped already.
    if (!helper_) {
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&helper_));
    }
    POINT screen_pt{pt.x, pt.y};
    if (helper_ && owner_) helper_->DragEnter(owner_->Handle(), data, &screen_pt, *effect);

    Retarget(screen_pt, data, key_state, effect);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TipDropTarget::DragOver(DWORD key_state, POINTL pt, DWORD* effect) {
    if (!effect) return E_POINTER;
    POINT screen_pt{pt.x, pt.y};
    if (helper_) helper_->DragOver(&screen_pt, *effect);

    Retarget(screen_pt, data_.Get(), key_state, effect);
    if (target_ && entered_) {
        target_->DragOver(key_state, pt, effect);
    } else {
        *effect = DROPEFFECT_NONE;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TipDropTarget::DragLeave() {
    if (helper_) helper_->DragLeave();
    ReleaseTarget();
    data_.Reset();
    if (owner_) owner_->SetDropState(false, -1);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TipDropTarget::Drop(IDataObject* data, DWORD key_state, POINTL pt,
                                              DWORD* effect) {
    if (!effect) return E_POINTER;
    POINT screen_pt{pt.x, pt.y};
    if (helper_) helper_->Drop(data, &screen_pt, *effect);

    Retarget(screen_pt, data, key_state, effect);

    HRESULT hr = S_OK;
    if (target_ && entered_) {
        hr = target_->Drop(data, key_state, pt, effect);
        entered_ = false;  // Drop consumes the enter; DragLeave must not follow
        EE_INFO(L"drop onto '%s' hr=0x%08X effect=%lu", target_path_.c_str(), hr, *effect);
    } else {
        *effect = DROPEFFECT_NONE;
    }

    ReleaseTarget();
    data_.Reset();
    if (owner_) {
        owner_->SetDropState(false, -1);
        owner_->NotifyDropped();
    }
    return hr;
}

}  // namespace ee
