#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include <windows.h>

namespace aether::system {

struct GamingModeChangedEvent {
    bool isGamingActive{false};
    std::wstring foregroundProcessName;
};

class GamingModeDetector {
public:
    static GamingModeDetector& Instance() {
        static GamingModeDetector instance;
        return instance;
    }

    void Start(uint32_t pollingIntervalMs = 1500);
    void Stop();

    [[nodiscard]] bool IsGaming() const { return m_isGaming; }
    [[nodiscard]] bool IsFullscreenWindow(HWND hwnd) const;

private:
    GamingModeDetector() = default;
    ~GamingModeDetector() { Stop(); }

    void WorkerLoop();
    bool CheckFullscreenState();

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_isGaming{false};
    uint32_t m_intervalMs{1500};
    std::thread m_workerThread;
};

} // namespace aether::system
