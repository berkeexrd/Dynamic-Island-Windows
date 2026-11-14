#pragma once

#include <filesystem>
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

    // Release builds only record warnings and errors (nothing is formatted or written for the rest), so a normal user's PC
    // does no logging work at all. Set the environment variable AETHER_LOG=1 to get the full log when chasing a problem.
    bool Enabled(LogLevel level) const { return level >= m_minLevel; }

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
#ifndef NDEBUG
        std::cout << formatted;
        OutputDebugStringA(formatted.c_str());
#endif
        if (m_path.empty()) m_path = LogPath();
        // keep the file small: start over once it grows past 512 KB
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        const bool big = GetFileAttributesExW(m_path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeHigh == 0 && fa.nFileSizeLow > 512u * 1024u;
        std::ofstream logFile(std::filesystem::path(m_path), big ? std::ios::trunc : std::ios::app);
        if (logFile.is_open()) {
            logFile << formatted;
        }
    }

private:
    Logger() {
#ifdef NDEBUG
        wchar_t v[8] = {};
        m_minLevel = GetEnvironmentVariableW(L"AETHER_LOG", v, 8) > 0 && v[0] != L'0' ? LogLevel::Debug : LogLevel::Warning;
#endif
    }
    static std::wstring LogPath() {
        wchar_t b[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", b, MAX_PATH) == 0) return L"aether.log";
        std::wstring d = std::wstring(b) + L"\\Aether";
        CreateDirectoryW(d.c_str(), nullptr);
        return d + L"\\aether.log";
    }

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
    std::wstring m_path;
};

#define AETHER_LOG_DEBUG(msg) do { if (::aether::core::Logger::Instance().Enabled(::aether::core::LogLevel::Debug)) { std::ostringstream _ss; _ss << msg; ::aether::core::Logger::Instance().Log(::aether::core::LogLevel::Debug, _ss.str()); } } while(0)
#define AETHER_LOG_INFO(msg) do { if (::aether::core::Logger::Instance().Enabled(::aether::core::LogLevel::Info)) { std::ostringstream _ss; _ss << msg; ::aether::core::Logger::Instance().Log(::aether::core::LogLevel::Info, _ss.str()); } } while(0)
#define AETHER_LOG_WARN(msg) do { if (::aether::core::Logger::Instance().Enabled(::aether::core::LogLevel::Warning)) { std::ostringstream _ss; _ss << msg; ::aether::core::Logger::Instance().Log(::aether::core::LogLevel::Warning, _ss.str()); } } while(0)
#define AETHER_LOG_ERROR(msg) do { if (::aether::core::Logger::Instance().Enabled(::aether::core::LogLevel::Error)) { std::ostringstream _ss; _ss << msg; ::aether::core::Logger::Instance().Log(::aether::core::LogLevel::Error, _ss.str()); } } while(0)

} // namespace aether::core
