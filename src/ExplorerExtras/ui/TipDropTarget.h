// TipDropTarget.h - a tip is somewhere you can drop files.
//
// The tip lists folders, and the point of listing them is that they are where
// things go. This makes each row a real drop target: drag a file out of the
// view, hover a tab or a folder, walk down the levels as they open, and let go.
// No navigation, no second window, and the file never leaves the view it is in
// until it lands.
//
// Nothing here decides what a drop *means*. The row's own shell drop target
// does, reached through IShellItem::BindToHandler, so copy versus move, the
// modifier keys, .lnk and .zip targets and every drop hook the user has
// installed all behave exactly as they do in Explorer. This class only decides
// which item is under the pointer and forwards.
//
// Requires OleInitialize on the thread that creates the window, and the window
// must be registered with RegisterDragDrop - both true of the worker.
#pragma once

#include <shlobj.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

namespace ee {

class TipWindow;

class TipDropTarget : public IDropTarget {
public:
    explicit TipDropTarget(TipWindow* owner) : owner_(owner) {}

    // The window is going away. OLE may still hold a reference and can still
    // call in, so the owner is dropped rather than the object.
    void Detach();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDropTarget
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD key_state, POINTL pt,
                                        DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD key_state, POINTL pt, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD key_state, POINTL pt,
                                   DWORD* effect) override;

private:
    // Binds the shell's drop target for whatever |pt| is over: the folder on
    // that row, or the folder the tip is listing when the row is a file or the
    // pointer is on the padding. Re-entered as the pointer moves between rows.
    void Retarget(POINT screen_pt, IDataObject* data, DWORD key_state, DWORD* effect);
    void ReleaseTarget();

    TipWindow* owner_ = nullptr;
    LONG references_ = 1;

    Microsoft::WRL::ComPtr<IDataObject> data_;
    Microsoft::WRL::ComPtr<IDropTarget> target_;   // the shell's, for target_path_
    Microsoft::WRL::ComPtr<IDropTargetHelper> helper_;
    std::wstring target_path_;
    bool entered_ = false;  // whether target_ has had DragEnter and owes a DragLeave
};

}  // namespace ee
