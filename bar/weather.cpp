#include "aether/bar/weather.hpp"
#include "aether/core/i18n.hpp"
#include <winhttp.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>

#pragma comment(lib, "winhttp.lib")

namespace aether::bar {

namespace {
std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
} // namespace

bool WeatherService::HttpGet(const wchar_t* host, const wchar_t* path, bool https, std::string& out) {
    out.clear();
    HINTERNET ses = WinHttpOpen(L"AetherBar/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 5000, 5000, 8000, 8000);
    bool ok = false;
    if (HINTERNET con = WinHttpConnect(ses, host, https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0)) {
        if (HINTERNET req = WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               https ? WINHTTP_FLAG_SECURE : 0)) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, sz = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                        std::string chunk(avail, '\0');
                        DWORD got = 0;
                        if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
                        out.append(chunk.data(), got);
                        if (out.size() > 256 * 1024) break;
                    }
                    ok = !out.empty();
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return ok;
}

bool WeatherService::Fetch() {
    std::string body;
    if (!m_haveLoc) {
        if (!HttpGet(L"ip-api.com", L"/json/?fields=status,lat,lon,city", false, body)) return false;
        auto j = nlohmann::json::parse(body, nullptr, false);
        if (j.is_discarded() || j.value("status", "") != "success") return false;
        m_lat = j.value("lat", 0.0);
        m_lon = j.value("lon", 0.0);
        m_city = Widen(j.value("city", std::string()));
        m_haveLoc = true;
    }
    char path[640];
    snprintf(path, sizeof(path),
        "/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,is_day,wind_speed_10m,wind_direction_10m,surface_pressure,precipitation"
        "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,uv_index_max&forecast_days=7&timezone=auto", m_lat, m_lon);
    if (!HttpGet(L"api.open-meteo.com", Widen(path).c_str(), true, body)) return false;
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.contains("current")) return false;
    const auto& c = j["current"];
    WeatherNow n;
    n.valid = true;
    n.tempC = static_cast<int>(std::lround(c.value("temperature_2m", 0.0)));
    n.code = c.value("weather_code", 0);
    n.day = c.value("is_day", 1) != 0;
    n.city = m_city;
    n.feelsC = static_cast<int>(std::lround(c.value("apparent_temperature", 0.0)));
    n.humidity = static_cast<int>(std::lround(c.value("relative_humidity_2m", 0.0)));
    n.windKmh = static_cast<int>(std::lround(c.value("wind_speed_10m", 0.0)));
    n.windDeg = static_cast<int>(std::lround(c.value("wind_direction_10m", 0.0)));
    n.pressure = static_cast<int>(std::lround(c.value("surface_pressure", 0.0)));
    n.precipMm = static_cast<float>(c.value("precipitation", 0.0));
    const std::string nowT = c.value("time", std::string());          // "2026-11-14T02:15"
    // hourly: start at the current hour
    if (j.contains("hourly")) {
        const auto& h = j["hourly"];
        if (h.contains("time") && h["time"].is_array()) {
            const std::string key = nowT.substr(0, 13);              // "2026-11-14T02"
            size_t start = 0;
            for (size_t i = 0; i < h["time"].size(); ++i) { const std::string t = h["time"][i].get<std::string>(); if (t.compare(0, 13, key) == 0) { start = i; break; } }
            for (size_t i = start; i < h["time"].size() && n.hours.size() < 12; ++i) {
                HourPoint p;
                const std::string t = h["time"][i].get<std::string>();
                p.hour = atoi(t.c_str() + 11);
                if (h.contains("temperature_2m") && h["temperature_2m"][i].is_number()) p.tempC = static_cast<int>(std::lround(h["temperature_2m"][i].get<double>()));
                if (h.contains("weather_code") && h["weather_code"][i].is_number()) p.code = h["weather_code"][i].get<int>();
                if (h.contains("is_day") && h["is_day"][i].is_number()) p.day = h["is_day"][i].get<int>() != 0;
                if (h.contains("precipitation_probability") && h["precipitation_probability"][i].is_number()) p.rainPct = h["precipitation_probability"][i].get<int>();
                n.hours.push_back(p);
            }
        }
    }
    if (j.contains("daily")) {
        const auto& d = j["daily"];
        if (d.contains("time") && d["time"].is_array()) {
            for (size_t i = 0; i < d["time"].size() && i < 7; ++i) {
                DayPoint p;
                const std::string t = d["time"][i].get<std::string>();   // "2026-11-14"
                int y = atoi(t.c_str()), m = atoi(t.c_str() + 5), dd = atoi(t.c_str() + 8);
                static const int tt[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
                int yy = m < 3 ? y - 1 : y;
                p.dow = (yy + yy / 4 - yy / 100 + yy / 400 + tt[m - 1] + dd) % 7;   // 0 = Sunday
                auto num = [&](const char* k, double def) { return d.contains(k) && d[k].size() > i && d[k][i].is_number() ? d[k][i].get<double>() : def; };
                p.hiC = static_cast<int>(std::lround(num("temperature_2m_max", 0)));
                p.loC = static_cast<int>(std::lround(num("temperature_2m_min", 0)));
                p.code = static_cast<int>(num("weather_code", 0));
                p.rainPct = static_cast<int>(num("precipitation_probability_max", 0));
                if (i == 0) {
                    n.hiC = p.hiC; n.loC = p.loC; n.uv = static_cast<int>(std::lround(num("uv_index_max", 0)));
                    if (d.contains("sunrise") && d["sunrise"].size() > 0 && d["sunrise"][0].is_string()) n.sunrise = Widen(d["sunrise"][0].get<std::string>().substr(11, 5));
                    if (d.contains("sunset") && d["sunset"].size() > 0 && d["sunset"][0].is_string()) n.sunset = Widen(d["sunset"][0].get<std::string>().substr(11, 5));
                }
                n.days.push_back(p);
            }
        }
    }
    std::lock_guard<std::mutex> l(m_mutex);
    m_now = n;
    return true;
}

void WeatherService::Loop() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    while (m_running) {
        if (clock::now() >= next || m_refreshNow.exchange(false)) {
            next = clock::now() + (Fetch() ? std::chrono::minutes(15) : std::chrono::minutes(2));
        }
        for (int i = 0; i < 10 && m_running && !m_refreshNow; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void WeatherService::Start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { Loop(); });
}

void WeatherService::Stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
}

