#include "aether/system/metrics_collector.hpp"
#include "aether/core/log.hpp"
#include "aether/core/events.hpp"
#include <algorithm>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "dxgi.lib")

namespace aether::system {

struct MetricsUpdatedEvent {
    SystemMetrics metrics;
};

void MetricsCollector::Start(uint32_t intervalMs) {
    if (m_running) return;
    m_intervalMs = intervalMs;
    m_running = true;
    m_paused = false;

    // Initialize histories
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_metrics.cpuHistory.assign(30, 0.0f);
        m_metrics.ramHistory.assign(30, 0.0f);
        m_metrics.netDownHistory.assign(30, 0.0f);
    }

    m_workerThread = std::thread(&MetricsCollector::WorkerLoop, this);
    AETHER_LOG_INFO("MetricsCollector started with interval " << intervalMs << "ms");
}

void MetricsCollector::Stop() {
    if (!m_running) return;
    m_running = false;
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    AETHER_LOG_INFO("MetricsCollector stopped");
}

void MetricsCollector::SetPaused(bool paused) {
    m_paused = paused;
}

SystemMetrics MetricsCollector::GetMetrics() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_metrics;
}

static uint64_t FileTimeToUInt64(const FILETIME& ft) {
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart;
}

void MetricsCollector::QueryCpu() {
    FILETIME idleTime, kernelTime, userTime;
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) return;

    uint64_t idle = FileTimeToUInt64(idleTime);
    uint64_t kernel = FileTimeToUInt64(kernelTime);
    uint64_t user = FileTimeToUInt64(userTime);

    if (m_prevKernelTime != 0) {
        uint64_t idleDiff = idle - m_prevIdleTime;
        uint64_t kernelDiff = kernel - m_prevKernelTime;
        uint64_t userDiff = user - m_prevUserTime;
        uint64_t totalDiff = kernelDiff + userDiff;

        if (totalDiff > 0) {
            float usage = (1.0f - static_cast<float>(idleDiff) / static_cast<float>(totalDiff)) * 100.0f;
            m_metrics.cpuUsagePercent = std::clamp(usage, 0.0f, 100.0f);
        }
    }

    m_prevIdleTime = idle;
    m_prevKernelTime = kernel;
    m_prevUserTime = user;

    if (!m_metrics.cpuHistory.empty()) {
        m_metrics.cpuHistory.erase(m_metrics.cpuHistory.begin());
        m_metrics.cpuHistory.push_back(m_metrics.cpuUsagePercent);
    }
}

void MetricsCollector::QueryMemory() {
    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        m_metrics.ramTotalBytes = memStatus.ullTotalPhys;
        m_metrics.ramUsedBytes = memStatus.ullTotalPhys - memStatus.ullAvailPhys;
        m_metrics.ramUsagePercent = static_cast<float>(memStatus.dwMemoryLoad);

        if (!m_metrics.ramHistory.empty()) {
            m_metrics.ramHistory.erase(m_metrics.ramHistory.begin());
            m_metrics.ramHistory.push_back(m_metrics.ramUsagePercent);
        }
    }
}

void MetricsCollector::QueryNetwork() {
    PMIB_IF_TABLE2 pIfTable = nullptr;
    if (GetIfTable2(&pIfTable) == NO_ERROR && pIfTable != nullptr) {
        uint64_t totalIn = 0;
        uint64_t totalOut = 0;

        for (ULONG i = 0; i < pIfTable->NumEntries; ++i) {
            const auto& row = pIfTable->Table[i];
            if (!row.InterfaceAndOperStatusFlags.FilterInterface &&
                row.OperStatus == IfOperStatusUp &&
                (row.Type == IF_TYPE_ETHERNET_CSMACD || row.Type == IF_TYPE_IEEE80211)) {
                totalIn += row.InOctets;
                totalOut += row.OutOctets;
            }
        }
        FreeMibTable(pIfTable);

        uint64_t nowTick = GetTickCount64();
        if (m_prevNetworkSampleTick != 0 && nowTick > m_prevNetworkSampleTick) {
            double dt = static_cast<double>(nowTick - m_prevNetworkSampleTick) / 1000.0;
            if (dt > 0.0) {
                if (totalIn >= m_prevInOctets && totalOut >= m_prevOutOctets) {
                    m_metrics.netDownloadBytesPerSec = static_cast<uint64_t>((totalIn - m_prevInOctets) / dt);
                    m_metrics.netUploadBytesPerSec = static_cast<uint64_t>((totalOut - m_prevOutOctets) / dt);
                }
            }
        }

        m_prevInOctets = totalIn;
        m_prevOutOctets = totalOut;
        m_prevNetworkSampleTick = nowTick;

        if (!m_metrics.netDownHistory.empty()) {
            float downKb = static_cast<float>(m_metrics.netDownloadBytesPerSec) / 1024.0f;
            m_metrics.netDownHistory.erase(m_metrics.netDownHistory.begin());
            m_metrics.netDownHistory.push_back(downKb);
        }
    }
}

void MetricsCollector::QueryBattery() {
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power)) {
        m_metrics.hasBattery = !(power.BatteryFlag & 128) && (power.BatteryFlag != 255);
        m_metrics.isCharging = (power.ACLineStatus == 1);
        if (power.BatteryLifePercent != 255) {
            m_metrics.batteryPercent = static_cast<float>(power.BatteryLifePercent);
        }
    }
}

void MetricsCollector::QueryUptime() {
    m_metrics.uptimeSeconds = GetTickCount64() / 1000;
}

void MetricsCollector::QueryGpu() {
    Microsoft::WRL::ComPtr<IDXGIFactory4> dxgiFactory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)))) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(dxgiFactory->EnumAdapters1(0, &adapter))) {
            Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
            if (SUCCEEDED(adapter.As(&adapter3))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO memInfo{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo))) {
                    m_metrics.vramUsedBytes = memInfo.CurrentUsage;
                    m_metrics.vramTotalBytes = memInfo.Budget;
                    if (memInfo.Budget > 0) {
                        m_metrics.gpuUsagePercent = static_cast<float>(memInfo.CurrentUsage) / static_cast<float>(memInfo.Budget) * 100.0f;
                    }
                }
            }
        }
    }
}

void MetricsCollector::WorkerLoop() {
    while (m_running) {
        if (!m_paused) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                QueryCpu();
                QueryMemory();
                QueryNetwork();
                QueryBattery();
                QueryUptime();
                QueryGpu();
            }

            core::EventBus::Instance().Publish(MetricsUpdatedEvent{GetMetrics()});
        }

        // Sleep with interruptible wait
        for (uint32_t elapsed = 0; elapsed < m_intervalMs && m_running; elapsed += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace aether::system
