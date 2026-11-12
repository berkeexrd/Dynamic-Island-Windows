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
    NetworkMeter
};

class DesktopWidgetWindow {
public:
    explicit DesktopWidgetWindow(WidgetType type);
    ~DesktopWidgetWindow();

    bool Create();
    void Show(bool show = true);
    void Update(float dt);
    void Render();

    [[nodiscard]] HWND GetHwnd() const { return m_hwnd; }
    [[nodiscard]] WidgetType GetType() const { return m_type; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void RenderSystemMonitor(ID2D1DeviceContext* ctx);
    void RenderClockCalendar(ID2D1DeviceContext* ctx);
    void RenderNetworkMeter(ID2D1DeviceContext* ctx);

    WidgetType m_type;
    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dcomp;

    bool m_isDragging{false};
    POINT m_dragStartCursor{};
    POINT m_dragStartWindow{};

    ComPtr<IDWriteTextFormat> m_fontLarge;
    ComPtr<IDWriteTextFormat> m_fontMedium;
    ComPtr<IDWriteTextFormat> m_fontSmall;

    uint32_t m_width{260};
    uint32_t m_height{130};
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

private:
    WidgetManager() = default;

    std::vector<std::unique_ptr<DesktopWidgetWindow>> m_widgets;
    bool m_visible{true};
};

} // namespace aether::widgets
