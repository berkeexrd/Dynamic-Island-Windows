#include "aether/widgets/widget_window.hpp"
#include "aether/config/config_manager.hpp"
#include "aether/system/taskbar_service.hpp"
#include "aether/core/log.hpp"
#include "aether/core/i18n.hpp"
#include "aether/render/glass.hpp"
#include "aether/media/media_service.hpp"
#include "aether/bar/weather.hpp"
#include "stb_image.h"
#include <shlwapi.h>
#include <windowsx.h>
#include <ctime>
#include <cmath>
#include <algorithm>
#include <vector>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

namespace aether::widgets {
namespace i18n = aether::i18n;

static const wchar_t* WIDGET_CLASS_NAME = L"AetherDesktopWidgetClass";

namespace {
// Card sizes from the mockup (logical px) and the transparent margin that holds the glow.
constexpr float kPad = 24.0f;
constexpr float kCardW = 264.0f;
constexpr float kCardH[6] = {138.0f, 112.0f, 92.0f, 150.0f, 178.0f, 96.0f};   // performance, network, clock, storage, weather, now playing
constexpr float kRadius = 20.0f;

int Slot(WidgetType t) {
    switch (t) {
        case WidgetType::SystemMonitor: return 0;
        case WidgetType::NetworkMeter:  return 1;
        case WidgetType::Storage:       return 3;
        case WidgetType::Weather:       return 4;
        case WidgetType::NowPlaying:    return 5;
        default:                        return 2;
    }
}
} // namespace

DesktopWidgetWindow::DesktopWidgetWindow(WidgetType type) : m_type(type) {
    m_width = static_cast<uint32_t>(kCardW + kPad * 2);
    m_height = static_cast<uint32_t>(kCardH[Slot(type)] + kPad * 2);
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

namespace {
core::Color& kViolet = render::glass::kViolet;
core::Color& kVioletLight = render::glass::kVioletLight;
core::Color& kVioletDeep = render::glass::kVioletDeep;
constexpr core::Color kText{0.97f, 0.96f, 1.0f, 1.0f};
constexpr core::Color kMuted{0.70f, 0.68f, 0.80f, 1.0f};

core::Color A(const core::Color& c, float a) { return core::Color{c.r, c.g, c.b, a}; }

// Positions from older builds (left edge stack, or the first violet build) count as "not placed".
bool IsUnplaced(const core::Vec2& p) {
    if (p.x < 0.0f || p.y < 0.0f) return true;
    return (p.x == 40.0f && (p.y == 60.0f || p.y == 210.0f || p.y == 320.0f));
}
} // namespace

bool DesktopWidgetWindow::Create() {
    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = WIDGET_CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    m_dpiScale = std::max(1.0f, static_cast<float>(GetDpiForSystem()) / 96.0f);
    const int pxW = static_cast<int>(m_width * m_dpiScale), pxH = static_cast<int>(m_height * m_dpiScale);

    const auto& cfg = config::ConfigManager::Instance().Get().widgets;
    const core::Vec2* saved = nullptr;
    switch (m_type) {
        case WidgetType::SystemMonitor: saved = &cfg.sysMonWidget.position; break;
        case WidgetType::NetworkMeter:  saved = &cfg.netWidget.position;    break;
        case WidgetType::ClockCalendar: saved = &cfg.clockWidget.position;  break;
        case WidgetType::Storage:       saved = &cfg.storageWidget.position; break;
        case WidgetType::Weather:       saved = &cfg.weatherWidget.position; break;
        case WidgetType::NowPlaying:    saved = &cfg.nowWidget.position;     break;
    }

    int posX = static_cast<int>(saved->x), posY = static_cast<int>(saved->y);
    if (IsUnplaced(*saved)) {
        // stacked on the right edge like the mockup: performance, network, clock
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        float cardTop = 100.0f;
        const int slot = Slot(m_type);
        for (int i = 0; i < std::min(slot, 4); ++i) cardTop += kCardH[i] + 14.0f;   // weather sits bottom-left instead
        if (m_type == WidgetType::NowPlaying) cardTop = 100.0f + kCardH[0] + kCardH[1] + kCardH[2] + kCardH[3] + 4 * 14.0f;
        posX = work.right - static_cast<int>((kCardW + kPad + 16.0f) * m_dpiScale);
        posY = work.top + static_cast<int>((cardTop - kPad) * m_dpiScale);
        if (m_type == WidgetType::Weather) {
            posX = work.left + static_cast<int>(8.0f * m_dpiScale);
            posY = work.bottom - pxH - static_cast<int>(70.0f * m_dpiScale);
        }
    }

    m_hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WIDGET_CLASS_NAME,
        L"Aether Desktop Widget",
        WS_POPUP,
        posX, posY, pxW, pxH,
        nullptr, nullptr, GetModuleHandle(nullptr), this
    );

    if (!m_hwnd) return false;

    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, static_cast<uint32_t>(pxW), static_cast<uint32_t>(pxH))) return false;
    m_brush = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 1.0f});

    auto dwrite = m_dcomp->GetDWriteFactory();
    if (dwrite) {
        ComPtr<IDWriteFontCollection> col;
        dwrite->GetSystemFontCollection(&col, FALSE);
        auto has = [&](const wchar_t* n) { UINT32 i = 0; BOOL ex = FALSE; return col && SUCCEEDED(col->FindFamilyName(n, &i, &ex)) && ex; };
        const wchar_t* display = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
        const wchar_t* text = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        const wchar_t* icons = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : (has(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets" : nullptr);
        m_hasIconFont = icons != nullptr;

        auto make = [&](ComPtr<IDWriteTextFormat>& f, const wchar_t* family, DWRITE_FONT_WEIGHT w, float size, DWRITE_TEXT_ALIGNMENT al) {
            if (!family) return;
            dwrite->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
            if (f) {
                f->SetTextAlignment(al);
                f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        };
        make(m_fontLarge,  display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 27.0f, DWRITE_TEXT_ALIGNMENT_LEADING);   // clock
        make(m_fontMedium, display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, DWRITE_TEXT_ALIGNMENT_LEADING);   // network values
        make(m_fontSmall,  text,    DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f, DWRITE_TEXT_ALIGNMENT_LEADING);   // clock date
        make(m_fontTitle,  display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, DWRITE_TEXT_ALIGNMENT_LEADING);   // card title
        make(m_fontCenter, display, DWRITE_FONT_WEIGHT_BOLD,      13.5f, DWRITE_TEXT_ALIGNMENT_CENTER);    // ring value
        make(m_fontTiny,   text,    DWRITE_FONT_WEIGHT_SEMI_BOLD,  8.5f, DWRITE_TEXT_ALIGNMENT_CENTER);    // ring label
        make(m_fontIcon,   icons,   DWRITE_FONT_WEIGHT_NORMAL,    26.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
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

void DesktopWidgetWindow::Update(float dt) {
    m_time += dt;
    const auto& w = config::ConfigManager::Instance().Get().widgets;
    const config::DesktopWidgetConfig* c = nullptr;
    switch (m_type) {
        case WidgetType::SystemMonitor: c = &w.sysMonWidget; break;
        case WidgetType::NetworkMeter:  c = &w.netWidget; break;
        case WidgetType::ClockCalendar: c = &w.clockWidget; break;
        case WidgetType::Storage:       c = &w.storageWidget; break;
        case WidgetType::Weather:       c = &w.weatherWidget; break;
        case WidgetType::NowPlaying:    c = &w.nowWidget; break;
    }
    const bool want = w.enabled && c && c->enabled;
    if (want != m_shownNow && m_hwnd) { m_shownNow = want; ShowWindow(m_hwnd, want ? SW_SHOWNOACTIVATE : SW_HIDE); }
}

void DesktopWidgetWindow::DrawText(ID2D1DeviceContext* ctx, const std::wstring& t, IDWriteTextFormat* f, const D2D1_RECT_F& r, const core::Color& c) {
    if (!ctx || !f || !m_brush || t.empty()) return;
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    ctx->DrawText(t.c_str(), static_cast<UINT32>(t.size()), f, &r, m_brush.Get());
}

// Thin ring like the mockup: faint track, violet arc with a soft glow, value + label inside.
void DesktopWidgetWindow::DrawRing(ID2D1DeviceContext* ctx, float cx, float cy, float radius, float thickness, float frac,
                                   const std::wstring& value, const std::wstring& label) {
    frac = std::clamp(frac, 0.0f, 1.0f);
    const float rr = radius - thickness * 0.5f;
    m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.10f));
    ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rr, rr), m_brush.Get(), thickness);

    auto grad = m_dcomp->CreateLinearGradientBrush(kVioletLight, kViolet, D2D1::Point2F(cx, cy - radius), D2D1::Point2F(cx, cy + radius));
    auto stroke = m_dcomp->CreateRoundedStrokeStyle();
    auto fac = m_dcomp->GetD2DFactory();
    if (grad && fac && frac > 0.004f) {
        ComPtr<ID2D1Geometry> arcGeo;
        if (frac >= 0.995f) {
            ComPtr<ID2D1EllipseGeometry> e;
            if (SUCCEEDED(fac->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), rr, rr), &e))) arcGeo = e;
        } else {
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(fac->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
                const float a0 = -1.5707963f, a1 = a0 + frac * 6.2831853f;
                sink->BeginFigure(D2D1::Point2F(cx + std::cos(a0) * rr, cy + std::sin(a0) * rr), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + std::cos(a1) * rr, cy + std::sin(a1) * rr), D2D1::SizeF(rr, rr), 0.0f,
                                              D2D1_SWEEP_DIRECTION_CLOCKWISE, frac > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                arcGeo = path;
            }
        }
        if (arcGeo) {
            for (int i = 3; i >= 1; --i) {   // glow under the arc
                m_brush->SetColor(D2D1::ColorF(kViolet.r, kViolet.g, kViolet.b, 0.10f));
                ctx->DrawGeometry(arcGeo.Get(), m_brush.Get(), thickness + i * 3.0f, stroke.Get());
            }
            ctx->DrawGeometry(arcGeo.Get(), grad.Get(), thickness, stroke.Get());
        }
    }
    DrawText(ctx, value, m_fontCenter.Get(), D2D1::RectF(cx - radius, cy - 13.0f, cx + radius, cy + 4.0f), kText);
    DrawText(ctx, label, m_fontTiny.Get(), D2D1::RectF(cx - radius, cy + 3.0f, cx + radius, cy + 14.0f), kMuted);
}

