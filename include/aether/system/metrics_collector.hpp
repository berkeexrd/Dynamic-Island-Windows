#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace aether::system {

struct SystemMetrics {
    float cpuUsagePercent{0.0f};
    
    uint64_t ramUsedBytes{0};
    uint64_t ramTotalBytes{0};
    float ramUsagePercent{0.0f};

    float gpuUsagePercent{0.0f};
    uint64_t vramUsedBytes{0};
    uint64_t vramTotalBytes{0};

    uint64_t netDownloadBytesPerSec{0};
    uint64_t netUploadBytesPerSec{0};

    bool hasBattery{false};
    bool isCharging{false};
    float batteryPercent{100.0f};

    uint64_t uptimeSeconds{0};

    // History buffers for sparkline rendering (e.g. last 30 samples)
    std::vector<float> cpuHistory;
    std::vector<float> ramHistory;
    std::vector<float> netDownHistory;
    std::vector<float> netUpHistory;
    std::vector<float> gpuHistory;
};

class MetricsCollector {
public:
    static MetricsCollector& Instance() {
        static MetricsCollector instance;
        return instance;
    }

    void Start(uint32_t intervalMs = 1000);
    void Stop();
    void SetPaused(bool paused);

    [[nodiscard]] SystemMetrics GetMetrics() const;

private:
    MetricsCollector() = default;
    ~MetricsCollector() { Stop(); }

    void WorkerLoop();
    void QueryCpu();
    void QueryMemory();
    void QueryNetwork();
    void QueryBattery();
    void QueryUptime();
    void QueryGpu();
    void QueryGpuLoad();
    void ShutdownPdh();

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    uint32_t m_intervalMs{1000};
    std::thread m_workerThread;

    mutable std::mutex m_mutex;
    SystemMetrics m_metrics;

    // CPU sampling state
    uint64_t m_prevIdleTime{0};
    uint64_t m_prevKernelTime{0};
    uint64_t m_prevUserTime{0};

    // Network sampling state
    uint64_t m_prevInOctets{0};
    uint64_t m_prevOutOctets{0};
    uint64_t m_prevNetworkSampleTick{0};

    // GPU load via PDH ("GPU Engine" counters)
    void* m_pdhQuery{nullptr};
    void* m_pdhCounter{nullptr};
    bool m_pdhTried{false};
};

} // namespace aether::system