WeatherService::Kind WeatherService::KindOf(int code, bool day) {
    if (code == 0) return day ? Kind::Clear : Kind::Night;
    if (code == 1) return day ? Kind::Clear : Kind::Night;
    if (code == 2) return day ? Kind::PartlyCloudy : Kind::NightCloudy;
    if (code == 3) return Kind::Cloudy;
    if (code == 45 || code == 48) return Kind::Fog;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return Kind::Rain;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return Kind::Snow;
    if (code >= 95) return Kind::Storm;
    return Kind::Cloudy;
}

std::wstring WeatherService::Describe(int code, bool day) {
    using i18n::Pick;
    switch (code) {
        case 0:  return day ? Pick(L"Clear", L"Açık") : Pick(L"Clear night", L"Açık gece");
        case 1:  return Pick(L"Mostly clear", L"Çoğunlukla açık");
        case 2:  return Pick(L"Partly cloudy", L"Parçalı Bulutlu");
        case 3:  return Pick(L"Overcast", L"Bulutlu");
        case 45: case 48: return Pick(L"Foggy", L"Sisli");
        case 51: case 53: case 55: return Pick(L"Drizzle", L"Çiseleme");
        case 56: case 57: case 66: case 67: return Pick(L"Freezing rain", L"Dondurucu yağmur");
        case 61: case 63: case 65: return Pick(L"Rain", L"Yağmurlu");
        case 71: case 73: case 75: case 77: return Pick(L"Snow", L"Karlı");
        case 80: case 81: case 82: return Pick(L"Showers", L"Sağanak");
        case 85: case 86: return Pick(L"Snow showers", L"Kar sağanağı");
        case 95: case 96: case 99: return Pick(L"Thunderstorm", L"Gök gürültülü fırtına");
    }
    return Pick(L"Cloudy", L"Bulutlu");
}

} // namespace aether::bar
