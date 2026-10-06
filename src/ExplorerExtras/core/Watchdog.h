// Watchdog.h - notices when one of our own threads stops answering.
//
// Twice a thread here has stopped pumping messages and stayed stopped. All
// anyone saw was the app quietly gone hours later: Windows closes a hung
// program the next time something touches one of its windows, and the report
// it files says that and nothing more - no thread, no stack.
//
// So a thread of its own asks the others the question Windows asks - will you
// take a message? - once a second. Ten seconds without an answer and it writes
// down which thread it is and where it is stuck, by function name wherever the
// symbols are to hand. Thirty, and it hands over to whoever started it, which
// for the tray host means a fresh copy of the app in place of this one.
#pragma once

#include <windows.h>

#include <functional>
#include <vector>

namespace ee {

class Watchdog {
public:
    struct Target {
        const wchar_t* name = nullptr;  // for the log: "main", "worker"
        HWND window = nullptr;          // any window the thread owns and pumps
    };

    // Runs on the watchdog's thread once a target has gone unanswered long
    // enough to give up on it. Whatever it does, it must not need the stuck
    // thread - and very likely it should not need the log either, whose lock
    // the stuck thread may be holding.
    using Recover = std::function<void(const wchar_t* thread_name)>;

    Watchdog() = default;
    ~Watchdog();

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    bool Start(std::vector<Target> targets, Recover recover);
    void Stop();

private:
    struct Watched {
        Target target;
        DWORD thread_id = 0;
        ULONGLONG silent_since = 0;  // 0 while it answers
        bool reported = false;
        bool recovered = false;
    };

    static DWORD WINAPI ThreadMain(LPVOID param);
    void Run();
    void Probe(Watched& watched);
    void Report(const Watched& watched, ULONGLONG silent_ms);

    std::vector<Watched> watched_;
    Recover recover_;
    HANDLE thread_ = nullptr;
    HANDLE stop_ = nullptr;
};

}  // namespace ee