void DesktopWidgetWindow::DrawSpark(ID2D1DeviceContext* ctx, const std::vector<float>& v, float peak, const core::Rect& r, const core::Color& c, bool fill) {
    if (v.size() < 2 || peak <= 0.0f) return;
    auto fac = m_dcomp->GetD2DFactory();
    if (!fac) return;
    const float step = r.width / static_cast<float>(v.size() - 1);
    auto pt = [&](size_t i) { return D2D1::Point2F(r.x + step * i, r.Bottom() - std::clamp(v[i] / peak, 0.0f, 1.0f) * r.height); };
    auto build = [&](bool closed, ComPtr<ID2D1PathGeometry>& out) {
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(fac->CreatePathGeometry(&out)) || FAILED(out->Open(&sink))) { out.Reset(); return; }
        sink->BeginFigure(closed ? D2D1::Point2F(r.x, r.Bottom()) : pt(0), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        if (closed) sink->AddLine(pt(0));
        for (size_t i = 1; i < v.size(); ++i) {   // smooth through midpoints
            const D2D1_POINT_2F p0 = pt(i - 1), p1 = pt(i);
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(p0, D2D1::Point2F((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f)));
        }
        sink->AddLine(pt(v.size() - 1));
        if (closed) sink->AddLine(D2D1::Point2F(r.Right(), r.Bottom()));
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        sink->Close();
    };
    if (fill) {
        ComPtr<ID2D1PathGeometry> area;
        build(true, area);
        if (area)
            if (auto g = m_dcomp->CreateLinearGradientBrush(A(c, 0.28f), A(c, 0.0f), D2D1::Point2F(0, r.y), D2D1::Point2F(0, r.Bottom())))
                ctx->FillGeometry(area.Get(), g.Get());
    }
    ComPtr<ID2D1PathGeometry> line;
    build(false, line);
    if (!line) return;
    auto stroke = m_dcomp->CreateRoundedStrokeStyle();
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, 0.22f));
    ctx->DrawGeometry(line.Get(), m_brush.Get(), 4.0f, stroke.Get());
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, 1.0f));
    ctx->DrawGeometry(line.Get(), m_brush.Get(), 1.6f, stroke.Get());
}

