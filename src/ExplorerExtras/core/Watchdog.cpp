#include "Watchdog.h"

#include <windows.h>
#include <dbghelp.h>

#include <new>
#include <string>
#include <utility>

#include "Logging.h"
#include "Paths.h"

namespace ee {
namespace {

constexpr DWORD kProbeIntervalMs = 1000;
// How long one probe waits. Windows calls a window hung after five seconds
// without a look at its queue; two is plenty to tell a busy thread from one
// that is not coming back, and keeps a round of probes short.
constexpr UINT kProbeTimeoutMs = 2000;
constexpr ULONGLONG kReportAfterMs = 10 * 1000;
constexpr ULONGLONG kRecoverAfterMs = 30 * 1000;
// The report runs on a thread of its own and gets this long. Walking a stack
// means dbghelp, and dbghelp means locks the stuck thread may be holding - the
// loader lock above all, if what it is stuck in is a DLL being loaded.
constexpr DWORD kReportBudgetMs = 5000;
constexpr int kMaxFrames = 40;

// Time that stops while the machine sleeps. A thread that was slow at the
// moment the lid closed has not been stuck all night.
ULONGLONG NowMs() {
    ULONGLONG unbiased = 0;
    QueryUnbiasedInterruptTime(&unbiased);
    return unbiased / 10000;
}

std::wstring FolderOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// dbghelp is single-threaded, and only ever called from report threads, one
// at a time - a report that overran its budget never finishes, and every one
// after it skips the stack rather than queue behind it.
volatile LONG g_symbols_busy = 0;
bool g_symbols_ready = false;

bool EnsureSymbols(HANDLE process) {
    if (g_symbols_ready) {
        SymRefreshModuleList(process);  // shell extensions arrive late
        return true;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS |
                  SYMOPT_NO_PROMPTS);
    // The executable's own folder, where a local build leaves its .pdb. No
    // symbol server: that is a network fetch, on a timer, in a hang.
    const std::wstring search = FolderOf(ExecutablePath());
    g_symbols_ready = SymInitializeW(process, search.empty() ? nullptr : search.c_str(), TRUE) != FALSE;
    return g_symbols_ready;
}

std::wstring DescribeAddress(HANDLE process, DWORD64 address) {
    IMAGEHLP_MODULEW64 module{};
    module.SizeOfStruct = sizeof(module);
    const bool have_module = SymGetModuleInfoW64(process, address, &module) != FALSE;

    alignas(SYMBOL_INFOW) unsigned char buffer[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;

    wchar_t text[400];
    if (have_module && SymFromAddrW(process, address, &displacement, symbol)) {
        swprintf_s(text, L"%s!%s+0x%llx", module.ModuleName, symbol->Name, displacement);
    } else if (have_module) {
        // No symbols: the offset still pins it down, given the matching .pdb.
        swprintf_s(text, L"%s+0x%llx", module.ModuleName, address - module.BaseOfImage);
    } else {
        swprintf_s(text, L"0x%016llx", address);
    }
    return text;
}

void LogStack(DWORD thread_id) {
#if defined(_M_X64)
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                               FALSE, thread_id);
    if (!thread) {
        EE_WARN(L"watchdog: cannot open the thread to see where it is, gle=%lu", GetLastError());
        return;
    }

    // Suspended only long enough to read its registers: nothing that might
    // allocate happens in between, in case what it is stuck holding is the
    // heap. It is stuck, so its stack stays put for the walk that follows.
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    bool captured = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        captured = GetThreadContext(thread, &context) != FALSE;
        ResumeThread(thread);
    }
    if (!captured) {
        EE_WARN(L"watchdog: cannot read the thread's registers, gle=%lu", GetLastError());
        CloseHandle(thread);
        return;
    }

    if (InterlockedCompareExchange(&g_symbols_busy, 1, 0) != 0) {
        EE_WARN(L"watchdog: an earlier stack walk never finished; not starting another");
        CloseHandle(thread);
        return;
    }

    const HANDLE process = GetCurrentProcess();
    if (!EnsureSymbols(process)) {
        EE_WARN(L"watchdog: symbols unavailable, gle=%lu", GetLastError());
    }

    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    EE_WARN(L"watchdog: where it is stuck, innermost first:");
    for (int i = 0; i < kMaxFrames; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) break;
        EE_WARN(L"watchdog:   %2d  %s", i, DescribeAddress(process, frame.AddrPC.Offset).c_str());
    }

