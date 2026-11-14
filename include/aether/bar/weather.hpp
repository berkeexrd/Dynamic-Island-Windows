#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aether::bar {

struct HourPoint { int hour{0}; int tempC{0}; int code{0}; bool day{true}; int rainPct{0}; };
struct DayPoint  { int dow{0}; int hiC{0}; int loC{0}; int code{0}; int rainPct{0}; };   // dow: 0 = Sunday

struct WeatherNow {
    bool valid{false};
    int tempC{0};
    int code{0};            // WMO weather code (Open-Meteo)
    bool day{true};
    std::wstring city;
    // details for the weather panel
    int feelsC{0}, humidity{0}, windKmh{0}, windDeg{0}, pressure{0}, uv{0};
    float precipMm{0.0f};
    int hiC{0}, loC{0};
    std::wstring sunrise, sunset;          // "06:52"
    std::vector<HourPoint> hours;          // the next 12 hours
    std::vector<DayPoint> days;            // today + 6
};

// Current weather for the bottom-left pill. Location comes from the IP address (no permission prompt),
// the data from Open-Meteo (free, no key). Everything runs on a worker thread; failures are retried.
class WeatherService {
public:
    static WeatherService& Instance() { static WeatherService w; return w; }
    void Start();
    void Stop();
    void RefreshNow() { m_refreshNow = true; }
    [[nodiscard]] WeatherNow Get() const { std::lock_guard<std::mutex> l(m_mutex); return m_now; }

    // Text for a WMO code in the UI language, e.g. "Parçalı Bulutlu"
    static std::wstring Describe(int code, bool day);
    enum class Kind { Clear, PartlyCloudy, Cloudy, Fog, Rain, Snow, Storm, Night, NightCloudy };
    static Kind KindOf(int code, bool day);

private:
    WeatherService() = default;
    ~WeatherService() { Stop(); }
    void Loop();
    bool Fetch();
    static bool HttpGet(const wchar_t* host, const wchar_t* path, bool https, std::string& out);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_refreshNow{false};
    mutable std::mutex m_mutex;
    WeatherNow m_now;
    double m_lat{0}, m_lon{0};
    bool m_haveLoc{false};
    std::wstring m_city;
};

} // namespace aether::bar