void DesktopWidgetWindow::Render() {
    if (!m_dcomp || !m_brush || !m_shownNow) return;
    const float s = m_dpiScale;
    const core::Rect card{kPad, kPad, kCardW, kCardH[Slot(m_type)]};
    auto body = m_dcomp->CreateSquircleGeometry(card, kRadius);
    auto silhouette = render::RecordSilhouette(m_dcomp.get(), {body.Get()}, 1.0f, 0.8f);   // logical px; drawn under the DPI transform
    auto ctx = m_dcomp->BeginDraw();
    if (!ctx) return;
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(s, s));
    render::DrawGlassGlow(ctx, silhouette.Get(), 1.0f, 0.8f);
    if (body) render::DrawGlassBody(ctx, m_dcomp.get(), m_brush.Get(), body.Get(), card, 1.0f, 0.9f);

    ctx->SetTransform(D2D1::Matrix3x2F::Translation(kPad, kPad) * D2D1::Matrix3x2F::Scale(s, s));   // card coordinates
    switch (m_type) {
        case WidgetType::SystemMonitor: RenderSystemMonitor(ctx); break;
        case WidgetType::ClockCalendar: RenderClockCalendar(ctx); break;
        case WidgetType::NetworkMeter:  RenderNetworkMeter(ctx); break;
        case WidgetType::Storage:       RenderStorage(ctx); break;
        case WidgetType::Weather:       RenderWeather(ctx); break;
        case WidgetType::NowPlaying:    RenderNowPlaying(ctx); break;
    }

    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dcomp->EndDraw();
}

