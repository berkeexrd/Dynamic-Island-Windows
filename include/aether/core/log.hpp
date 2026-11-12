#pragma once

#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <windows.h>

namespace aether::core {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

class Logger {
public:
    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    void SetMinLevel(LogLevel level) {
        m_minLevel = level;
    }

    void Log(LogLevel level, const std::string& message) {
        if (level < m_minLevel) return;

        std::lock_guard<std::mutex> lock(m_mutex);
        auto now = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tmNow{};
        localtime_s(&tmNow, &timeT);

        std::ostringstream ss;
        ss << "[" << std::put_time(&tmNow, "%H:%M:%S") << "."
           << std::setfill('0') << std::setw(3) << ms.count() << "] ["
           << LevelToString(level) << "] " << message << "\n";

        std::string formatted = ss.str();
        std::cout << formatted;
        OutputDebugStringA(formatted.c_str());

        std::ofstream logFile("aether.log", std::ios::app);
        if (logFile.is_open()) {
            logFile << formatted;
        }
    }

private:
    Logger() = default;
    static const char* LevelToString(LogLevel level) {
        switch (level) {
            case LogLevel::Debug:   return "DEBUG";
            case LogLevel::Info:    return "INFO ";
            case LogLevel::Warning: return "WARN ";
            case LogLevel::Error:   return "ERROR";
            default:                return "UNKN ";
        }
    }

    LogLevel m_minLevel{LogLevel::Debug};
    std::mutex m_mutex;
};

#define AETHER_LOG_DEBUG(msg) do { std::ostringstream _ss; _ss << msg; ::aether::core::Logger::Instance().Log(::aether::core::LogLevel::Debug, _ss.str()); } while(0)
#define AETHER_LOG_INFO(msg)  do { std::ostringstream _ss; _ss << msg; ::aether::core::Logger::Instance().Log(::aether::core::LogLevel::Info, _ss.str()); } while(0)
#define AETHER_LOG_WARN(msg)  do { std::ostringstream _ss; _ss << msg; ::aether::core::Logger::Instance().Log(::aether::core::LogLevel::Warning, _ss.str()); } while(0)
#define AETHER_LOG_ERROR(msg) do { std::ostringstream _ss; _ss << msg; ::aether::core::Logger::Instance().Log(::aether::core::LogLevel::Error, _ss.str()); } while(0)

} // namespace aether::core
