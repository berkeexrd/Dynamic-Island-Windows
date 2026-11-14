#pragma once

#include <windows.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <vector>
#include "aether/core/types.hpp"
#include "aether/animation/spring.hpp"
#include "aether/render/dcomp_layer.hpp"
#include "aether/render/backdrop.hpp"

namespace aether::bar {

using Microsoft::WRL::ComPtr;
class StartMenu;
class Flyout;

// Hides / restores the stock Windows taskbar. Always restorable, even after a crash (--restore).
//  hard (default): the stock bar window is hidden completely; Aether's own tray (TrayHost) replaces it.
//  soft (--keep-stock): the stock bar is only switched to auto-hide, so Win+B still reaches the real tray.
class ShellTaskbarGuard {
public:
    static ShellTaskbarGuard& Instance() { static ShellTaskbarGuard g; return g; }
    void Hide(bool hard);
    void Restore();
    void KeepHidden();
    [[nodiscard]] bool IsHidden() const { return m_hidden; }
private:
    UINT m_prevState{0};
    bool m_hidden{false};
    bool m_hard{false};
};

// One button in the dock = one application (its windows on this monitor are grouped).
struct Entry {
    std::wstring key;
    std::wstring exePath;
    std::wstring label;
    bool pinned{false};
    std::vector<HWND> windows;
    ComPtr<ID2D1Bitmap1> icon;
    ULONGLONG iconStamp{0};
    int cycle{0};
    animation::Spring<float> appear;
    animation::Spring<float> hover;
    animation::Spring<float> active;
    Entry();
    [[nodiscard]] bool Running() const { return !windows.empty(); }
};

// One bar per monitor. Only the primary bar carries the tray / quick-settings controls.
class AetherBar {
public:
    AetherBar(HMONITOR monitor, bool primary, StartMenu* start);
    ~AetherBar();

    bool Create();
    bool Frame(float dt);          // update + render, returns true while animating
    [[nodiscard]] HMONITOR Monitor() const { return m_mon; }
    [[nodiscard]] int ReservedPx() const { return m_reservedPx; }
    [[nodiscard]] HWND Hwnd() const { return m_hwnd; }
    // Panels opened by the chevron (tray), the volume/battery area (quick settings) and the clock (calendar).
    void SetPanels(Flyout* quick, Flyout* calendar, Flyout* tray) { m_quick = quick; m_calendar = calendar; m_tray = tray; }
    void SetExtraPanels(Flyout* notify, Flyout* weather, Flyout* net) { m_notify = notify; m_weather = weather; m_net = net; }
    void OpenTray() { ToggleFlyout(m_tray, m_layout.chevron); }

private:
    static constexpr UINT WM_APPBAR = WM_USER + 1;
    enum class Hit { None, Start, Search, Item, Chevron, Quick, Network, Clock, Bell, Weather, Desktop, Right, Dock };

    struct LayoutInfo {
        core::Rect dock, right, start, search, chevron, quick, network, clock, bell, weather, desktop;
        std::vector<core::Rect> items;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);

    void RegisterAppBar();
    void UnregisterAppBar();
    void SetAppBarPos();
    void PositionWindow();
    void RefreshBackdrop(bool hideWindowFirst);

    void RefreshWindows();
    ComPtr<ID2D1Bitmap1> LoadIcon(const Entry& e);
    Entry* FindEntry(const std::wstring& key);
    void Tick(float dt);
    bool IsAnimating() const;

    Hit HitTest(const core::Vec2& pt, int& itemIndex) const;
    void OnItemClick(Entry& e);
    void ShowItemMenu(int index);
    void ShowBarMenu();
    void ActivateWindow(HWND hwnd);
    void AdjustVolume(float delta);
    void ToggleFlyout(Flyout* panel, const core::Rect& anchor);

    void ComputeLayout();
    void ComputeTooltip();
    void UpdateRegion();
    void Render();
    void DrawPill(ID2D1DeviceContext* ctx, const core::Rect& r, float radius);
    void DrawStartLogo(ID2D1DeviceContext* ctx, const core::Vec2& c, float size, float alpha);
    void DrawWeatherIcon(ID2D1DeviceContext* ctx, const core::Vec2& c, float size, int kind);
    void DrawBattery(ID2D1DeviceContext* ctx, float x, float cy, int percent, bool charging);
    void DrawText(ID2D1DeviceContext* ctx, const std::wstring& t, IDWriteTextFormat* f,
                  const D2D1_RECT_F& r, const core::Color& c);
    void DrawGlyph(ID2D1DeviceContext* ctx, wchar_t g, const D2D1_RECT_F& r, const core::Color& c);
    void CreateFonts();

    HWND m_hwnd{nullptr};
    HMONITOR m_mon{nullptr};
    bool m_primary{false};
    StartMenu* m_start{nullptr};
    Flyout *m_quick{nullptr}, *m_calendar{nullptr}, *m_tray{nullptr}, *m_notify{nullptr}, *m_weather{nullptr}, *m_net{nullptr};
    std::unique_ptr<render::DCompLayer> m_dcomp;
    ComPtr<ID2D1SolidColorBrush> m_brush;

    RECT m_monRect{};
    float m_dpi{1.0f};
    int m_windowW{0}, m_windowH{0}, m_reservedPx{0};
    bool m_visible{true};
    render::Backdrop m_backdrop;               // blurred wallpaper strip behind the pills
    float m_hoverAmt[16]{};                    // eased hover amount per Hit kind (system pill highlights)

    std::vector<std::unique_ptr<Entry>> m_entries;
    unsigned m_pinVersion{0};
    LayoutInfo m_layout;
    animation::Spring<float> m_startHover;
    animation::Spring<float> m_dockAppear;

    core::Vec2 m_mouse{-1.0f, -1.0f};
    int m_hoverItem{-1};
    Hit m_hoverKind{Hit::None};
    bool m_trackingMouse{false};
    float m_refreshTimer{0.0f};
    float m_keepHiddenTimer{0.0f};
    float m_volTimer{0.0f};
    float m_idleRender{0.0f};

    // tooltip
    std::wstring m_tipText, m_tipOverride;
    float m_tipAnchor{0.0f};
    float m_tipOverrideTimer{0.0f};
    float m_tipAlpha{0.0f};
    bool m_tipWanted{false};
    core::Rect m_tipRect;

    // quick-status
    float m_volume{0.5f};
    bool m_muted{false};

    std::vector<int> m_regionKey;

    ComPtr<IDWriteTextFormat> m_fontTip, m_fontTime, m_fontDate, m_fontIcon;
    bool m_hasIconFont{false};
    int m_netKind{0};            // 0 = offline, 1 = Wi-Fi, 2 = Ethernet
    float m_netTimer{10.0f};
    float m_weatherTimer{10.0f};
    int m_weatherKind{0};
    std::wstring m_weatherText;
    int m_weatherTemp{0};
    bool m_weatherValid{false};
    UINT m_taskbarCreatedMsg{0};
};

} // namespace aether::bar