void DesktopWidgetWindow::RenderSystemMonitor(ID2D1DeviceContext* ctx) {
    const auto m = system::MetricsCollector::Instance().GetMetrics();
    DrawText(ctx, i18n::Pick(L"System Performance", L"Sistem Performansı"), m_fontTitle.Get(), D2D1::RectF(18.0f, 10.0f, 246.0f, 34.0f), kText);

    const float cy = 84.0f, rad = 30.0f, th = 4.5f;
    wchar_t buf[32];
    swprintf_s(buf, L"%d%%", static_cast<int>(m.cpuUsagePercent + 0.5f));
    DrawRing(ctx, 50.0f, cy, rad, th, m.cpuUsagePercent / 100.0f, buf, L"CPU");
    swprintf_s(buf, L"%.1f GB", static_cast<double>(m.ramUsedBytes) / 1073741824.0);
    DrawRing(ctx, 132.0f, cy, rad, th, m.ramUsagePercent / 100.0f, buf, L"RAM");
    swprintf_s(buf, L"%d%%", static_cast<int>(m.gpuUsagePercent + 0.5f));
    DrawRing(ctx, 214.0f, cy, rad, th, m.gpuUsagePercent / 100.0f, buf, L"GPU");
}

void DesktopWidgetWindow::RenderClockCalendar(ID2D1DeviceContext* ctx) {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t timeBuf[32];
    swprintf_s(timeBuf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    if (m_hasIconFont && m_fontIcon) {   // violet calendar glyph
        wchar_t g[2] = {0xE787, 0};
        const D2D1_RECT_F r = D2D1::RectF(14.0f, 26.0f, 50.0f, 66.0f);
        m_brush->SetColor(D2D1::ColorF(kViolet.r, kViolet.g, kViolet.b, 0.35f));
        const D2D1_RECT_F rg = D2D1::RectF(r.left, r.top + 1.0f, r.right, r.bottom + 1.0f);
        ctx->DrawText(g, 1, m_fontIcon.Get(), &rg, m_brush.Get());
        m_brush->SetColor(D2D1::ColorF(kVioletLight.r, kVioletLight.g, kVioletLight.b, 1.0f));
        ctx->DrawText(g, 1, m_fontIcon.Get(), &r, m_brush.Get());
    }
    DrawText(ctx, timeBuf, m_fontLarge.Get(), D2D1::RectF(62.0f, 14.0f, 256.0f, 50.0f), kText);
    DrawText(ctx, i18n::LongDate(st), m_fontSmall.Get(), D2D1::RectF(63.0f, 50.0f, 256.0f, 72.0f), core::Color{0.90f, 0.89f, 0.96f, 1.0f});
}

void DesktopWidgetWindow::RenderNetworkMeter(ID2D1DeviceContext* ctx) {
    const auto m = system::MetricsCollector::Instance().GetMetrics();
    DrawText(ctx, i18n::Pick(L"Network Activity", L"Ağ Aktivitesi"), m_fontTitle.Get(), D2D1::RectF(18.0f, 10.0f, 246.0f, 34.0f), kText);

    auto rate = [](uint64_t bps) {
        wchar_t b[32];
        swprintf_s(b, L"%.2f MB/s", static_cast<double>(bps) / 1048576.0);
        return std::wstring(b);
    };
    // "↓ 0.01 MB/s"  left,  "↑ 0.00 MB/s"  right
    DrawText(ctx, L"↓", m_fontMedium.Get(), D2D1::RectF(18.0f, 40.0f, 32.0f, 60.0f), kVioletLight);
    DrawText(ctx, rate(m.netDownloadBytesPerSec), m_fontMedium.Get(), D2D1::RectF(33.0f, 40.0f, 140.0f, 60.0f), kText);
    const std::wstring up = rate(m.netUploadBytesPerSec);
    float upW = 70.0f;
    if (auto dw = m_dcomp->GetDWriteFactory()) {
        ComPtr<IDWriteTextLayout> tl;
        if (m_fontMedium && SUCCEEDED(dw->CreateTextLayout(up.c_str(), static_cast<UINT32>(up.size()), m_fontMedium.Get(), 400, 30, &tl))) {
            DWRITE_TEXT_METRICS tm{}; tl->GetMetrics(&tm); upW = tm.widthIncludingTrailingWhitespace;
        }
    }
    const float upX = 246.0f - upW;
    DrawText(ctx, L"↑", m_fontMedium.Get(), D2D1::RectF(upX - 15.0f, 40.0f, upX, 60.0f), kVioletLight);
    DrawText(ctx, up, m_fontMedium.Get(), D2D1::RectF(upX, 40.0f, 250.0f, 60.0f), kText);

    float peak = 48.0f;   // KB/s; never zoom into noise
    for (float v : m.netDownHistory) peak = std::max(peak, v);
    for (float v : m.netUpHistory) peak = std::max(peak, v);
    const core::Rect spark{18.0f, 70.0f, 228.0f, 24.0f};
    DrawSpark(ctx, m.netUpHistory, peak, spark, A(kVioletLight, 0.45f), false);
    DrawSpark(ctx, m.netDownHistory, peak, spark, kViolet, true);
}

// ---- Storage: one bar per drive (like the mockup) ----------------------------------------------
void DesktopWidgetWindow::RenderStorage(ID2D1DeviceContext* ctx) {
    const ULONGLONG now = GetTickCount64();
    if (m_drives.empty() || now - m_drivesAt > 30000) {
        m_drivesAt = now; m_drives.clear();
        const DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26 && m_drives.size() < 4; ++i) {
            if (!(mask & (1u << i))) continue;
            wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
            if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
            ULARGE_INTEGER freeB{}, totB{}, totFree{};
            if (!GetDiskFreeSpaceExW(root, &freeB, &totB, &totFree) || !totB.QuadPart) continue;
            wchar_t vol[64] = {};
            GetVolumeInformationW(root, vol, 64, nullptr, nullptr, nullptr, nullptr, 0);
            Drive d; d.letter = root[0]; d.label = vol[0] ? vol : L"Disk";
            d.totalGB = static_cast<double>(totB.QuadPart) / 1073741824.0;
            d.usedGB = d.totalGB - static_cast<double>(freeB.QuadPart) / 1073741824.0;
            m_drives.push_back(d);
        }
    }
    DrawText(ctx, i18n::Pick(L"Storage", L"Depolama"), m_fontTitle.Get(), D2D1::RectF(18.0f, 10.0f, 246.0f, 34.0f), kText);
    const float rowH = 36.0f;
    for (size_t i = 0; i < m_drives.size(); ++i) {
        const auto& d = m_drives[i];
        const float y = 40.0f + i * rowH;
        wchar_t nm[96]; swprintf_s(nm, L"%ls (%c:)", d.label.c_str(), d.letter);
        auto sz = [](double gb) { wchar_t b[24]; if (gb >= 1000.0) swprintf_s(b, L"%.2f TB", gb / 1024.0); else swprintf_s(b, L"%.0f GB", gb); return std::wstring(b); };
        const std::wstring usage = sz(d.usedGB) + L" / " + sz(d.totalGB);
        DrawText(ctx, nm, m_fontSmall.Get(), D2D1::RectF(18.0f, y, 150.0f, y + 14.0f), kText);
        DrawText(ctx, usage, m_fontTiny.Get(), D2D1::RectF(130.0f, y, 246.0f, y + 14.0f), kMuted);
        const float frac = std::clamp(static_cast<float>(d.usedGB / d.totalGB), 0.0f, 1.0f);
        const D2D1_RECT_F track = D2D1::RectF(18.0f, y + 20.0f, 246.0f, y + 24.0f);
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.10f));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(track, 2.0f, 2.0f), m_brush.Get());
        const float fw = (track.right - track.left) * frac;
        if (auto g = m_dcomp->CreateLinearGradientBrush(kViolet, kVioletLight, D2D1::Point2F(track.left, 0), D2D1::Point2F(track.right, 0)))
            ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(track.left, track.top, track.left + std::max(fw, 4.0f), track.bottom), 2.0f, 2.0f), g.Get());
    }
}

