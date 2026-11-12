#include "aether/ui/settings_window.hpp"
#include "aether/ui/theme.hpp"
#include "aether/ui/components.hpp"
#include "aether/ui/animation.hpp"
#include "aether/core/log.hpp"
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace aether::ui {

static const wchar_t* SETTINGS_CLASS_NAME = L"AetherSettingsWindowClass";

bool SettingsWindow::CreateDeviceD3D() {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &m_pSwapChain,
        &m_pd3dDevice, &featureLevel, &m_pd3dDeviceContext
    );

    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &m_pSwapChain,
            &m_pd3dDevice, &featureLevel, &m_pd3dDeviceContext
        );
    }

    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

void SettingsWindow::CleanupDeviceD3D() {
    CleanupRenderTarget();
    m_pSwapChain.Reset();
    m_pd3dDeviceContext.Reset();
    m_pd3dDevice.Reset();
}

void SettingsWindow::CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> pBackBuffer;
    m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    m_pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(), nullptr, &m_mainRenderTargetView);
}

void SettingsWindow::CleanupRenderTarget() {
    m_mainRenderTargetView.Reset();
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return true;
    }

    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<SettingsWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool SettingsWindow::Create() {
    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = SETTINGS_CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - static_cast<int>(m_width)) / 2;
    int posY = (screenH - static_cast<int>(m_height)) / 2;

    m_hwnd = CreateWindowExW(
        0,
        SETTINGS_CLASS_NAME,
        L"Aether Control Center",
        WS_OVERLAPPEDWINDOW,
        posX, posY, m_width, m_height,
        nullptr, nullptr, GetModuleHandle(nullptr), this
    );

    if (!m_hwnd) return false;

    // Windows 11 Dark Mode Titlebar & Mica/Dark preference
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20, &darkMode, sizeof(darkMode)); // DWMWA_USE_IMMERSIVE_DARK_MODE

    if (!CreateDeviceD3D()) {
        CleanupDeviceD3D();
        return false;
    }

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    Theme::Instance().Apply();

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_pd3dDevice.Get(), m_pd3dDeviceContext.Get());

    AETHER_LOG_INFO("SettingsWindow created successfully");
    return true;
}

void SettingsWindow::Show(bool show) {
    m_visible = show;
    if (m_hwnd) {
        ShowWindow(m_hwnd, show ? SW_SHOW : SW_HIDE);
        if (show) {
            SetForegroundWindow(m_hwnd);
        }
    }
}

