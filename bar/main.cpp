#include <windows.h>
#include <shellscalingapi.h>
#include <cwchar>
#include "aether/bar/aether_bar.hpp"

using namespace aether::bar;

// Safety net: whatever happens, never leave the user without a taskbar.
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS*) {
    ShellTaskbarGuard::Instance().Restore();
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // AetherBar.exe --restore  : emergency switch to bring the stock Windows taskbar back
    if (cmdLine && wcsstr(cmdLine, L"--restore")) {
        ShellTaskbarGuard::Instance().Restore();
        return 0;
    }

    // Only one bar at a time
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Aether.Bar.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetUnhandledExceptionFilter(CrashHandler);
    atexit([] { ShellTaskbarGuard::Instance().Restore(); });

    AetherBar bar;
    if (!bar.Create()) {
        ShellTaskbarGuard::Instance().Restore();
        MessageBoxW(nullptr, L"Could not create the Aether Bar window.", L"Aether Bar", MB_ICONERROR);
        return 1;
    }
    const int rc = bar.Run();
    if (mutex) CloseHandle(mutex);
    return rc;
}
