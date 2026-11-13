#include <windows.h>
#include <shellapi.h>
#include <cstdlib>
#include <objbase.h>
#include <shellscalingapi.h>
#include <chrono>
#include <optional>
#include <cwchar>
#include <memory>
#include <vector>
#include "aether/bar/aether_bar.hpp"
#include "aether/bar/start_menu.hpp"
#include "aether/core/monitors.hpp"

using namespace aether::bar;

namespace {

constexpr UINT kMsgToggleStart = WM_APP + 1;   // Win key tapped
constexpr UINT kMsgRebuild     = WM_APP + 2;   // monitors / DPI changed
constexpr UINT kMsgReapplyHide = WM_APP + 3;   // Explorer restarted

DWORD g_mainThread = 0;
HHOOK g_hook = nullptr;
bool  g_winDown = false;
bool  g_combo = false;

// Win key tap (press + release with nothing in between) opens Aether Start instead of the stock one.
// Win+E, Win+D, Win+B ... still work because those count as a combo.
LRESULT CALLBACK KeyboardHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (!(k->flags & LLKHF_INJECTED)) {
            const bool isWin = (k->vkCode == VK_LWIN || k->vkCode == VK_RWIN);
            const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
            if (isWin) {
                if (down) {
                    if (!g_winDown) { g_winDown = true; g_combo = false; }
                } else {
                    if (g_winDown && !g_combo) {
                        // Inject an unassigned key so Windows treats this as Win+<key> and skips its own Start
                        INPUT in[2] = {};
                        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = 0xE8;
                        in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 0xE8; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                        SendInput(2, in, sizeof(INPUT));
                        PostThreadMessageW(g_mainThread, kMsgToggleStart, 0, 0);
                    }
                    g_winDown = false;
                }
            } else if (down && g_winDown) {
                g_combo = true;
            }
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

class BarManager {
public:
    explicit BarManager(bool hardHide) : m_hard(hardHide) {}

    bool Init() {
        if (!m_start.Create()) return false;
        m_start.PreloadAsync();
        ShellTaskbarGuard::Instance().Hide(m_hard);
        Rebuild();
        return !m_bars.empty();
    }

    void Rebuild() {
        m_bars.clear();
        for (HMONITOR mon : aether::core::EnumerateMonitors()) {
            MONITORINFO mi = {sizeof(mi)};
            GetMonitorInfoW(mon, &mi);
            auto bar = std::make_unique<AetherBar>(mon, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, &m_start);
            if (bar->Create()) m_bars.push_back(std::move(bar));
        }
    }

    void ToggleStart() {
        POINT pt; GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        int reserved = static_cast<int>(74.0f * aether::core::MonitorScale(mon));
        for (auto& b : m_bars) if (b->Monitor() == mon) reserved = b->ReservedPx();
        m_start.Toggle(mon, reserved);
    }

    void ReapplyHide() { ShellTaskbarGuard::Instance().Hide(m_hard); }

    bool Frame(float dt) {
        if (m_rebuildAt && std::chrono::steady_clock::now() >= *m_rebuildAt) { m_rebuildAt.reset(); Rebuild(); }
        bool animating = false;
        for (auto& b : m_bars) animating |= b->Frame(dt);
        animating |= m_start.Frame(dt);
        return animating || m_rebuildAt.has_value();
    }

    void ScheduleRebuild() { m_rebuildAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(600); }

private:
    bool m_hard;
    StartMenu m_start;
    std::vector<std::unique_ptr<AetherBar>> m_bars;
    std::optional<std::chrono::steady_clock::time_point> m_rebuildAt;
};

LONG WINAPI CrashHandler(EXCEPTION_POINTERS*) {
    ShellTaskbarGuard::Instance().Restore();
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // AetherBar.exe --restore : emergency switch to bring the stock Windows taskbar back
    if (cmdLine && wcsstr(cmdLine, L"--restore")) {
        ShellTaskbarGuard::Instance().Restore();
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Aether.Bar.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetUnhandledExceptionFilter(CrashHandler);
    atexit([] { ShellTaskbarGuard::Instance().Restore(); });

    const bool hard = cmdLine && wcsstr(cmdLine, L"--hide-stock");
    const bool useHook = !(cmdLine && wcsstr(cmdLine, L"--no-start-hook"));

    int rc = 0;
    {
        BarManager manager(hard);
        if (!manager.Init()) {
            ShellTaskbarGuard::Instance().Restore();
            MessageBoxW(nullptr, L"Could not create the Aether Bar window.", L"Aether Bar", MB_ICONERROR);
            return 1;
        }

        g_mainThread = GetCurrentThreadId();
        if (useHook) g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, GetModuleHandleW(nullptr), 0);

        LARGE_INTEGER freq, last;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&last);
        bool running = true;
        while (running) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { running = false; break; }
                if (!msg.hwnd) {
                    if (msg.message == kMsgToggleStart) { manager.ToggleStart(); continue; }
                    if (msg.message == kMsgRebuild)     { manager.ScheduleRebuild(); continue; }
                    if (msg.message == kMsgReapplyHide) { manager.ReapplyHide(); continue; }
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!running) break;

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart);
            last = now;
            dt = dt > 0.1f ? 0.1f : dt;

            if (!manager.Frame(dt)) MsgWaitForMultipleObjectsEx(0, nullptr, 120, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        if (g_hook) UnhookWindowsHookEx(g_hook);
    }
    ShellTaskbarGuard::Instance().Restore();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return rc;
}