void SettingsWindow::Destroy() {
    if (m_hwnd) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CleanupDeviceD3D();
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void SettingsWindow::Render() {
    if (!m_visible || !m_hwnd) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    AnimationEngine::Instance().Update(ImGui::GetIO().DeltaTime);

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    const auto& theme = Theme::Instance().Colors();

    if (ImGui::Begin("AetherMainWindow", nullptr, windowFlags)) {
        // --- HEADER (54px) ---
        float headerHeight = 54.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Shell);
        ImGui::BeginChild("HeaderRegion", ImVec2(0, headerHeight), false, ImGuiWindowFlags_NoScrollbar);
        {
            ImGui::SetCursorPos(ImVec2(Spacing::LG, 14.0f));
            ImGui::TextColored(theme.TextPrimary, "A E T H E R");
            ImGui::SameLine();
            ImGui::TextColored(theme.TextMuted, "|  CUSTOMIZATION ECOSYSTEM");

            // Right side status badge & close
            float availW = ImGui::GetContentRegionAvail().x;
            ImGui::SameLine(availW - 120.0f);
            UI::StatusBadge("v1.0.0 NATIVE", theme.Accent);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // --- BODY (SIDEBAR + CONTENT) ---
        float bodyHeight = viewport->Size.y - headerHeight - 32.0f; // Leaving 32px for status bar
        float sidebarWidth = 230.0f;

        // 1. SIDEBAR
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Shell);
        ImGui::BeginChild("SidebarRegion", ImVec2(sidebarWidth, bodyHeight), false);
        {
            ImGui::SetCursorPosY(Spacing::MD);

            auto navItem = [this](SettingsTab tab, const char* id, const char* label) {
                bool isSelected = (m_activeTab == tab);
                if (UI::SidebarItem(id, label, isSelected, 38.0f)) {
                    m_activeTab = tab;
                }
            };

            navItem(SettingsTab::General,          "nav_gen",    "General");
            navItem(SettingsTab::DynamicIsland,    "nav_island", "Dynamic Island");
            navItem(SettingsTab::Widgets,          "nav_widg",   "Widgets");
            navItem(SettingsTab::Taskbar,          "nav_tb",     "Taskbar");
            navItem(SettingsTab::Desktop,          "nav_desk",   "Desktop");
            navItem(SettingsTab::Appearance,       "nav_app",    "Appearance");
            navItem(SettingsTab::Animations,       "nav_anim",   "Animations");
            navItem(SettingsTab::Media,            "nav_med",    "Media Integration");
            navItem(SettingsTab::SystemMonitoring, "nav_sys",    "System Monitoring");
            navItem(SettingsTab::GamingMode,       "nav_game",   "Gaming Mode");
            navItem(SettingsTab::Startup,          "nav_start",  "Startup & Keys");
            navItem(SettingsTab::Performance,      "nav_perf",   "Performance");
            navItem(SettingsTab::About,            "nav_abt",    "About & Presets");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // 2. CONTENT AREA
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Background);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Spacing::XL, Spacing::LG));
        ImGui::BeginChild("ContentRegion", ImVec2(0, bodyHeight), false);
        {
            // Content header with breadcrumbs
            const char* tabNames[] = {
                "General Settings", "Dynamic Island Configuration", "Desktop Widget Suite",
                "Taskbar Customization", "Desktop Composition", "Appearance & Themes",
                "Spring Physics & Animations", "Media & Audio Integration",
                "System Monitoring Telemetry", "Gaming Mode Performance Guard",
                "Startup & Hotkeys", "Diagnostics & Verification", "About Project Aether"
            };
            ImGui::TextColored(theme.TextPrimary, "%s", tabNames[static_cast<int>(m_activeTab)]);
            ImGui::TextColored(theme.TextMuted, "Aether Framework  >  %s", tabNames[static_cast<int>(m_activeTab)]);
            ImGui::Separator();
            ImGui::Spacing();

            // Render active settings page
            SettingsPages::Render(m_activeTab);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // --- STATUS FOOTER (32px) ---
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Shell);
        ImGui::BeginChild("StatusRegion", ImVec2(0, 32.0f), false, ImGuiWindowFlags_NoScrollbar);
        {
            ImGui::SetCursorPos(ImVec2(Spacing::LG, 7.0f));
            ImGui::TextColored(theme.TextMuted, "Engine Status: Active (C++20 DirectX 11 / DirectComposition)");

            float availW = ImGui::GetContentRegionAvail().x;
            ImGui::SameLine(availW - 170.0f);
            ImGui::TextColored(theme.Success, "[ 0%% CPU IDLE TARGET ]");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Overlay Toasts
        UI::RenderToasts();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    ImGui::Render();
    const float clearColor[4] = {0.07f, 0.07f, 0.09f, 1.0f};
    m_pd3dDeviceContext->OMSetRenderTargets(1, m_mainRenderTargetView.GetAddressOf(), nullptr);
    m_pd3dDeviceContext->ClearRenderTargetView(m_mainRenderTargetView.Get(), clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    m_pSwapChain->Present(1, 0);
}

LRESULT SettingsWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE: {
            if (m_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                m_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        }
        case WM_CLOSE: {
            Show(false);
            return 0;
        }
        case WM_DESTROY: {
            m_hwnd = nullptr;
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace aether::ui