// ---- Weather card: now + next hours ----------------------------------------------------------------
namespace {
void MiniWeatherIcon(ID2D1DeviceContext* c, ID2D1SolidColorBrush* b, float cx, float cy, float sz, bar::WeatherService::Kind k, float t) {
    using K = bar::WeatherService::Kind;
    const float u = sz * 0.5f;
    auto circ = [&](float x, float y, float r, core::Color col) { b->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a)); c->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), b); };
    auto cloud = [&](float x, float y, float kk, core::Color col) {
        circ(x - 0.55f * kk, y + 0.12f * kk, 0.42f * kk, col); circ(x - 0.05f * kk, y - 0.15f * kk, 0.55f * kk, col); circ(x + 0.52f * kk, y + 0.10f * kk, 0.44f * kk, col);
        b->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a));
        c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - 0.9f * kk, y + 0.10f * kk, x + 0.92f * kk, y + 0.55f * kk), 0.22f * kk, 0.22f * kk), b);
    };
    const core::Color sun{1.0f, 0.80f, 0.22f, 1}, white{0.94f, 0.96f, 1.0f, 1}, grey{0.68f, 0.72f, 0.84f, 1}, blue{0.45f, 0.72f, 1.0f, 1};
    auto sunAt = [&](float x, float y, float r) {
        circ(x, y, r, sun);
        b->SetColor(D2D1::ColorF(sun.r, sun.g, sun.b, 0.85f));
        for (int i = 0; i < 8; ++i) { const float a = i * 0.7853982f + t * 0.4f; c->DrawLine(D2D1::Point2F(x + std::cos(a) * r * 1.4f, y + std::sin(a) * r * 1.4f), D2D1::Point2F(x + std::cos(a) * r * 1.8f, y + std::sin(a) * r * 1.8f), b, sz * 0.07f); }
    };
    switch (k) {
        case K::Clear: sunAt(cx, cy, u * 0.55f); break;
        case K::PartlyCloudy: sunAt(cx - u * 0.28f, cy - u * 0.28f, u * 0.44f); cloud(cx + u * 0.12f, cy + u * 0.2f, u * 0.78f, white); break;
        case K::Cloudy: cloud(cx, cy + u * 0.05f, u * 0.95f, white); break;
        case K::Fog: cloud(cx, cy - u * 0.2f, u * 0.85f, grey); b->SetColor(D2D1::ColorF(grey.r, grey.g, grey.b, 0.9f)); c->DrawLine(D2D1::Point2F(cx - u * 0.8f, cy + u * 0.6f), D2D1::Point2F(cx + u * 0.8f, cy + u * 0.6f), b, sz * 0.08f); break;
        case K::Rain: cloud(cx, cy - u * 0.25f, u * 0.85f, grey); b->SetColor(D2D1::ColorF(blue.r, blue.g, blue.b, 1)); for (int i = -1; i <= 1; ++i) c->DrawLine(D2D1::Point2F(cx + i * u * 0.5f, cy + u * 0.4f), D2D1::Point2F(cx + i * u * 0.5f - u * 0.1f, cy + u * 0.7f), b, sz * 0.08f); break;
        case K::Snow: cloud(cx, cy - u * 0.25f, u * 0.85f, grey); for (int i = -1; i <= 1; ++i) circ(cx + i * u * 0.5f, cy + u * 0.55f, sz * 0.05f, white); break;
        case K::Storm: cloud(cx, cy - u * 0.25f, u * 0.85f, grey); b->SetColor(D2D1::ColorF(sun.r, sun.g, sun.b, 1)); c->DrawLine(D2D1::Point2F(cx + u * 0.1f, cy + u * 0.3f), D2D1::Point2F(cx - u * 0.15f, cy + u * 0.65f), b, sz * 0.09f); c->DrawLine(D2D1::Point2F(cx - u * 0.15f, cy + u * 0.65f), D2D1::Point2F(cx + u * 0.1f, cy + u * 0.65f), b, sz * 0.09f); break;
        case K::Night: circ(cx, cy, u * 0.55f, core::Color{0.88f, 0.90f, 1.0f, 1}); circ(cx + u * 0.3f, cy - u * 0.2f, u * 0.5f, core::Color{0.07f, 0.06f, 0.12f, 1}); break;
        case K::NightCloudy: circ(cx - u * 0.3f, cy - u * 0.3f, u * 0.38f, core::Color{0.88f, 0.90f, 1.0f, 1}); cloud(cx + u * 0.12f, cy + u * 0.2f, u * 0.78f, white); break;
    }
}
} // namespace

