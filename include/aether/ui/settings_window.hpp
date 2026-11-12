#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "aether/ui/settings_pages.hpp"

namespace aether::ui {

using Microsoft::WRL::ComPtr;

class SettingsWindow {
public:
    static SettingsWindow& Instance() {
        static SettingsWindow instance;
        return instance;
    }

    bool Create();
    void Show(bool show = true);
    [[nodiscard]] bool IsVisible() const { return m_visible; }
    void Render();
    void Destroy();

    [[nodiscard]] HWND GetHwnd() const { return m_hwnd; }

private:
    SettingsWindow() = default;
    ~SettingsWindow() { Destroy(); }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool CreateDeviceD3D();
    void CleanupDeviceD3D();
    void CreateRenderTarget();
    void CleanupRenderTarget();

    HWND m_hwnd{nullptr};
    bool m_visible{false};
    uint32_t m_width{960};
    uint32_t m_height{640};

    ComPtr<ID3D11Device> m_pd3dDevice;
    ComPtr<ID3D11DeviceContext> m_pd3dDeviceContext;
    ComPtr<IDXGISwapChain> m_pSwapChain;
    ComPtr<ID3D11RenderTargetView> m_mainRenderTargetView;

    SettingsTab m_activeTab{SettingsTab::DynamicIsland};
};

} // namespace aether::ui
