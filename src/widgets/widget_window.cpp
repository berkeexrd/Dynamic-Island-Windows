#include "aether/widgets/widget_window.hpp"
#include "aether/config/config_manager.hpp"
#include "aether/system/taskbar_service.hpp"
#include "aether/core/log.hpp"
#include <ctime>
#include <cmath>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

namespace aether::widgets {

static const wchar_t* WIDGET_CLASS_NAME = L"AetherDesktopWidgetClass";

DesktopWidgetWindow::DesktopWidgetWindow(WidgetType type) : m_type(type) {
    switch (type) {
        case WidgetType::SystemMonitor: m_width = 240; m_height = 140; break;
        case WidgetType::ClockCalendar:  m_width = 240; m_height = 96;  break;
        case WidgetType::NetworkMeter:  m_width = 240; m_height = 96;  break;
    }
}

DesktopWidgetWindow::~DesktopWidgetWindow() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

LRESULT CALLBACK DesktopWidgetWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DesktopWidgetWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<DesktopWidgetWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DesktopWidgetWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool DesktopWidgetWindow::Create() {
    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = WIDGET_CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    const auto& cfg = config::ConfigManager::Instance().Get().widgets;
    int posX = 40;
    int posY = 60;

    switch (m_type) {
        case WidgetType::SystemMonitor:
            posX = static_cast<int>(cfg.sysMonWidget.position.x);
            posY = static_cast<int>(cfg.sysMonWidget.position.y);
            break;
        case WidgetType::ClockCalendar:
            posX = static_cast<int>(cfg.clockWidget.position.x);
            posY = static_cast<int>(cfg.clockWidget.position.y);
            break;
        case WidgetType::NetworkMeter:
            posX = static_cast<int>(cfg.netWidget.position.x);
            posY = static_cast<int>(cfg.netWidget.position.y);
            break;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WIDGET_CLASS_NAME,
        L"Aether Desktop Widget",
        WS_POPUP,
        posX, posY, m_width, m_height,
        nullptr, nullptr, GetModuleHandle(nullptr), this
    );

    if (!m_hwnd) return false;

    m_dpiScale = static_cast<float>(GetDpiForWindow(m_hwnd)) / 96.0f;

    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, m_width, m_height)) return false;

    auto dwrite = m_dcomp->GetDWriteFactory();
    if (dwrite) {
        dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24.0f * m_dpiScale, L"en-us", &m_fontLarge);
        dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * m_dpiScale, L"en-us", &m_fontMedium);
        dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.0f * m_dpiScale, L"en-us", &m_fontSmall);
        if (m_fontLarge) m_fontLarge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (m_fontMedium) m_fontMedium->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (m_fontSmall) m_fontSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Place near desktop level (bottom of Z-order)
    SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    return true;
}

