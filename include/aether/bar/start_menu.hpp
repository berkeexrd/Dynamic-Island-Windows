#pragma once

#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "aether/core/types.hpp"
#include "aether/animation/spring.hpp"
#include "aether/render/dcomp_layer.hpp"

namespace aether::bar {

using Microsoft::WRL::ComPtr;

// Replacement for the Windows Start menu: pinned apps, searchable "All apps" list
// (desktop + Store apps straight from the shell's AppsFolder) and power options.
class StartMenu {
public:
    StartMenu();
    ~StartMenu();

    bool Create();
    void PreloadAsync();                        // enumerate installed apps on a worker thread
    void Toggle(HMONITOR monitor, int reservedPx);
    void Show(HMONITOR monitor, int reservedPx);
    void Hide();
    [[nodiscard]] bool IsOpen() const { return m_wantOpen; }
    bool Frame(float dt);                       // returns true while it needs frames

private:
    struct App {
        std::wstring name, lower, parse;
        ComPtr<ID2D1Bitmap1> icon;
        bool iconTried{false};
    };
    struct Row {
        bool header{false};
        std::wstring label;
        int first{0}, count{0};
        float y{0}, h{0};                       // logical px inside the scroll content
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);

    void AdoptLoadedApps();
    void LoadPins();
    void SavePins();
    bool IsPinned(const std::wstring& parse) const;
    void TogglePin(const std::wstring& parse);
    void BuildRows();
    void CreateFonts();
    void EnsureIcon(App& app);

    void Render();
    void Launch(int appIndex);
    void ShowPowerMenu();
    void ShowTileMenu(int appIndex);
    void SetQuery(const std::wstring& q);
    void EnsureVisible(int flatIndex);
    void MoveSelection(int dRow, int dCol);

    // geometry helpers (window pixels)
    [[nodiscard]] core::Rect TileRect(const Row& r, int k) const;
    [[nodiscard]] int HitTile(const core::Vec2& p) const;
    [[nodiscard]] float ViewportTop() const;
    [[nodiscard]] float ViewportBottom() const;
    [[nodiscard]] float MaxScroll() const;
    void DrawText(ID2D1DeviceContext* ctx, const std::wstring& t, IDWriteTextFormat* f,
                  const D2D1_RECT_F& r, const core::Color& c);
    void DrawGlyph(ID2D1DeviceContext* ctx, wchar_t g, const D2D1_RECT_F& r, const core::Color& c);

    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dcomp;
    ComPtr<ID2D1SolidColorBrush> m_brush;

    // data
    std::vector<App> m_apps;
    std::vector<std::wstring> m_pins;
    std::vector<Row> m_rows;
    std::vector<int> m_flat;                    // flat tile order -> index into m_apps
    std::wstring m_query;
    std::wstring m_userName;
    std::atomic<int> m_loadState{0};            // 0 idle, 1 loading, 2 loaded (not yet adopted)
    std::mutex m_loadMutex;
    std::vector<std::pair<std::wstring, std::wstring>> m_loadedRaw;  // name, parsing name
    std::thread m_loader;
    bool m_adopted{false};

    // state
    int m_sel{-1};
    int m_hover{-1};
    int m_hoverBtn{0};                          // 1 = settings, 2 = power
    float m_contentH{0.0f};
    float m_time{0.0f};
    bool m_wantOpen{false};
    bool m_menuOpen{false};
    animation::Spring<float> m_open;
    animation::Spring<float> m_scroll;

    // layout
    float m_scale{1.0f};
    int m_winW{0}, m_winH{0};
    float m_panelW{0}, m_panelH{0}, m_pad{0};
    core::Rect m_btnSettings, m_btnPower;

    ComPtr<IDWriteTextFormat> m_fontSearch, m_fontHeader, m_fontTile, m_fontUser, m_fontIcon, m_fontLetter;
    bool m_hasIconFont{false};
};

} // namespace aether::bar
