#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "aether/core/types.hpp"
#include "aether/animation/spring.hpp"
#include "aether/render/dcomp_layer.hpp"
#include "aether/render/backdrop.hpp"

namespace aether::bar {

using Microsoft::WRL::ComPtr;

// Base class of every pop-up panel that opens above the bar (quick settings, calendar, tray).
class Flyout {
public:
    virtual ~Flyout();
    bool Create(const wchar_t* cls, float w, float h);
    bool Create(const wchar_t* cls) { return Create(cls, m_baseW, m_baseH); }   // uses the panel's own size
    void Show(HMONITOR mon, int reservedPx, float anchorCenterX);
    void Hide();
    bool Frame(float dt);                       // true while visible / animating
    [[nodiscard]] bool IsOpen() const { return m_wantOpen; }
    [[nodiscard]] bool JustDismissed() const { return GetTickCount64() - m_dismissTick < 600; }   // closed by an outside click a moment ago
    [[nodiscard]] HWND Hwnd() const { return m_hwnd; }

protected:
    virtual void OnShow() {}
    virtual void OnTick(float) {}
    virtual void OnDraw(ID2D1DeviceContext* ctx, const core::Rect& panel) = 0;
    virtual void OnClick(int /*hitId*/, const core::Vec2& /*p*/) {}
    virtual void OnDrag(int /*hitId*/, const core::Vec2& /*p*/, bool /*begin*/) {}
    virtual void OnWheel(float /*notches*/) {}
    virtual void OnChar(wchar_t /*ch*/) {}
    virtual void OnKey(WPARAM /*vk*/) {}
    virtual float PanelHeight() { return m_baseH; }

    void Text(ID2D1DeviceContext*, const std::wstring&, IDWriteTextFormat*, const core::Rect&, const core::Color&);
    void Glyph(ID2D1DeviceContext*, wchar_t, const core::Rect&, const core::Color&, bool big = false);
    void Fill(ID2D1DeviceContext*, const core::Rect&, float radius, const core::Color&);
    void Stroke(ID2D1DeviceContext*, const core::Rect&, float radius, const core::Color&, float w = 1.0f);
    void AddHit(const core::Rect& r, int id) { m_hits.push_back({r, id}); }
    [[nodiscard]] int HitAt(const core::Vec2& p) const;
    [[nodiscard]] float S() const { return m_scale; }

    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dcomp;
    ComPtr<ID2D1SolidColorBrush> m_brush;
    unsigned long long m_lastDrawMs{0};
    float m_skipDt{0};
    ComPtr<IDWriteTextFormat> m_fTitle, m_fBody, m_fSmall, m_fBig, m_fIcon, m_fIconBig, m_fCenter, m_fCTitle, m_fCBody, m_fCSmall;
    bool m_hasIcons{false};
    render::Backdrop m_backdrop;                // blurred snapshot behind the panel (taken when it opens)
    float m_scale{1.0f}, m_baseW{360}, m_baseH{400}, m_pad{24};
    core::Vec2 m_mouse{-1, -1};
    bool m_down{false};
    int m_dragId{0};
    bool m_rightButton{false};
    float m_time{0};

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void MakeFonts();
    struct HitRect { core::Rect r; int id; };
    std::vector<HitRect> m_hits;
    bool m_wantOpen{false};
    bool m_prevDown{false};
    ULONGLONG m_dismissTick{0};
    animation::Spring<float> m_open{0.0f, animation::SpringConfig{520.0f, 36.0f, 1.0f, 0.002f}};
};

// Quick settings: volume, network, battery, shortcuts
class QuickPanel : public Flyout {
public:
    QuickPanel() { m_baseW = 392; m_baseH = 560; }
protected:
    void OnShow() override;
    void OnTick(float dt) override;
    void OnDraw(ID2D1DeviceContext*, const core::Rect&) override;
    void OnClick(int id, const core::Vec2&) override;
    void OnDrag(int id, const core::Vec2& p, bool begin) override;
    void OnWheel(float n) override;
private:
    struct Dev { std::wstring id, name; bool input{false}, isDefault{false}; };
    void Refresh();
    void RefreshDevices();
    int m_view{0};                       // 0 = main, 1 = sound devices
    std::vector<Dev> m_devices;
    std::wstring m_micName; bool m_hasMic{false}, m_micMuted{false}; float m_micVolume{0.5f}; core::Rect m_micSlider;
    float m_volume{0.5f}; bool m_muted{false};
    std::wstring m_device, m_netName, m_netDetail; bool m_netUp{false}, m_wifi{false}; int m_signal{0};
    int m_battery{-1}; bool m_charging{false};
    float m_refreshTimer{0}; core::Rect m_slider;
};

// Clock + calendar
class CalendarPanel : public Flyout {
public:
    CalendarPanel() { m_baseW = 372; m_baseH = 500; }
protected:
    void OnShow() override;
    void OnDraw(ID2D1DeviceContext*, const core::Rect&) override;
    void OnClick(int id, const core::Vec2&) override;
    void OnWheel(float n) override;
private:
    int m_year{2026}, m_month{1}, m_selDay{0}, m_selMonth{0}, m_selYear{0};
};

// System tray (icons of every app, served by TrayHost)
class TrayPanel : public Flyout {
public:
    TrayPanel() { m_baseW = 344; m_baseH = 220; }
protected:
    void OnShow() override;
    void OnDraw(ID2D1DeviceContext*, const core::Rect&) override;
    void OnClick(int id, const core::Vec2&) override;
    float PanelHeight() override;
private:
    struct Cached { ComPtr<ID2D1Bitmap1> bmp; unsigned serial{0}; };
    std::map<std::pair<HWND, UINT>, Cached> m_cache;
    ULONGLONG m_lastClick{0};
    int m_lastId{-1};
};

} // namespace aether::bar
