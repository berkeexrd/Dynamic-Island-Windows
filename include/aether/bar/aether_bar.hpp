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

namespace aether::bar {

using Microsoft::WRL::ComPtr;

// Hides / restores the stock Windows taskbar. Always restorable, even after a crash (--restore).
class ShellTaskbarGuard {
public:
    static ShellTaskbarGuard& Instance() { static ShellTaskbarGuard g; return g; }
    void Hide();
    void Restore();
    void KeepHidden();   // call periodically: Explorer re-shows the bar on auto-hide hover etc.
    [[nodiscard]] bool IsHidden() const { return m_hidden; }
private:
    UINT m_prevState{0};
    bool m_hidden{false};
};

// One button in the dock = one application (all its windows are grouped), pinned or running.
struct Entry {
    std::wstring key;        // exe path (or a unique key for UWP frame windows)
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

class AetherBar {
public:
    AetherBar();
    ~AetherBar();

    bool Create();
    int  Run();   // message / render loop, returns exit code

private:
    static constexpr UINT WM_APPBAR = WM_USER + 1;
    enum class Hit { None, Start, Item, Right };

    struct LayoutInfo {
        core::Rect dock, right, start;
        std::vector<core::Rect> items;   // parallel to m_entries
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);

    // shell integration
    void RegisterAppBar();
    void UnregisterAppBar();
    void SetAppBarPos();
    void PositionWindow();

    // data
    void RefreshWindows();
    void LoadPins();
    void SavePins();
    Entry* FindEntry(const std::wstring& key);
    ComPtr<ID2D1Bitmap1> LoadIcon(const Entry& e);
    void Tick(float dt);
    bool IsAnimating() const;

    // interaction
    Hit HitTest(const core::Vec2& pt, int& itemIndex) const;
    void OnItemClick(Entry& e);
    void ShowItemMenu(int index);
    void ShowBarMenu();
    void ActivateWindow(HWND hwnd);

    // rendering
    void ComputeLayout();
    void Render();
    void DrawPill(ID2D1DeviceContext* ctx, const core::Rect& r, float radius);
    void DrawStartLogo(ID2D1DeviceContext* ctx, const core::Vec2& c, float size, float alpha);
    void DrawBattery(ID2D1DeviceContext* ctx, float x, float cy, int percent, bool charging);
    void DrawText(ID2D1DeviceContext* ctx, const std::wstring& t, IDWriteTextFormat* f,
                  const D2D1_RECT_F& r, const core::Color& c);
    void CreateFonts();

    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dcomp;
    ComPtr<ID2D1SolidColorBrush> m_brush;

    RECT m_monitor{};
    float m_dpi{1.0f};
    int m_windowW{0}, m_windowH{0}, m_reservedPx{0};
    bool m_visible{true};

    std::vector<std::unique_ptr<Entry>> m_entries;
    LayoutInfo m_layout;
    animation::Spring<float> m_startHover;
    animation::Spring<float> m_dockAppear;

    core::Vec2 m_mouse{-1.0f, -1.0f};
    int m_hoverItem{-1};
    Hit m_hoverKind{Hit::None};
    bool m_trackingMouse{false};
    float m_refreshTimer{0.0f};
    float m_keepHiddenTimer{0.0f};
    float m_tooltipAlpha{0.0f};

    ComPtr<IDWriteTextFormat> m_fontTip, m_fontTime, m_fontDate;
    UINT m_taskbarCreatedMsg{0};
};

} // namespace aether::bar
