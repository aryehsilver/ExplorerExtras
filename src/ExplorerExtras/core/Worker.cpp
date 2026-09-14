#include "Worker.h"

#include <objbase.h>

#include <algorithm>
#include <new>
#include <string>
#include <vector>

#include "ExplorerSession.h"
#include "Logging.h"
#include "RecentFolders.h"
#include "Settings.h"

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

        case kMsgOpenFolder: {
            auto* path = reinterpret_cast<std::wstring*>(lparam);
            if (path) {
                if (self) self->subfolder_tip_.Dismiss();
                OpenRecentFolder(*path, wparam != 0);
                delete path;
            }
            return 0;
        }

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

// Two seconds between looks. Folders do not change that fast, and each look is
// a cross-process enumeration of every Explorer window.
constexpr int kPollTicks = 2000 / kTipTickMs;

void Worker::PollOpenFolders() {
    if (!Config().rememberRecentFolders.load(std::memory_order_relaxed)) return;
    // Cheapest possible gate: no Explorer window, nothing to enumerate. Worth
    // it because this runs forever, including all the hours nobody is
    // browsing anything.
    if (!FindWindowW(L"CabinetWClass", nullptr)) return;

    LARGE_INTEGER start, frequency;
    QueryPerformanceCounter(&start);

    std::vector<std::wstring> open = OpenFolders();

    // Only what is new since the last look. Noting everything every time would
    // shuffle two open windows past each other on every poll, and rewrite the
    // file for it - twice a second, forever, for a list nobody changed.
    for (const std::wstring& folder : open) {
        const bool seen = std::any_of(last_seen_.begin(), last_seen_.end(),
                                      [&folder](const std::wstring& previous) {
                                          return CompareStringOrdinal(previous.c_str(), -1,
                                                                      folder.c_str(), -1,
                                                                      TRUE) == CSTR_EQUAL;
                                      });
        if (!seen) Recents().Note(folder);
    }
    last_seen_ = std::move(open);
    Recents().Save();

    // This shares a thread with the hover tick and with anything a drag is
    // doing, so it has no business being slow. Silent unless it is.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const double elapsed = (now.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
    if (elapsed > 50.0) EE_INFO(L"recent folders: the look round took %.0fms", elapsed);
}

void Worker::OnTick() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;

    // Cheap enough for every tick, and it has to be: the answer is wanted at
    // the moment the user opens a menu, long after they left Explorer.
    NoteForegroundExplorer(GetForegroundWindow());

    if (++ticks_since_poll_ >= kPollTicks) {
        ticks_since_poll_ = 0;
        PollOpenFolders();
    }

    if (cursor.x == last_tick_pt_.x && cursor.y == last_tick_pt_.y) {
        still_ms_ += kTipTickMs;
    } else {
        still_ms_ = 0;
        last_tick_pt_ = cursor;
    }

    subfolder_tip_.OnTick(cursor, still_ms_);
}

}  // namespace ee
