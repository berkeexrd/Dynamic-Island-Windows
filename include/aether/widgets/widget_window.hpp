#pragma once

#include <windows.h>
#include <wrl/client.h>
#include <memory>
#include <vector>
#include "aether/core/types.hpp"
#include "aether/config/config_manager.hpp"
#include "aether/render/dcomp_layer.hpp"
#include "aether/system/metrics_collector.hpp"

namespace aether::widgets {

using Microsoft::WRL::ComPtr;

enum class WidgetType {
    SystemMonitor,
    ClockCalendar,
    NetworkMeter,
    Storage,
    Weather,
    NowPlaying
};

class DesktopWidgetWindow {
public:
    explicit DesktopWidgetWindow(WidgetType type);
    ~DesktopWidgetWindow();

    bool Create();
    void Show(bool show = true);
    void Update(float dt);
    void Render();
    [[nodiscard]] bool IsAnimating() const { return m_shownNow && m_appear < 1.0f; }   // entrance animation running

    [[nodiscard]] HWND GetHwnd() const { return m_hwnd; }
    [[nodiscard]] WidgetType GetType() const { return m_type; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void RenderSystemMonitor(ID2D1DeviceContext* ctx);
    void RenderClockCalendar(ID2D1DeviceContext* ctx);
    void RenderNetworkMeter(ID2D1DeviceContext* ctx);
    void RenderStorage(ID2D1DeviceContext* ctx);
    void RenderWeather(ID2D1DeviceContext* ctx);
    void RenderNowPlaying(ID2D1DeviceContext* ctx);
    void UpdateArt();
    bool m_shownNow{true};
    float m_appear{1.0f}, m_delay{0.0f};   // entrance animation (0..1) and its stagger delay in seconds
    float m_time{0};
    ComPtr<ID2D1Bitmap1> m_art; uint64_t m_artVersion{~0ull};
    struct Drive { wchar_t letter; std::wstring label; double usedGB, totalGB; };
    std::vector<Drive> m_drives; ULONGLONG m_drivesAt{0};
    core::Rect m_btnPrev, m_btnPlay, m_btnNext;

    WidgetType m_type;
    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dcomp;

    bool m_isDragging{false};
    POINT m_dragStartCursor{};
    POINT m_dragStartWindow{};

    // Drawing helpers (all coordinates are logical px, the context carries the DPI scale)
    void DrawText(ID2D1DeviceContext* ctx, const std::wstring& t, IDWriteTextFormat* f, const D2D1_RECT_F& r, const core::Color& c);
    void DrawRing(ID2D1DeviceContext* ctx, float cx, float cy, float radius, float thickness, float frac,
                  const std::wstring& value, const std::wstring& label);
    void DrawSpark(ID2D1DeviceContext* ctx, const std::vector<float>& v, float peak, const core::Rect& r, const core::Color& c, bool fill);

    ComPtr<IDWriteTextFormat> m_fontLarge;     // clock digits
    ComPtr<IDWriteTextFormat> m_fontMedium;    // values
    ComPtr<IDWriteTextFormat> m_fontSmall;     // captions
    ComPtr<IDWriteTextFormat> m_fontTitle;     // card title
    ComPtr<IDWriteTextFormat> m_fontCenter;    // centred text inside rings
    ComPtr<IDWriteTextFormat> m_fontTiny;      // ring labels
    ComPtr<IDWriteTextFormat> m_fontIcon;      // Segoe Fluent Icons
    ComPtr<IDWriteTextFormat> m_fontClock;     // big, thin clock digits (card-less clock)
    ComPtr<IDWriteTextFormat> m_fontDate;      // date line above the clock (centred)
    ComPtr<IDWriteTextFormat> m_fontInfo;      // weather line below the clock (left aligned, measured)
    ComPtr<IDWriteTextFormat> m_fontTinyL;     // left aligned small text (weather)
    ComPtr<IDWriteTextFormat> m_fontTinyR;     // right aligned small text (storage usage)
    ComPtr<ID2D1SolidColorBrush> m_brush;
    bool m_hasIconFont{false};

    uint32_t m_width{300};     // logical px
    uint32_t m_height{172};
    float m_dpiScale{1.0f};
};

class WidgetManager {
public:
    static WidgetManager& Instance() {
        static WidgetManager instance;
        return instance;
    }

    void Initialize();
    void Shutdown();
    void Update(float dt);
    void Render();
    void SetVisible(bool visible);
    void SetFrameByFrame(bool on) { m_frameByFrame = on; }   // recording: render on every call
    [[nodiscard]] bool IsAnimating() const { for (const auto& w : m_widgets) if (w->IsAnimating()) return true; return false; }

private:
    WidgetManager() = default;

    std::vector<std::unique_ptr<DesktopWidgetWindow>> m_widgets;
    bool m_visible{true};
    bool m_frameByFrame{false};
};

} // namespace aether::widgets