void DesktopWidgetWindow::Show(bool show) {
    if (m_hwnd) {
        ShowWindow(m_hwnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
}

void DesktopWidgetWindow::Update(float /*dt*/) {
    // Redraw on metrics/clock updates
}

void DesktopWidgetWindow::Render() {
    auto ctx = m_dcomp->BeginDraw();
    if (!ctx) return;

    // Background squircle card
    const auto& appCfg = config::ConfigManager::Instance().Get().appearance;
    core::Rect cardBounds{4.0f, 4.0f, m_width - 8.0f, m_height - 8.0f};
    auto cardSquircle = m_dcomp->CreateSquircleGeometry(cardBounds, 18.0f * m_dpiScale);

    // Multi-stage soft drop shadow
    if (appCfg.enableShadow) {
        core::Rect s1 = cardBounds;
        s1.y += 3.0f * m_dpiScale;
        auto sq1 = m_dcomp->CreateSquircleGeometry(s1, 20.0f * m_dpiScale);
        if (sq1) {
            auto b1 = m_dcomp->CreateSolidBrush(core::Color{0.0f, 0.0f, 0.0f, 0.12f * appCfg.shadowIntensity});
            ctx->FillGeometry(sq1.Get(), b1.Get());
        }
        core::Rect s2 = cardBounds;
        s2.y += 1.5f * m_dpiScale;
        auto sq2 = m_dcomp->CreateSquircleGeometry(s2, 18.5f * m_dpiScale);
        if (sq2) {
            auto b2 = m_dcomp->CreateSolidBrush(core::Color{0.0f, 0.0f, 0.0f, 0.28f * appCfg.shadowIntensity});
            ctx->FillGeometry(sq2.Get(), b2.Get());
        }
    }

    // Deep obsidian OLED surface
    auto glassGradient = m_dcomp->CreateLinearGradientBrush(
        core::Color{0.07f, 0.07f, 0.10f, 0.95f * appCfg.backgroundOpacity},
        core::Color{0.02f, 0.02f, 0.03f, 0.98f * appCfg.backgroundOpacity},
        D2D1::Point2F(cardBounds.Center().x, cardBounds.Top()),
        D2D1::Point2F(cardBounds.Center().x, cardBounds.Bottom())
    );
    if (glassGradient && cardSquircle) {
        ctx->FillGeometry(cardSquircle.Get(), glassGradient.Get());
    }

    // Specular top rim line
    auto rimGradient = m_dcomp->CreateLinearGradientBrush(
        core::Color{1.0f, 1.0f, 1.0f, 0.22f},
        core::Color{1.0f, 1.0f, 1.0f, 0.04f},
        D2D1::Point2F(cardBounds.Center().x, cardBounds.Top()),
        D2D1::Point2F(cardBounds.Center().x, cardBounds.Bottom())
    );
    if (rimGradient && cardSquircle) {
        ctx->DrawGeometry(cardSquircle.Get(), rimGradient.Get(), 1.0f * m_dpiScale);
    }

    switch (m_type) {
        case WidgetType::SystemMonitor:
            RenderSystemMonitor(ctx);
            break;
        case WidgetType::ClockCalendar:
            RenderClockCalendar(ctx);
            break;
        case WidgetType::NetworkMeter:
            RenderNetworkMeter(ctx);
            break;
    }

    m_dcomp->EndDraw();
}

static void DrawD2DText(ID2D1DeviceContext* ctx, const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, ID2D1Brush* brush) {
    if (ctx && format && brush && !text.empty()) {
        ctx->DrawText(text.c_str(), static_cast<UINT32>(text.length()), format, &rect, brush);
    }
}

void DesktopWidgetWindow::RenderSystemMonitor(ID2D1DeviceContext* ctx) {
    auto metrics = system::MetricsCollector::Instance().GetMetrics();
    const auto& appCfg = config::ConfigManager::Instance().Get().appearance;
    auto labelBrush = m_dcomp->CreateSolidBrush(core::Color{0.60f, 0.65f, 0.72f, 1.0f});
    auto textBrush = m_dcomp->CreateSolidBrush(core::Color{0.96f, 0.96f, 0.98f, 1.0f});

    // Title header
    DrawD2DText(ctx, L"SYSTEM PERFORMANCE", m_fontSmall.Get(), D2D1::RectF(16.0f, 12.0f, 220.0f, 28.0f), labelBrush.Get());

    // CPU Row
    DrawD2DText(ctx, L"CPU", m_fontMedium.Get(), D2D1::RectF(16.0f, 32.0f, 60.0f, 48.0f), textBrush.Get());
    wchar_t cpuBuf[16];
    swprintf_s(cpuBuf, L"%.1f%%", metrics.cpuUsagePercent);
    DrawD2DText(ctx, cpuBuf, m_fontMedium.Get(), D2D1::RectF(170.0f, 32.0f, 224.0f, 48.0f), textBrush.Get());

    // CPU Bar (Gradient)
    float barW = 208.0f;
    auto barBg = m_dcomp->CreateSolidBrush(core::Color{0.18f, 0.20f, 0.25f, 1.0f});
    ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(16.0f, 52.0f, 16.0f + barW, 56.5f), 2.25f, 2.25f), barBg.Get());

    auto cpuGrad = m_dcomp->CreateLinearGradientBrush(
        core::Color{0.0f, 0.85f, 1.0f, 1.0f},
        core::Color{0.0f, 0.45f, 0.95f, 1.0f},
        D2D1::Point2F(16.0f, 52.0f),
        D2D1::Point2F(16.0f + barW, 56.5f)
    );
    if (cpuGrad) {
        ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(16.0f, 52.0f, 16.0f + barW * (metrics.cpuUsagePercent / 100.0f), 56.5f), 2.25f, 2.25f), cpuGrad.Get());
    }

    // RAM Row
    DrawD2DText(ctx, L"RAM", m_fontMedium.Get(), D2D1::RectF(16.0f, 66.0f, 60.0f, 82.0f), textBrush.Get());
    wchar_t ramBuf[16];
    float ramGb = static_cast<float>(metrics.ramUsedBytes) / (1024.0f * 1024.0f * 1024.0f);
    swprintf_s(ramBuf, L"%.1f GB", ramGb);
    DrawD2DText(ctx, ramBuf, m_fontMedium.Get(), D2D1::RectF(170.0f, 66.0f, 224.0f, 82.0f), textBrush.Get());

    // RAM Bar (Gradient)
    ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(16.0f, 86.0f, 16.0f + barW, 90.5f), 2.25f, 2.25f), barBg.Get());
    auto ramGrad = m_dcomp->CreateLinearGradientBrush(
        core::Color{0.10f, 0.92f, 0.50f, 1.0f},
        core::Color{0.05f, 0.70f, 0.35f, 1.0f},
        D2D1::Point2F(16.0f, 86.0f),
        D2D1::Point2F(16.0f + barW, 90.5f)
    );
    if (ramGrad) {
        ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(16.0f, 86.0f, 16.0f + barW * (metrics.ramUsagePercent / 100.0f), 90.5f), 2.25f, 2.25f), ramGrad.Get());
    }

    // Sparkline history on bottom
    if (metrics.cpuHistory.size() > 1) {
        float sparkX = 16.0f;
        float sparkY = 125.0f;
        float sparkW = barW;
        float sparkH = 22.0f;
        float stepX = sparkW / static_cast<float>(metrics.cpuHistory.size() - 1);

        auto sparkBrush = m_dcomp->CreateSolidBrush(core::Color::FromHex(appCfg.accentColor).WithAlpha(0.75f));
        for (size_t i = 0; i < metrics.cpuHistory.size() - 1; ++i) {
            float y1 = sparkY - (metrics.cpuHistory[i] / 100.0f) * sparkH;
            float y2 = sparkY - (metrics.cpuHistory[i + 1] / 100.0f) * sparkH;
            ctx->DrawLine(
                D2D1::Point2F(sparkX + i * stepX, y1),
                D2D1::Point2F(sparkX + (i + 1) * stepX, y2),
                sparkBrush.Get(),
                1.5f
            );
        }
    }
}