    InterlockedExchange(&g_symbols_busy, 0);
    CloseHandle(thread);
#else
    (void)thread_id;
    EE_WARN(L"watchdog: stack capture is only built for x64");
#endif
}

struct ReportJob {
    std::wstring name;
    DWORD thread_id;
    ULONGLONG silent_ms;
};

DWORD WINAPI ReportThread(LPVOID param) {
    ReportJob* job = static_cast<ReportJob*>(param);
    EE_WARN(L"watchdog: the %s thread (%lu) has not taken a message for %llu s",
            job->name.c_str(), job->thread_id, job->silent_ms / 1000);
    LogStack(job->thread_id);
    delete job;
    return 0;
}

}  // namespace

Watchdog::~Watchdog() {
    Stop();
}

bool Watchdog::Start(std::vector<Target> targets, Recover recover) {
    if (thread_) return true;

    watched_.clear();
    for (const Target& target : targets) {
        if (!target.window) continue;
        Watched watched;
        watched.target = target;
        watched.thread_id = GetWindowThreadProcessId(target.window, nullptr);
        watched_.push_back(watched);
    }
    recover_ = std::move(recover);

    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_) return false;
    thread_ = CreateThread(nullptr, 0, &Watchdog::ThreadMain, this, 0, nullptr);
    if (!thread_) {
        CloseHandle(stop_);
        stop_ = nullptr;
        return false;
    }
    return true;
}

void Watchdog::Stop() {
    if (thread_) {
        SetEvent(stop_);
        // A probe in flight can hold it up for one probe timeout, no more.
        WaitForSingleObject(thread_, kProbeTimeoutMs + 3000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stop_) {
        CloseHandle(stop_);
        stop_ = nullptr;
    }
}

DWORD WINAPI Watchdog::ThreadMain(LPVOID param) {
    static_cast<Watchdog*>(param)->Run();
    return 0;
}

void Watchdog::Run() {
    while (WaitForSingleObject(stop_, kProbeIntervalMs) == WAIT_TIMEOUT) {
        for (Watched& watched : watched_) {
            if (WaitForSingleObject(stop_, 0) != WAIT_TIMEOUT) return;
            Probe(watched);
        }
    }
}

void Watchdog::Probe(Watched& watched) {
    // WM_NULL does nothing at all, so answering it proves only that the thread
    // looked at its queue - which is precisely the question. A modal loop, a
    // menu, a drag, a COM call waiting on another process: all of them still
    // look, and none of them is mistaken for a hang.
    DWORD_PTR ignored = 0;
    if (SendMessageTimeoutW(watched.target.window, WM_NULL, 0, 0, SMTO_NORMAL, kProbeTimeoutMs,
                            &ignored)) {
        if (watched.silent_since && watched.reported) {
            EE_INFO(L"watchdog: the %s thread is answering again after %llu s", watched.target.name,
                    (NowMs() - watched.silent_since) / 1000);
        }
        watched.silent_since = 0;
        watched.reported = false;
        watched.recovered = false;
        return;
    }
    // Anything but a timeout means the window has gone: we are shutting down.
    if (GetLastError() != ERROR_TIMEOUT) return;

    const ULONGLONG now = NowMs();
    // It had already gone the whole probe without answering.
    if (!watched.silent_since) watched.silent_since = now - kProbeTimeoutMs;
    const ULONGLONG silent = now - watched.silent_since;

    if (!watched.reported && silent >= kReportAfterMs) {
        watched.reported = true;
        Report(watched, silent);
    }
    if (!watched.recovered && silent >= kRecoverAfterMs) {
        watched.recovered = true;
        if (recover_) recover_(watched.target.name);
    }
}

void Watchdog::Report(const Watched& watched, ULONGLONG silent_ms) {
    // Not on this thread: if the report gets stuck behind something the hung
    // thread holds - the log's lock, the loader lock - the recovery still has
    // to happen on time. A report that overruns is left where it is.
    auto* job = new (std::nothrow) ReportJob{watched.target.name, watched.thread_id, silent_ms};
    if (!job) return;
    HANDLE thread = CreateThread(nullptr, 0, &ReportThread, job, 0, nullptr);
    if (!thread) {
        delete job;
        return;
    }
    WaitForSingleObject(thread, kReportBudgetMs);
    CloseHandle(thread);
}

}  // namespace ee
