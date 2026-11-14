#pragma once
#include "aether/bar/flyouts.hpp"
#include "aether/bar/weather.hpp"
#include <map>
#include <string>
#include <vector>

namespace aether::bar {

// Animated weather: scene for the current conditions, details, hourly and 7-day forecast.
class WeatherPanel : public Flyout {
public:
    WeatherPanel() { m_baseW = 420; m_baseH = 690; }
protected:
    void OnShow() override;
    void OnDraw(ID2D1DeviceContext*, const core::Rect&) override;
    void OnClick(int id, const core::Vec2&) override;
private:
    void Scene(ID2D1DeviceContext*, const core::Rect&, const WeatherNow&);
    void Icon(ID2D1DeviceContext*, float cx, float cy, float size, int kind);
    void Moon(ID2D1DeviceContext*, float cx, float cy, float r, const core::Color& c);
    void EnsureFonts();
    ComPtr<IDWriteTextFormat> m_fHuge;
    float m_fontScale{0};
};

// Own notification centre: the toasts Windows has collected, with dismiss / clear all.
class NotifyPanel : public Flyout {
public:
    NotifyPanel() { m_baseW = 392; m_baseH = 580; }
protected:
    void OnShow() override;
    void OnTick(float dt) override;
    void OnDraw(ID2D1DeviceContext*, const core::Rect&) override;
    void OnClick(int id, const core::Vec2&) override;
    void OnWheel(float n) override;
private:
    float m_scroll{0}, m_scrollTarget{0}, m_refresh{0}, m_shown{0}, m_contentH{0};
    std::vector<long long> m_ids;
    std::map<std::wstring, ComPtr<ID2D1Bitmap1>> m_icons;
};

// Network centre: Wi-Fi switch, current connection, scanned networks, connect with password.
class NetPanel : public Flyout {
public:
    NetPanel() { m_baseW = 392; m_baseH = 620; }
protected:
    void OnShow() override;
    void OnTick(float dt) override;
    void OnDraw(ID2D1DeviceContext*, const core::Rect&) override;
    void OnClick(int id, const core::Vec2&) override;
    void OnWheel(float n) override;
    void OnChar(wchar_t ch) override;
    void OnKey(WPARAM vk) override;
private:
    void Bars(ID2D1DeviceContext*, float x, float y, float h, int signal, const core::Color& c);
    float m_scroll{0}, m_scrollTarget{0}, m_poll{0}, m_shown{0}, m_switch{1.0f}, m_expand{0};
    std::wstring m_sel, m_pass;
    bool m_selSecure{false};
    unsigned m_selAuth{0};
    bool m_showPass{false};
    std::vector<std::wstring> m_ssids;
};

} // namespace aether::bar
