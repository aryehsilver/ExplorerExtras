#include "Worker.h"

#include <objbase.h>

#include <new>

#include "Logging.h"

namespace ee {
namespace {
constexpr wchar_t kWorkerClassName[] = L"ExplorerExtras.Worker";
constexpr UINT_PTR kTickTimerId = 1;
}  // namespace

Worker::~Worker() {
    Stop();
}

bool Worker::Start() {
    if (thread_) return true;

    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready_) return false;

    thread_ = CreateThread(nullptr, 0, &Worker::ThreadMain, this, 0, &thread_id_);
    if (!thread_) {
        CloseHandle(ready_);
        ready_ = nullptr;
        return false;
    }

    WaitForSingleObject(ready_, 10000);
    CloseHandle(ready_);
    ready_ = nullptr;

    if (!window_) {
        EE_ERR(L"worker thread failed to start");
        Stop();
        return false;
    }
    return true;
}

void Worker::Stop() {
    if (thread_) {
        PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
        if (WaitForSingleObject(thread_, 5000) == WAIT_TIMEOUT) {
            EE_WARN(L"worker thread did not exit in time");
        }
        CloseHandle(thread_);
        thread_ = nullptr;
        thread_id_ = 0;
    }
    if (ready_) {
        CloseHandle(ready_);
        ready_ = nullptr;
    }
}

void Worker::RequestDiagnostics(POINT pt) {
    if (!window_) return;
    auto* copy = new (std::nothrow) POINT{pt};
    if (!copy) return;
    if (!PostMessageW(window_, kMsgDiagnostics, 0, reinterpret_cast<LPARAM>(copy))) {
        delete copy;
    }
}

DWORD WINAPI Worker::ThreadMain(LPVOID param) {
    static_cast<Worker*>(param)->Run();
    return 0;
}

void Worker::Run() {
    // Shell COM is apartment-threaded. From an MTA, IShellBrowser calls return
    // S_OK but produce null window handles. OleInitialize rather than
    // CoInitializeEx because drag-and-drop needs full OLE on the thread.
    const HRESULT hr = OleInitialize(nullptr);
    if (FAILED(hr)) {
        EE_ERR(L"OleInitialize failed on worker, hr=0x%08X", hr);
        SetEvent(ready_);
        return;
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Worker::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWorkerClassName;
    RegisterClassExW(&wc);

    window_ = CreateWindowExW(0, kWorkerClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              instance, nullptr);
    if (window_) {
        SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        if (FAILED(hit_tester_.Initialize())) {
            EE_ERR(L"UI Automation unavailable; hit testing disabled");
        }
        navigate_up_.Initialize(&hit_tester_);
        subfolder_tip_.Initialize(instance, &hit_tester_, window_, kMsgThumbnail);
        // Polling rather than mouse messages: the hook must stay off the input
        // path, and a tick is what notices the pointer leaving as well.
        SetTimer(window_, kTickTimerId, kTipTickMs, nullptr);
        EE_INFO(L"worker ready (STA)");
    }

    SetEvent(ready_);

    if (window_) {
        MSG msg;
        BOOL result;
        while ((result = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
            if (result == -1) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        KillTimer(window_, kTickTimerId);
        subfolder_tip_.Shutdown();
        DestroyWindow(window_);
        window_ = nullptr;
    }

    hit_tester_.Reset();
    ShutdownMediaFoundation();
    OleUninitialize();
    EE_INFO(L"worker stopped");
}

LRESULT CALLBACK Worker::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<Worker*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
        case kMsgGesture: {
            auto* event = reinterpret_cast<GestureEvent*>(lparam);
            if (event) {
                if (self) {
                    if (event->gesture == Gesture::LeftClick) {
                        // Clicking anything that is not ours puts the tips away.
                        self->subfolder_tip_.OnExternalClick(event->pt);
                    } else {
                        self->subfolder_tip_.Dismiss();
                        self->navigate_up_.OnGesture(*event);
                    }
                }
                delete event;
            }
            return 0;
        }
        case kMsgDiagnostics: {
            auto* pt = reinterpret_cast<POINT*>(lparam);
            if (pt) {
                if (self) {
                    const HWND under = WindowFromPoint(*pt);
                    const HWND frame = under ? GetAncestor(under, GA_ROOT) : nullptr;
                    EE_INFO(L"diagnostics at (%ld,%ld):\r\n%s%s", pt->x, pt->y,
                            self->hit_tester_.DescribeChain(*pt).c_str(),
                            IsExplorerWindow(frame) ? DescribeTabs(frame).c_str() : L"");
                }
                delete pt;
            }
            return 0;
        }
        case kMsgKey:
            self->subfolder_tip_.OnKey(static_cast<DWORD>(wparam));
            return 0;

        case kMsgThumbnail:
            self->subfolder_tip_.OnThumbnail(static_cast<uint64_t>(wparam),
                                             reinterpret_cast<HBITMAP>(lparam));
            return 0;

        case kMsgResetPreview:
            self->subfolder_tip_.ResetPreviewHandler();
            return 0;

        case WM_TIMER:
            if (wparam == kTickTimerId) self->OnTick();
            return 0;

        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

void Worker::OnTick() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;

    if (cursor.x == last_tick_pt_.x && cursor.y == last_tick_pt_.y) {
        still_ms_ += kTipTickMs;
    } else {
        still_ms_ = 0;
        last_tick_pt_ = cursor;
    }

    subfolder_tip_.OnTick(cursor, still_ms_);
}

}  // namespace ee
