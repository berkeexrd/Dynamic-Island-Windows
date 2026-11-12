#pragma once

#include <windows.h>
#include <cstdint>

namespace aether::core {

class Timer {
public:
    Timer() {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        m_frequency = freq.QuadPart;
        Reset();
    }

    void Reset() {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        m_startTime = counter.QuadPart;
        m_lastTime = m_startTime;
    }

    /// Returns time elapsed since last Tick() call in seconds.
    double Tick() {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        int64_t current = counter.QuadPart;
        double dt = static_cast<double>(current - m_lastTime) / static_cast<double>(m_frequency);
        m_lastTime = current;
        return dt;
    }

    /// Returns total elapsed time since construction or Reset() in seconds.
    [[nodiscard]] double TotalTime() const {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<double>(counter.QuadPart - m_startTime) / static_cast<double>(m_frequency);
    }

    /// Returns current timestamp in seconds.
    [[nodiscard]] double Now() const {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<double>(counter.QuadPart) / static_cast<double>(m_frequency);
    }

private:
    int64_t m_frequency{0};
    int64_t m_startTime{0};
    int64_t m_lastTime{0};
};

} // namespace aether::core
