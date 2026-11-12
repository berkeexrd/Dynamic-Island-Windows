#include "aether/system/gaming_mode.hpp"
#include "aether/core/log.hpp"
#include "aether/core/events.hpp"
#include <shellapi.h>
#include <psapi.h>

#pragma comment(lib, "shell32.lib")

namespace aether::system {

void GamingModeDetector::Start(uint32_t pollingIntervalMs) {
    if (m_running) return;
    m_intervalMs = pollingIntervalMs;
    m_running = true;
    m_workerThread = std::thread(&GamingModeDetector::WorkerLoop, this);
    AETHER_LOG_INFO("GamingModeDetector started (interval: " << pollingIntervalMs << "ms)");
}

void GamingModeDetector::Stop() {
    if (!m_running) return;
    m_running = false;
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    AETHER_LOG_INFO("GamingModeDetector stopped");
}

bool GamingModeDetector::IsFullscreenWindow(HWND hwnd) const {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }

    // Ignore shell and desktop windows
    wchar_t className[256] = {0};
    GetClassNameW(hwnd, className, 255);
    if (wcscmp(className, L"Progman") == 0 ||
        wcscmp(className, L"WorkerW") == 0 ||
        wcscmp(className, L"Shell_TrayWnd") == 0 ||
        wcscmp(className, L"Shell_SecondaryTrayWnd") == 0) {
        return false;
    }

    RECT appRect;
    if (!GetWindowRect(hwnd, &appRect)) return false;

    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return false;

    // Check if window bounds cover the entire monitor
    bool coversMonitor = (appRect.left <= mi.rcMonitor.left &&
                          appRect.top <= mi.rcMonitor.top &&
                          appRect.right >= mi.rcMonitor.right &&
                          appRect.bottom >= mi.rcMonitor.bottom);

    if (coversMonitor) {
        LONG style = GetWindowLongW(hwnd, GWL_STYLE);
        // Exclusive fullscreen or borderless window
        if ((style & WS_POPUP) || !(style & WS_CAPTION)) {
            return true;
        }
    }
    return false;
}

bool GamingModeDetector::CheckFullscreenState() {
    // 1. Check Windows Notification State (reports QUNS_RUNNING_D3D_FULL_SCREEN)
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE) {
            return true;
        }
    }

    // 2. Check foreground window bounds
    HWND foreground = GetForegroundWindow();
    if (foreground && IsFullscreenWindow(foreground)) {
        return true;
    }

    return false;
}

void GamingModeDetector::WorkerLoop() {
    bool lastState = false;

    while (m_running) {
        bool currentState = CheckFullscreenState();

        if (currentState != lastState) {
            lastState = currentState;
            m_isGaming = currentState;

            std::wstring procName = L"Unknown";
            HWND fg = GetForegroundWindow();
            if (fg) {
                DWORD pid = 0;
                GetWindowThreadProcessId(fg, &pid);
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (hProc) {
                    wchar_t path[MAX_PATH] = {0};
                    DWORD size = MAX_PATH;
                    if (QueryFullProcessImageNameW(hProc, 0, path, &size)) {
                        procName = path;
                    }
                    CloseHandle(hProc);
                }
            }

            AETHER_LOG_INFO("Gaming Mode state transition: " << (currentState ? "ACTIVATED" : "DEACTIVATED"));
            core::EventBus::Instance().Publish(GamingModeChangedEvent{currentState, procName});
        }

        for (uint32_t elapsed = 0; elapsed < m_intervalMs && m_running; elapsed += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace aether::system