void DesktopWidgetWindow::RenderWeather(ID2D1DeviceContext* ctx) {
    const bar::WeatherNow w = bar::WeatherService::Instance().Get();
    if (!w.valid) { DrawText(ctx, i18n::Pick(L"Loading weather...", L"Hava durumu yükleniyor..."), m_fontTitle.Get(), D2D1::RectF(18.0f, 10.0f, 246.0f, 34.0f), kMuted); return; }
    const auto kind = bar::WeatherService::KindOf(w.code, w.day);
    MiniWeatherIcon(ctx, m_brush.Get(), 34.0f, 36.0f, 40.0f, kind, m_time);
    wchar_t tb[16]; swprintf_s(tb, L"%d°C", w.tempC);
    DrawText(ctx, tb, m_fontLarge.Get(), D2D1::RectF(66.0f, 12.0f, 180.0f, 44.0f), kText);
    DrawText(ctx, bar::WeatherService::Describe(w.code, w.day), m_fontSmall.Get(), D2D1::RectF(67.0f, 40.0f, 246.0f, 56.0f), core::Color{0.90f, 0.89f, 0.96f, 1.0f});
    DrawText(ctx, w.city, m_fontTiny.Get(), D2D1::RectF(67.0f, 55.0f, 180.0f, 68.0f), kMuted);
    wchar_t hl[32]; swprintf_s(hl, L"%d° / %d°", w.hiC, w.loC);
    DrawText(ctx, hl, m_fontSmall.Get(), D2D1::RectF(150.0f, 14.0f, 250.0f, 32.0f), kMuted);

    // temperature curve of the next hours
    const int n = std::min<int>(5, static_cast<int>(w.hours.size()));
    if (n >= 2) {
        std::vector<float> v;
        int lo = 99, hi = -99;
        for (int i = 0; i < n; ++i) { lo = std::min(lo, w.hours[i].tempC); hi = std::max(hi, w.hours[i].tempC); }
        for (int i = 0; i < n; ++i) v.push_back(static_cast<float>(w.hours[i].tempC - lo + 1));
        DrawSpark(ctx, v, static_cast<float>(hi - lo + 2), core::Rect{27.0f, 76.0f, 210.0f, 22.0f}, kViolet, true);
    }
    for (int i = 0; i < n; ++i) {
        const float cx = 27.0f + i * (210.0f / std::max(n - 1, 1));
        const auto& h = w.hours[i];
        wchar_t hb[12]; if (i == 0) wcscpy_s(hb, i18n::Pick(L"Now", L"Şimdi")); else swprintf_s(hb, L"%02d", h.hour);
        DrawText(ctx, hb, m_fontTiny.Get(), D2D1::RectF(cx - 22, 106.0f, cx + 22, 118.0f), kMuted);
        MiniWeatherIcon(ctx, m_brush.Get(), cx, 132.0f, 20.0f, bar::WeatherService::KindOf(h.code, h.day), m_time);
        wchar_t tb2[12]; swprintf_s(tb2, L"%d°", h.tempC);
        DrawText(ctx, tb2, m_fontTiny.Get(), D2D1::RectF(cx - 22, 148.0f, cx + 22, 162.0f), kText);
    }
}