void DesktopWidgetWindow::RenderClockCalendar(ID2D1DeviceContext* ctx) {
    std::time_t t = std::time(nullptr);
    std::tm now{};
    localtime_s(&now, &t);

    wchar_t timeBuf[32];
    swprintf_s(timeBuf, L"%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);

    wchar_t dateBuf[64];
    wcsftime(dateBuf, sizeof(dateBuf) / sizeof(wchar_t), L"%A, %B %d", &now);

    auto timeBrush = m_dcomp->CreateSolidBrush(core::Color{0.98f, 0.98f, 0.98f, 1.0f});
    auto dateBrush = m_dcomp->CreateSolidBrush(core::Color{0.65f, 0.70f, 0.78f, 1.0f});

    DrawD2DText(ctx, timeBuf, m_fontLarge.Get(), D2D1::RectF(16.0f, 14.0f, 224.0f, 50.0f), timeBrush.Get());
    DrawD2DText(ctx, dateBuf, m_fontMedium.Get(), D2D1::RectF(16.0f, 56.0f, 224.0f, 80.0f), dateBrush.Get());
}

void DesktopWidgetWindow::RenderNetworkMeter(ID2D1DeviceContext* ctx) {
    auto metrics = system::MetricsCollector::Instance().GetMetrics();
    auto labelBrush = m_dcomp->CreateSolidBrush(core::Color{0.60f, 0.65f, 0.72f, 1.0f});
    auto textBrush = m_dcomp->CreateSolidBrush(core::Color{0.95f, 0.95f, 0.98f, 1.0f});

    DrawD2DText(ctx, L"NETWORK ACTIVITY", m_fontSmall.Get(), D2D1::RectF(16.0f, 12.0f, 220.0f, 28.0f), labelBrush.Get());

    // Download
    auto downBrush = m_dcomp->CreateSolidBrush(core::Color{0.18f, 0.85f, 0.50f, 1.0f});
    DrawD2DText(ctx, L"DOWN", m_fontMedium.Get(), D2D1::RectF(16.0f, 34.0f, 80.0f, 52.0f), downBrush.Get());
    wchar_t downBuf[32];
    float downMb = static_cast<float>(metrics.netDownloadBytesPerSec) / (1024.0f * 1024.0f);
    swprintf_s(downBuf, L"%.2f MB/s", downMb);
    DrawD2DText(ctx, downBuf, m_fontMedium.Get(), D2D1::RectF(130.0f, 34.0f, 224.0f, 52.0f), textBrush.Get());

    // Upload
    auto upBrush = m_dcomp->CreateSolidBrush(core::Color{0.0f, 0.70f, 1.0f, 1.0f});
    DrawD2DText(ctx, L"UP", m_fontMedium.Get(), D2D1::RectF(16.0f, 58.0f, 80.0f, 76.0f), upBrush.Get());
    wchar_t upBuf[32];
    float upMb = static_cast<float>(metrics.netUploadBytesPerSec) / (1024.0f * 1024.0f);
    swprintf_s(upBuf, L"%.2f MB/s", upMb);
    DrawD2DText(ctx, upBuf, m_fontMedium.Get(), D2D1::RectF(130.0f, 58.0f, 224.0f, 76.0f), textBrush.Get());
}

LRESULT DesktopWidgetWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            m_isDragging = true;
            SetCapture(hwnd);
            GetCursorPos(&m_dragStartCursor);
            RECT rect;
            GetWindowRect(hwnd, &rect);
            m_dragStartWindow = {rect.left, rect.top};
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (m_isDragging) {
                POINT currentCursor;
                GetCursorPos(&currentCursor);
                int dx = currentCursor.x - m_dragStartCursor.x;
                int dy = currentCursor.y - m_dragStartCursor.y;

                int newX = m_dragStartWindow.x + dx;
                int newY = m_dragStartWindow.y + dy;

                const auto& wcfg = config::ConfigManager::Instance().Get().widgets;
                if (wcfg.snapToGrid && wcfg.gridSize > 0.0f) {
                    int grid = static_cast<int>(wcfg.gridSize);
                    newX = (newX / grid) * grid;
                    newY = (newY / grid) * grid;
                }

                SetWindowPos(hwnd, nullptr, newX, newY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

                // Update config
                auto& mutableCfg = config::ConfigManager::Instance().GetMutable();
                switch (m_type) {
                    case WidgetType::SystemMonitor:
                        mutableCfg.widgets.sysMonWidget.position = {static_cast<float>(newX), static_cast<float>(newY)};
                        break;
                    case WidgetType::ClockCalendar:
                        mutableCfg.widgets.clockWidget.position = {static_cast<float>(newX), static_cast<float>(newY)};
                        break;
                    case WidgetType::NetworkMeter:
                        mutableCfg.widgets.netWidget.position = {static_cast<float>(newX), static_cast<float>(newY)};
                        break;
                }
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (m_isDragging) {
                m_isDragging = false;
                ReleaseCapture();
                config::ConfigManager::Instance().Save();
            }
            return 0;
        }

        case WM_DESTROY: {
            m_hwnd = nullptr;
            return 0;
        }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void WidgetManager::Initialize() {
    m_widgets.clear();
    m_widgets.push_back(std::make_unique<DesktopWidgetWindow>(WidgetType::SystemMonitor));
    m_widgets.push_back(std::make_unique<DesktopWidgetWindow>(WidgetType::ClockCalendar));
    m_widgets.push_back(std::make_unique<DesktopWidgetWindow>(WidgetType::NetworkMeter));

    for (auto& w : m_widgets) {
        w->Create();
    }
    AETHER_LOG_INFO("WidgetManager initialized with " << m_widgets.size() << " desktop widgets");
}

void WidgetManager::Shutdown() {
    m_widgets.clear();
}

void WidgetManager::Update(float dt) {
    if (!m_visible) return;
    for (auto& w : m_widgets) {
        w->Update(dt);
    }
}

void WidgetManager::Render() {
    if (!m_visible) return;
    for (auto& w : m_widgets) {
        w->Render();
    }
}

void WidgetManager::SetVisible(bool visible) {
    m_visible = visible;
    for (auto& w : m_widgets) {
        w->Show(visible);
    }
}

} // namespace aether::widgets
