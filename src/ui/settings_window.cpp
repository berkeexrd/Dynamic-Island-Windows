#include "aether/ui/settings_window.hpp"
#include "aether/ui/theme.hpp"
#include "aether/ui/components.hpp"
#include "aether/ui/animation.hpp"
#include "aether/core/log.hpp"
#include "aether/core/i18n.hpp"
#include "aether/render/glass.hpp"
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <string>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#define L10N(en, tr) (aether::i18n::Turkish() ? (tr) : (en))

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

    Scale = std::max(1.0f, static_cast<float>(GetDpiForSystem()) / 96.0f);
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    m_width = std::min(static_cast<uint32_t>(1120.0f * Scale), static_cast<uint32_t>(screenW - 60));
    m_height = std::min(static_cast<uint32_t>(760.0f * Scale), static_cast<uint32_t>(screenH - 80));
    int posX = (screenW - static_cast<int>(m_width)) / 2;
    int posY = (screenH - static_cast<int>(m_height)) / 2;

    m_hwnd = CreateWindowExW(
        0,
        SETTINGS_CLASS_NAME,
        L"Aether Control Center",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU,
        posX, posY, m_width, m_height,
        nullptr, nullptr, GetModuleHandle(nullptr), this
    );

    if (!m_hwnd) return false;

    // Windows 11 Dark Mode Titlebar & Mica/Dark preference
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20, &darkMode, sizeof(darkMode)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    const int corner = 2;   // DWMWCP_ROUND: rounded window corners (Windows 11)
    DwmSetWindowAttribute(m_hwnd, 33, &corner, sizeof(corner));
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);   // frameless: we draw the caption

    if (!CreateDeviceD3D()) {
        CleanupDeviceD3D();
        return false;
    }

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    {   // Segoe UI (+ Semibold for headings, Fluent Icons for the sidebar) with Latin Extended so Turkish letters render properly
        static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2013, 0x2026, 0x2190, 0x2193, 0};
        static const ImWchar iconRanges[] = {0xE700, 0xF8FF, 0};
        const char* reg = "C:\\Windows\\Fonts\\segoeui.ttf";
        const char* semi = "C:\\Windows\\Fonts\\seguisb.ttf";
        const char* bold = "C:\\Windows\\Fonts\\segoeuib.ttf";
        const char* icons = "C:\\Windows\\Fonts\\SegoeIcons.ttf";
        auto exists = [](const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; };
        ImFontConfig fc; fc.OversampleH = 3; fc.OversampleV = 2; fc.PixelSnapH = false;
        if (exists(reg)) {
            const char* head = exists(semi) ? semi : (exists(bold) ? bold : reg);
            io.FontDefault = io.Fonts->AddFontFromFileTTF(reg, 15.5f * Scale, &fc, ranges);
            FontSmall = io.Fonts->AddFontFromFileTTF(reg, 13.0f * Scale, &fc, ranges);
            FontBold = io.Fonts->AddFontFromFileTTF(head, 17.0f * Scale, &fc, ranges);
            FontTitle = io.Fonts->AddFontFromFileTTF(head, 26.0f * Scale, &fc, ranges);
            if (exists(icons)) FontIcons = io.Fonts->AddFontFromFileTTF(icons, 16.0f * Scale, &fc, iconRanges);
        }
    }
    render::glass::SyncTheme();
    Theme::Instance().UseGlassTheme(render::glass::ThemeIndex());

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

    if (render::glass::SyncTheme()) Theme::Instance().UseGlassTheme(render::glass::ThemeIndex());
    const auto& theme = Theme::Instance().Colors();

    const ImVec4 winBg(theme.Background.x * 0.62f, theme.Background.y * 0.62f, theme.Background.z * 0.62f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, winBg);
    if (ImGui::Begin("AetherMainWindow", nullptr, windowFlags)) {
        const float m = Sc(12.0f);                       // gap between the floating panels
        const float sidebarWidth = Sc(244.0f);
        const float panelH = viewport->Size.y - m * 2.0f;
        const float panelR = Sc(20.0f);
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        (void)wdl;

        auto utf8 = [](unsigned cp) { char b[5] = {}; if (cp < 0x80) b[0] = static_cast<char>(cp); else if (cp < 0x800) { b[0] = static_cast<char>(0xC0 | (cp >> 6)); b[1] = static_cast<char>(0x80 | (cp & 0x3F)); } else { b[0] = static_cast<char>(0xE0 | (cp >> 12)); b[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); b[2] = static_cast<char>(0x80 | (cp & 0x3F)); } return std::string(b); };

        if (SettingsPages::WelcomeNeeded()) {
            SettingsPages::RenderWelcome(viewport->Size.x, viewport->Size.y);
        } else {
        // ---- SIDEBAR (floating rounded panel) --------------------------------------------------
        ImGui::SetCursorPos(ImVec2(m, m));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Shell);
        ImGui::PushStyleColor(ImGuiCol_Border, theme.BorderSubtle);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, panelR);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sc(14.0f), Sc(4.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("SidebarRegion", ImVec2(sidebarWidth, panelH), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        {
            const ImVec2 o = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 b0(o.x + Sc(4.0f), o.y + Sc(18.0f)), b1(b0.x + Sc(42.0f), b0.y + Sc(42.0f));
            dl->AddRectFilled(ImVec2(b0.x - 4, b0.y - 4), ImVec2(b1.x + 4, b1.y + 4), ImGui::GetColorU32(ImVec4(theme.Accent.x, theme.Accent.y, theme.Accent.z, 0.22f)), Sc(16.0f));
            dl->AddRectFilledMultiColor(b0, b1, ImGui::GetColorU32(theme.AccentHover), ImGui::GetColorU32(theme.Accent), ImGui::GetColorU32(theme.AccentActive), ImGui::GetColorU32(theme.Accent));
            dl->AddRect(b0, b1, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.28f)), Sc(13.0f), 0, 1.0f);
            if (FontBold) ImGui::PushFont(FontBold);
            const ImVec2 ts = ImGui::CalcTextSize("A");
            dl->AddText(ImVec2(b0.x + (Sc(42.0f) - ts.x) * 0.5f, b0.y + (Sc(42.0f) - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), "A");
            dl->AddText(ImVec2(b1.x + Sc(14.0f), b0.y + 1.0f), ImGui::GetColorU32(theme.TextPrimary), "Aether");
            if (FontBold) ImGui::PopFont();
            if (FontSmall) ImGui::PushFont(FontSmall);
            dl->AddText(ImVec2(b1.x + Sc(14.0f), b0.y + Sc(23.0f)), ImGui::GetColorU32(theme.TextMuted), L10N("Control Center", "Kontrol Merkezi"));
            if (FontSmall) ImGui::PopFont();
            ImGui::SetCursorPosY(Sc(86.0f));

            auto group = [&](const char* title) {
                ImGui::Dummy(ImVec2(0.0f, Sc(10.0f)));
                if (FontSmall) ImGui::PushFont(FontSmall);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Sc(10.0f));
                ImGui::TextColored(theme.TextDisabled, "%s", title);
                if (FontSmall) ImGui::PopFont();
                ImGui::Dummy(ImVec2(0.0f, Sc(4.0f)));
            };
            auto nav = [&](SettingsTab tab, const char* id, const char* label, unsigned icon) {
                const std::string ic = utf8(icon);
                if (UI::SidebarItem(id, label, m_activeTab == tab, Sc(42.0f), ic.c_str())) m_activeTab = tab;
                ImGui::Dummy(ImVec2(0.0f, Sc(3.0f)));
            };
            group(L10N("GENERAL", "GENEL"));
            nav(SettingsTab::General,       "nav_gen",    L10N("General", "Genel"), 0xE713);
            nav(SettingsTab::Appearance,    "nav_app",    L10N("Appearance & Themes", "Görünüm ve Temalar"), 0xE790);
            group(L10N("COMPONENTS", "BİLEŞENLER"));
            nav(SettingsTab::DynamicIsland, "nav_island", "Dynamic Island", 0xE8B2);
            nav(SettingsTab::Widgets,       "nav_widg",   L10N("Widgets", "Widget'lar"), 0xE8A1);
            group(L10N("SYSTEM", "SİSTEM"));
            nav(SettingsTab::Media,            "nav_med",  L10N("Media", "Medya"), 0xE8D6);
            nav(SettingsTab::SystemMonitoring, "nav_sys",  L10N("Hardware Monitor", "Donanım İzleme"), 0xEEA1);
            nav(SettingsTab::GamingMode,       "nav_game", L10N("Gaming Mode", "Oyun Modu"), 0xE7FC);
            nav(SettingsTab::About,            "nav_abt",  L10N("About", "Hakkında"), 0xE946);

            // footer: status pill
            const float fy = panelH - Sc(54.0f);
            const ImVec2 w0 = ImGui::GetWindowPos();
            const ImVec2 p0(w0.x + Sc(14.0f), w0.y + fy), p1(w0.x + sidebarWidth - Sc(14.0f), w0.y + fy + Sc(34.0f));
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.045f)), Sc(12.0f));
            dl->AddCircleFilled(ImVec2(p0.x + Sc(16.0f), (p0.y + p1.y) * 0.5f), Sc(4.0f), ImGui::GetColorU32(theme.Success));
            if (FontSmall) ImGui::PushFont(FontSmall);
            const ImVec2 fs = ImGui::CalcTextSize("A");
            dl->AddText(ImVec2(p0.x + Sc(30.0f), (p0.y + p1.y - fs.y) * 0.5f), ImGui::GetColorU32(theme.TextMuted), L10N("Running  -  v1.0", "Çalışıyor  -  v1.0"));
            if (FontSmall) ImGui::PopFont();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);

        // ---- CONTENT (floating rounded panel with its own header) -----------------------------------
        ImGui::SetCursorPos(ImVec2(m * 2.0f + sidebarWidth, m));
        const float contentW = viewport->Size.x - (m * 3.0f + sidebarWidth);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Background);
        ImGui::PushStyleColor(ImGuiCol_Border, theme.BorderSubtle);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, panelR);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("ContentRegion", ImVec2(contentW, panelH), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        {
            struct Head { const char* title; const char* sub; };
            const Head heads[] = {
                {L10N("General", "Genel"), L10N("Startup, display and shortcuts", "Başlangıç, ekran ve kısayollar")},
                {"Dynamic Island", L10N("The floating pill at the top of the screen", "Ekranın üstündeki yüzen hap")},
                {L10N("Widgets", "Widget'lar"), L10N("Live cards on your desktop", "Masaüstündeki canlı kartlar")},
                {L10N("Taskbar", "Görev Çubuğu"), L10N("Material of the Windows taskbar", "Windows görev çubuğunun malzemesi")},
                {L10N("Desktop", "Masaüstü"), L10N("Desktop elements", "Masaüstü öğeleri")},
                {L10N("Appearance & Themes", "Görünüm ve Temalar"), L10N("Pick the look of the whole shell", "Tüm arayüzün görünümünü seç")},
                {L10N("Motion", "Hareket"), L10N("Spring physics", "Yay fiziği")},
                {L10N("Media", "Medya"), L10N("Music and video playback", "Müzik ve video oynatma")},
                {L10N("Hardware Monitor", "Donanım İzleme"), L10N("Live CPU, memory, graphics and network", "Canlı CPU, bellek, ekran kartı ve ağ")},
                {L10N("Gaming Mode", "Oyun Modu"), L10N("Stay out of the way of games", "Oyunların yoluna çıkma")},
                {L10N("Shortcuts", "Kısayollar"), L10N("Global hotkeys", "Genel kısayol tuşları")},
                {L10N("Diagnostics", "Tanılama"), L10N("Rendering status", "Çizim durumu")},
                {L10N("About", "Hakkında"), L10N("About Aether", "Aether hakkında")},
            };
            const Head& h = heads[static_cast<int>(m_activeTab)];
            const float headH = Sc(84.0f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 w0 = ImGui::GetWindowPos();
            // header: title + subtitle on the left, caption buttons on the right
            ImGui::SetCursorPos(ImVec2(Sc(34.0f), Sc(18.0f)));
            if (FontTitle) ImGui::PushFont(FontTitle);
            ImGui::TextColored(theme.TextPrimary, "%s", h.title);
            if (FontTitle) ImGui::PopFont();
            ImGui::SetCursorPos(ImVec2(Sc(35.0f), Sc(52.0f)));
            ImGui::TextColored(theme.TextMuted, "%s", h.sub);
            const float bw = Sc(40.0f), bh = Sc(30.0f), by = Sc(14.0f);
            for (int k = 0; k < 2; ++k) {   // 0 = minimise, 1 = close
                const float bx = contentW - Sc(16.0f) - (2 - k) * (bw + Sc(4.0f)) + Sc(4.0f);
                ImGui::SetCursorPos(ImVec2(bx, by));
                char id[16]; snprintf(id, sizeof(id), "##cap%d", k);
                const bool clicked = ImGui::InvisibleButton(id, ImVec2(bw, bh));
                const bool hov = ImGui::IsItemHovered();
                const ImVec2 a(w0.x + bx, w0.y + by), b(a.x + bw, a.y + bh);
                if (hov) dl->AddRectFilled(a, b, k == 1 ? IM_COL32(232, 72, 85, 235) : ImGui::GetColorU32(ImVec4(1, 1, 1, 0.09f)), Sc(10.0f));
                if (FontIcons) {
                    const std::string g = utf8(k == 0 ? 0xE921 : 0xE8BB);
                    ImGui::PushFont(FontIcons);
                    const ImVec2 gs = ImGui::CalcTextSize(g.c_str());
                    dl->AddText(ImVec2(a.x + (bw - gs.x) * 0.5f, a.y + (bh - gs.y) * 0.5f), hov && k == 1 ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(theme.TextSecondary), g.c_str());
                    ImGui::PopFont();
                }
                if (clicked) { if (k == 0) ShowWindow(m_hwnd, SW_MINIMIZE); else Show(false); }
            }
            dl->AddLine(ImVec2(w0.x + Sc(24.0f), w0.y + headH), ImVec2(w0.x + contentW - Sc(24.0f), w0.y + headH), ImGui::GetColorU32(theme.BorderSubtle), 1.0f);

            // scrolling page area
            ImGui::SetCursorPos(ImVec2(0.0f, headH + Sc(2.0f)));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sc(34.0f), Sc(16.0f)));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
            ImGui::BeginChild("PageScroll", ImVec2(contentW, panelH - headH - Sc(2.0f)), ImGuiChildFlags_AlwaysUseWindowPadding);
            SettingsPages::Render(m_activeTab);
            ImGui::Dummy(ImVec2(0.0f, Sc(24.0f)));
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        }

        // Overlay Toasts
        UI::RenderToasts();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ImGui::Render();
    const auto& bgc = Theme::Instance().Colors().Background;
    const float clearColor[4] = {bgc.x * 0.62f, bgc.y * 0.62f, bgc.z * 0.62f, 1.0f};
    m_pd3dDeviceContext->OMSetRenderTargets(1, m_mainRenderTargetView.GetAddressOf(), nullptr);
    m_pd3dDeviceContext->ClearRenderTargetView(m_mainRenderTargetView.Get(), clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    m_pSwapChain->Present(1, 0);
}

LRESULT SettingsWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wParam) return 0;   // the whole window is client area: we draw our own caption
            break;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lParam);
            mm->ptMinTrackSize.x = static_cast<LONG>(900.0f * Scale);
            mm->ptMinTrackSize.y = static_cast<LONG>(600.0f * Scale);
            return 0;
        }
        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            RECT rc; GetClientRect(hwnd, &rc);
            const int e = static_cast<int>(6.0f * Scale);
            const bool L = pt.x < e, R = pt.x >= rc.right - e, T = pt.y < e, B = pt.y >= rc.bottom - e;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
            if (pt.y < static_cast<int>(72.0f * Scale) && pt.x < rc.right - static_cast<int>(120.0f * Scale)) return HTCAPTION;   // drag by the header
            return HTCLIENT;
        }
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