// ---- Now playing -----------------------------------------------------------------------------------
void DesktopWidgetWindow::UpdateArt() {
    auto& ms = media::MediaService::Instance();
    uint64_t ver = m_artVersion;
    std::vector<uint8_t> bytes;
    if (!ms.FetchAlbumArt(ver, bytes)) return;
    m_artVersion = ver;
    m_art.Reset();
    if (bytes.empty()) return;
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (!px) return;
    if (auto ctx = m_dcomp->GetContext()) {
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        ctx->CreateBitmap(D2D1::SizeU(w, h), px, w * 4, props, &m_art);
    }
    stbi_image_free(px);
}

void DesktopWidgetWindow::RenderNowPlaying(ID2D1DeviceContext* ctx) {
    UpdateArt();
    const auto t = media::MediaService::Instance().GetTrackLite();
    // album art (rounded)
    const core::Rect art{14.0f, 14.0f, 56.0f, 56.0f};
    if (auto geo = m_dcomp->CreateSquircleGeometry(art, 12.0f)) {
        if (m_art) {
            D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE);
            ctx->PushLayer(&lp, nullptr);
            const D2D1_SIZE_F sz = m_art->GetSize();
            const float side = std::min(sz.width, sz.height);
            const D2D1_RECT_F src = D2D1::RectF((sz.width - side) / 2, (sz.height - side) / 2, (sz.width + side) / 2, (sz.height + side) / 2);
            const D2D1_RECT_F dst = D2D1::RectF(art.x, art.y, art.Right(), art.Bottom());
            ctx->DrawBitmap(m_art.Get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src, nullptr);
            ctx->PopLayer();
        } else {
            if (auto g = m_dcomp->CreateLinearGradientBrush(kVioletLight, kVioletDeep, D2D1::Point2F(art.x, art.y), D2D1::Point2F(art.Right(), art.Bottom()))) ctx->FillGeometry(geo.Get(), g.Get());
            if (m_hasIconFont && m_fontIcon) { wchar_t g2[2] = {0xE8D6, 0}; m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.9f)); const D2D1_RECT_F r = D2D1::RectF(art.x, art.y, art.Right(), art.Bottom()); ctx->DrawText(g2, 1, m_fontIcon.Get(), &r, m_brush.Get()); }
        }
    }
    const bool playing = t.status == media::PlaybackStatus::Playing;
    DrawText(ctx, t.hasMedia ? t.title : std::wstring(i18n::Pick(L"Nothing playing", L"Çalan bir şey yok")), m_fontTitle.Get(), D2D1::RectF(82.0f, 12.0f, 250.0f, 30.0f), kText);
    DrawText(ctx, t.hasMedia ? t.artist : std::wstring(), m_fontTiny.Get(), D2D1::RectF(82.0f, 29.0f, 250.0f, 43.0f), kMuted);
    // progress
    const float frac = t.durationSeconds > 0.5 ? std::clamp(static_cast<float>(t.positionSeconds / t.durationSeconds), 0.0f, 1.0f) : 0.0f;
    m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.12f));
    ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(82.0f, 50.0f, 250.0f, 53.0f), 1.5f, 1.5f), m_brush.Get());
    if (frac > 0.0f) {
        m_brush->SetColor(D2D1::ColorF(kVioletLight.r, kVioletLight.g, kVioletLight.b, 1.0f));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(82.0f, 50.0f, 82.0f + 168.0f * frac, 53.0f), 1.5f, 1.5f), m_brush.Get());
    }
    // transport buttons
    m_btnPrev = core::Rect{104.0f, 60.0f, 30.0f, 28.0f};
    m_btnPlay = core::Rect{142.0f, 58.0f, 32.0f, 32.0f};
    m_btnNext = core::Rect{182.0f, 60.0f, 30.0f, 28.0f};
    if (m_hasIconFont && m_fontIcon) {
        auto glyph = [&](wchar_t g, const core::Rect& r, const core::Color& c) {
            wchar_t b[2] = {g, 0}; m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
            const D2D1_RECT_F rr = D2D1::RectF(r.x, r.y, r.Right(), r.Bottom());
            ctx->DrawText(b, 1, m_fontIcon.Get(), &rr, m_brush.Get());
        };
        glyph(0xE892, m_btnPrev, core::Color{1, 1, 1, 0.85f});
        glyph(playing ? 0xE769 : 0xE768, m_btnPlay, kVioletLight);
        glyph(0xE893, m_btnNext, core::Color{1, 1, 1, 0.85f});
    }
}

