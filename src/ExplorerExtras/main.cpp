// Explorer Extras - QTTabBar-style conveniences for File Explorer on Windows 11.
//
// Runs as an ordinary user-level process. Nothing is injected into explorer.exe:
// a low-level mouse hook recognises the gesture, and the shell's own automation
// and browser interfaces do the rest. If this process dies, Explorer is
// untouched.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdlib>
#include <cwchar>
#include <string>

#include "core/Logging.h"
#include "core/Paths.h"
#include "host/TrayHost.h"

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    // Started by the watchdog in place of a copy that hung:
    //   --replace <pid of that copy> --stuck <which of its threads>
    DWORD replaced_pid = 0;
    std::wstring stuck_thread;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; ++i) {
            if (wcscmp(argv[i], L"--replace") == 0) {
                replaced_pid = wcstoul(argv[++i], nullptr, 10);
            } else if (wcscmp(argv[i], L"--stuck") == 0) {
                stuck_thread = argv[++i];
            }
        }
        LocalFree(argv);
    }
    // It ends itself as soon as we are started; wait until it has, or the
    // check below would find it still here and leave nobody running.
    if (replaced_pid) {
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, replaced_pid)) {
            WaitForSingleObject(old, 30000);
            CloseHandle(old);
        }
    }

    // One instance per session; two hooks would navigate up twice.
    HANDLE single_instance = CreateMutexW(nullptr, TRUE, L"Local\\ExplorerExtras.SingleInstance");
    if (single_instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(single_instance);
        return 0;
    }

    // The manifest already asks for PerMonitorV2; this is belt and braces for
    // the case where the manifest is stripped.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    ee::log::Init();
    EE_INFO(L"Explorer Extras starting, exe=%s", ee::ExecutablePath().c_str());
    if (replaced_pid) {
        EE_WARN(L"started in place of pid %lu, whose %s thread stopped responding - see the "
                L"watchdog lines above for where",
                replaced_pid, stuck_thread.empty() ? L"?" : stuck_thread.c_str());
    }

    // The tray thread does shell work of its own - the icon, ShellExecute, and
    // enumerating what Explorer has open to build the recent folders menu - and
    // all of it wants an apartment. The worker has its own, this is the main
    // thread's. Apartment-threaded, like every thread here that touches the
    // shell: from an MTA the browser interfaces hand back null handles.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) EE_ERR(L"CoInitializeEx failed on the main thread, hr=0x%08X", com);

    int exit_code = 0;
    {
        ee::TrayHost host;
        if (host.Start(instance, replaced_pid != 0)) {
            exit_code = host.RunMessageLoop();
        } else {
            EE_ERR(L"startup failed");
            exit_code = 1;
        }
        host.Stop();
    }

    if (SUCCEEDED(com)) CoUninitialize();

    EE_INFO(L"Explorer Extras stopped");
    ee::log::Shutdown();

    if (single_instance) {
        ReleaseMutex(single_instance);
        CloseHandle(single_instance);
    }
    return exit_code;
}