LRESULT DesktopWidgetWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            if (m_type == WidgetType::NowPlaying) {
                const float px = GET_X_LPARAM(lParam) / m_dpiScale - kPad, py = GET_Y_LPARAM(lParam) / m_dpiScale - kPad;
                auto& ms = media::MediaService::Instance();
                if (m_btnPrev.Contains(px, py)) { ms.PreviousTrack(); return 0; }
                if (m_btnPlay.Contains(px, py)) { ms.PlayPause(); return 0; }
                if (m_btnNext.Contains(px, py)) { ms.NextTrack(); return 0; }
            }
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
                    case WidgetType::Storage:
                        mutableCfg.widgets.storageWidget.position = {static_cast<float>(newX), static_cast<float>(newY)};
                        break;
                    case WidgetType::Weather:
                        mutableCfg.widgets.weatherWidget.position = {static_cast<float>(newX), static_cast<float>(newY)};
                        break;
                    case WidgetType::NowPlaying:
                        mutableCfg.widgets.nowWidget.position = {static_cast<float>(newX), static_cast<float>(newY)};
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
    m_widgets.push_back(std::make_unique<DesktopWidgetWindow>(WidgetType::Storage));
    m_widgets.push_back(std::make_unique<DesktopWidgetWindow>(WidgetType::Weather));
    m_widgets.push_back(std::make_unique<DesktopWidgetWindow>(WidgetType::NowPlaying));
    bar::WeatherService::Instance().Start();

    for (auto& w : m_widgets) {
        w->Create();
    }
    AETHER_LOG_INFO("WidgetManager initialized with " << m_widgets.size() << " desktop widgets");
}

void WidgetManager::Shutdown() {
    bar::WeatherService::Instance().Stop();
    m_widgets.clear();
}

void WidgetManager::Update(float dt) {
    if (!m_visible) return;
    for (auto& w : m_widgets) {
        w->Update(dt);
    }
}

void WidgetManager::Render() {
    render::glass::SyncTheme();
    if (!m_visible) return;
    // The cards only change with the metrics (1 s) and the clock: 5 redraws per second are plenty and keep the GPU idle.
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 200) return;
    last = now;
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
