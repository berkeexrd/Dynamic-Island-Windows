#include "aether/widgets/widget_window.hpp"
#include "aether/config/config_manager.hpp"
#include "aether/system/taskbar_service.hpp"
#include "aether/core/log.hpp"
#include "aether/core/i18n.hpp"
#include "aether/render/glass.hpp"
#include "aether/render/context_menu.hpp"
#include "aether/media/media_service.hpp"
#include "aether/bar/weather.hpp"
#include "stb_image.h"
#include <shlwapi.h>
#include <windowsx.h>
#include <ctime>
#include <cmath>
#include <algorithm>
#include <atomic>
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
// The margin must be wider than the glow reaches (sigma 13-16 -> ~3 sigma), otherwise the halo is cut off at the window edge.
constexpr float kPad = 36.0f;
constexpr float kCardW = 264.0f;
constexpr float kCardH[6] = {138.0f, 112.0f, 92.0f, 150.0f, 200.0f, 96.0f};   // performance, network, clock, storage, weather, now playing
constexpr float kRadius = 20.0f;
// The clock has no card in the concept: big thin digits, date above, weather below.
constexpr float kClockW = 340.0f;
constexpr float kClockH = 186.0f;

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
float CardW(WidgetType t) { return t == WidgetType::ClockCalendar ? kClockW : kCardW; }
float CardH(WidgetType t) { return t == WidgetType::ClockCalendar ? kClockH : kCardH[Slot(t)]; }

// Widgets redraw once a second (that is how often their data changes). After a click on the player buttons, a theme
// change or a show/hide they redraw at 5 Hz for a moment so the UI reacts immediately.
std::atomic<ULONGLONG> g_fastUntil{0};
void RedrawSoon(ULONGLONG ms = 1500) { g_fastUntil = GetTickCount64() + ms; }
} // namespace

DesktopWidgetWindow::DesktopWidgetWindow(WidgetType type) : m_type(type) {
    m_width = static_cast<uint32_t>(CardW(type) + kPad * 2);
    m_height = static_cast<uint32_t>(CardH(type) + kPad * 2);
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
core::Color& kAccent2 = render::glass::kAccent2;
core::Color& kAccent2Light = render::glass::kAccent2Light;
core::Color& kText = render::glass::kText;      // theme tinted, see glass::SetTheme
core::Color& kMuted = render::glass::kMuted;

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

    const float sysScale = std::max(1.0f, static_cast<float>(GetDpiForSystem()) / 96.0f);
    // The concept was drawn on a 1672 px wide canvas. On a wider screen the same pixel sizes look smaller than designed,
    // so the widgets grow with the screen width (1.0x up to 1.5x; a 1920 px wide screen gets 1.15x).
    const float designScale = std::clamp(static_cast<float>(GetSystemMetrics(SM_CXSCREEN)) / (1672.0f * sysScale), 1.0f, 1.5f);
    m_dpiScale = sysScale * designScale;
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
        // like the mockup: performance, network, storage and the player stacked on the right edge,
        // weather bottom-left, the card-less clock left of the centre
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        static const WidgetType kStack[] = {WidgetType::SystemMonitor, WidgetType::NetworkMeter, WidgetType::Storage, WidgetType::NowPlaying};
        float cardTop = 100.0f;
        for (WidgetType o : kStack) { if (o == m_type) break; cardTop += CardH(o) + 14.0f; }
        posX = work.right - static_cast<int>((CardW(m_type) + kPad + 16.0f) * m_dpiScale);
        posY = work.top + static_cast<int>((cardTop - kPad) * m_dpiScale);
        if (m_type == WidgetType::Weather) {
            posX = work.left + static_cast<int>((20.0f - kPad) * m_dpiScale);
            posY = work.bottom - pxH - static_cast<int>((94.0f - kPad) * m_dpiScale);   // card bottom stays 94 px above the work area
        } else if (m_type == WidgetType::ClockCalendar) {
            posX = work.left + static_cast<int>((work.right - work.left) * 0.17f) - static_cast<int>(kPad * m_dpiScale);
            posY = work.top + static_cast<int>((work.bottom - work.top) * 0.22f) - static_cast<int>(kPad * m_dpiScale);
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
                // long titles end in an ellipsis instead of running out of the card
                ComPtr<IDWriteInlineObject> ell;
                if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(f.Get(), &ell))) {
                    const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                    f->SetTrimming(&trim, ell.Get());
                }
            }
        };
        make(m_fontLarge,  display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 27.0f, DWRITE_TEXT_ALIGNMENT_LEADING);   // clock
        make(m_fontMedium, display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, DWRITE_TEXT_ALIGNMENT_LEADING);   // network values
        make(m_fontSmall,  text,    DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f, DWRITE_TEXT_ALIGNMENT_LEADING);   // clock date
        make(m_fontTitle,  display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, DWRITE_TEXT_ALIGNMENT_LEADING);   // card title
        make(m_fontCenter, display, DWRITE_FONT_WEIGHT_BOLD,      13.5f, DWRITE_TEXT_ALIGNMENT_CENTER);    // ring value
        make(m_fontTiny,   text,    DWRITE_FONT_WEIGHT_SEMI_BOLD,  8.5f, DWRITE_TEXT_ALIGNMENT_CENTER);    // ring label
        make(m_fontIcon,   icons,   DWRITE_FONT_WEIGHT_NORMAL,    26.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
        make(m_fontClock,  display, DWRITE_FONT_WEIGHT_LIGHT,    112.0f, DWRITE_TEXT_ALIGNMENT_CENTER);    // card-less clock
        make(m_fontDate,   text,    DWRITE_FONT_WEIGHT_NORMAL,    19.0f, DWRITE_TEXT_ALIGNMENT_CENTER);    // date above the clock
        make(m_fontInfo,   text,    DWRITE_FONT_WEIGHT_NORMAL,    16.5f, DWRITE_TEXT_ALIGNMENT_LEADING);   // weather line below it
        make(m_fontTinyL,  text,    DWRITE_FONT_WEIGHT_NORMAL,     9.5f, DWRITE_TEXT_ALIGNMENT_LEADING);
        make(m_fontTinyR,  text,    DWRITE_FONT_WEIGHT_NORMAL,     9.5f, DWRITE_TEXT_ALIGNMENT_TRAILING);  // storage usage
    }

    // Place near desktop level (bottom of Z-order)
    SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    m_shownNow = false;          // Update() shows the card as soon as the config allows it (the master switch can be off at start)
    return true;
}

void DesktopWidgetWindow::Show(bool show) {
    if (m_hwnd) {
        // hiding is immediate; showing is left to Update(), which also honours the user's own on/off switches
        if (!show) ShowWindow(m_hwnd, SW_HIDE);
        m_shownNow = false;
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
    if (want != m_shownNow && m_hwnd) {
        m_shownNow = want;
        ShowWindow(m_hwnd, want ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (want) { m_appear = config::ConfigManager::Instance().Get().animation.reducedMotion ? 1.0f : 0.0f; m_delay = 0.30f + Slot(m_type) * 0.12f; }   // cards rise in one after another
    }
    if (m_shownNow && m_appear < 1.0f) {
        if (m_delay > 0.0f) m_delay -= dt;
        else m_appear = std::min(1.0f, m_appear + dt / 0.65f);
    }
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

    auto stroke = m_dcomp->CreateRoundedStrokeStyle();
    auto fac = m_dcomp->GetD2DFactory();
    if (fac && frac > 0.004f) {
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
                m_brush->SetColor(D2D1::ColorF(kAccent2.r, kAccent2.g, kAccent2.b, 0.09f));
                ctx->DrawGeometry(arcGeo.Get(), m_brush.Get(), thickness + i * 3.0f, stroke.Get());
            }
            // The concept's arc fades in from its start (dim) to a full, vivid colour at the head. Direct2D has no
            // conic gradient, so a linear one runs along the chord from the start to the head (at most half a turn).
            const float a0 = -1.5707963f, head = a0 + std::min(frac, 0.5f) * 6.2831853f;
            const D2D1_POINT_2F p0 = D2D1::Point2F(cx + std::cos(a0) * rr, cy + std::sin(a0) * rr);
            const D2D1_POINT_2F p1 = D2D1::Point2F(cx + std::cos(head) * rr, cy + std::sin(head) * rr);
            const float chord = std::hypot(p1.x - p0.x, p1.y - p0.y);
            ComPtr<ID2D1LinearGradientBrush> grad;
            if (chord > 6.0f)
                grad = m_dcomp->CreateLinearGradientBrush({{0.0f, A(kAccent2, 0.30f)},
                                                           {0.55f, A(kAccent2, 0.90f)},
                                                           {1.0f, core::Color::Lerp(kAccent2, kAccent2Light, 0.20f)}}, p0, p1);
            if (grad) {
                ctx->DrawGeometry(arcGeo.Get(), grad.Get(), thickness, stroke.Get());
            } else {   // very short arc: a solid colour reads better than a squeezed gradient
                m_brush->SetColor(D2D1::ColorF(kAccent2.r, kAccent2.g, kAccent2.b, 0.95f));
                ctx->DrawGeometry(arcGeo.Get(), m_brush.Get(), thickness, stroke.Get());
            }
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
    const bool cardless = m_type == WidgetType::ClockCalendar;   // the concept's clock floats on the wallpaper
    const core::Rect card{kPad, kPad, CardW(m_type), CardH(m_type)};
    ComPtr<ID2D1PathGeometry> body;
    ComPtr<ID2D1CommandList> silhouette;
    if (!cardless) {
        body = m_dcomp->CreateSquircleGeometry(card, kRadius);
        silhouette = render::RecordSilhouette(m_dcomp.get(), {body.Get()}, 1.0f, 0.8f);   // logical px; drawn under the DPI transform
    }
    auto ctx = m_dcomp->BeginDraw();
    if (!ctx) return;
    // entrance: fade in, rise ~20 px and grow from 94 % (ease-out cubic); identity once finished
    const bool intro = m_appear < 1.0f;
    const float e = 1.0f - std::pow(1.0f - std::clamp(m_appear, 0.0f, 1.0f), 3.0f);
    D2D1::Matrix3x2F anim = D2D1::Matrix3x2F::Identity();
    if (intro) {
        const float cx = kPad + CardW(m_type) * 0.5f, cy = kPad + CardH(m_type) * 0.5f, sc = 0.94f + 0.06f * e;
        anim = D2D1::Matrix3x2F::Translation(-cx, -cy) * D2D1::Matrix3x2F::Scale(sc, sc) * D2D1::Matrix3x2F::Translation(cx, cy + (1.0f - e) * 20.0f);
        ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), e, nullptr, D2D1_LAYER_OPTIONS1_NONE), nullptr);
    }
    ctx->SetTransform(anim * D2D1::Matrix3x2F::Scale(s, s));
    if (!cardless) {
        render::DrawGlassGlow(ctx, silhouette.Get(), 1.0f, 0.8f);
        if (body) render::DrawGlassBody(ctx, m_dcomp.get(), m_brush.Get(), body.Get(), card, 1.0f, 0.9f);
    }

    ctx->SetTransform(D2D1::Matrix3x2F::Translation(kPad, kPad) * anim * D2D1::Matrix3x2F::Scale(s, s));   // card coordinates
    switch (m_type) {
        case WidgetType::SystemMonitor: RenderSystemMonitor(ctx); break;
        case WidgetType::ClockCalendar: RenderClockCalendar(ctx); break;
        case WidgetType::NetworkMeter:  RenderNetworkMeter(ctx); break;
        case WidgetType::Storage:       RenderStorage(ctx); break;
        case WidgetType::Weather:       RenderWeather(ctx); break;
        case WidgetType::NowPlaying:    RenderNowPlaying(ctx); break;
    }

    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    if (intro) ctx->PopLayer();
    m_dcomp->EndDraw();
}

void DesktopWidgetWindow::RenderSystemMonitor(ID2D1DeviceContext* ctx) {
    const auto m = system::MetricsCollector::Instance().GetMetrics();
    DrawText(ctx, i18n::Pick(L"System Performance", L"Sistem Performansı"), m_fontTitle.Get(), D2D1::RectF(18.0f, 10.0f, 246.0f, 34.0f), kText);

    const float cy = 84.0f, rad = 30.0f, th = 5.0f;
    wchar_t buf[32];
    swprintf_s(buf, L"%d%%", static_cast<int>(m.cpuUsagePercent + 0.5f));
    DrawRing(ctx, 50.0f, cy, rad, th, m.cpuUsagePercent / 100.0f, buf, L"CPU");
    swprintf_s(buf, L"%.1f GB", static_cast<double>(m.ramUsedBytes) / 1073741824.0);
    DrawRing(ctx, 132.0f, cy, rad, th, m.ramUsagePercent / 100.0f, buf, L"RAM");
    swprintf_s(buf, L"%d%%", static_cast<int>(m.gpuUsagePercent + 0.5f));
    DrawRing(ctx, 214.0f, cy, rad, th, m.gpuUsagePercent / 100.0f, buf, L"GPU");
}

namespace {
void MiniWeatherIcon(ID2D1DeviceContext* c, ID2D1SolidColorBrush* b, float cx, float cy, float sz, bar::WeatherService::Kind k, float t);   // defined with the weather card below
}

// ---- Clock: no card, like the concept. Date on top (weekday in the accent colour), big thin digits with a
// light -> accent gradient, weather line below, faint corner brackets around it all. ------------------------------
void DesktopWidgetWindow::RenderClockCalendar(ID2D1DeviceContext* ctx) {
    constexpr float W = kClockW, H = kClockH;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t timeBuf[16];
    swprintf_s(timeBuf, L"%02d:%02d", st.wHour, st.wMinute);
    auto dw = m_dcomp->GetDWriteFactory();

    {   // corner brackets
        m_brush->SetColor(D2D1::ColorF(kAccent2.r, kAccent2.g, kAccent2.b, 0.32f));
        const float l = 14.0f, x0 = 6.0f, y0 = 6.0f, x1 = W - 6.0f, y1 = H - 6.0f, w = 1.2f;
        auto ln = [&](float ax, float ay, float bx, float by) { ctx->DrawLine(D2D1::Point2F(ax, ay), D2D1::Point2F(bx, by), m_brush.Get(), w); };
        ln(x0, y0 + l, x0, y0); ln(x0, y0, x0 + l, y0);
        ln(x1 - l, y0, x1, y0); ln(x1, y0, x1, y0 + l);
        ln(x0, y1 - l, x0, y1); ln(x0, y1, x0 + l, y1);
        ln(x1 - l, y1, x1, y1); ln(x1, y1, x1, y1 - l);
    }

    // date: "Cuma, 13 Kasım" - the weekday part tinted
    const std::wstring date = i18n::LongDate(st);
    if (dw && m_fontDate) {
        ComPtr<IDWriteTextLayout> tl;
        if (SUCCEEDED(dw->CreateTextLayout(date.c_str(), static_cast<UINT32>(date.size()), m_fontDate.Get(), W, 28.0f, &tl)) && tl) {
            const size_t comma = date.find(L',');
            auto accent = m_dcomp->CreateSolidBrush(kAccent2Light);
            if (accent && comma != std::wstring::npos) tl->SetDrawingEffect(accent.Get(), DWRITE_TEXT_RANGE{0, static_cast<UINT32>(comma + 1)});
            m_brush->SetColor(D2D1::ColorF(0.96f, 0.95f, 0.99f, 1.0f));
            ctx->DrawTextLayout(D2D1::Point2F(0.0f, 12.0f), tl.Get(), m_brush.Get());
        }
    }

    // digits: gradient fill (bottom-left light -> top-right accent, like the concept)
    const D2D1_RECT_F tr = D2D1::RectF(0.0f, 34.0f, W, 34.0f + 124.0f);
    if (auto g = m_dcomp->CreateLinearGradientBrush({{0.0f, core::Color{0.97f, 0.95f, 1.0f, 1.0f}}, {1.0f, kAccent2Light}},
                                                    D2D1::Point2F(W * 0.30f, tr.bottom), D2D1::Point2F(W * 0.72f, tr.top)))
        ctx->DrawText(timeBuf, static_cast<UINT32>(wcslen(timeBuf)), m_fontClock.Get(), &tr, g.Get());

    // weather line: [icon] 12°C  •  Parçalı Bulutlu, centred
    const bar::WeatherNow w = bar::WeatherService::Instance().Get();
    if (w.valid && dw && m_fontInfo) {
        wchar_t tb[16]; swprintf_s(tb, L"%d°C", w.tempC);
        const std::wstring info = std::wstring(tb) + L"  •  " + bar::WeatherService::Describe(w.code, w.day);
        ComPtr<IDWriteTextLayout> tl;
        if (SUCCEEDED(dw->CreateTextLayout(info.c_str(), static_cast<UINT32>(info.size()), m_fontInfo.Get(), 400.0f, 24.0f, &tl)) && tl) {
            DWRITE_TEXT_METRICS tm{}; tl->GetMetrics(&tm);
            const float iconW = 26.0f, total = iconW + 8.0f + tm.widthIncludingTrailingWhitespace;
            const float x0 = (W - total) * 0.5f, y0 = H - 34.0f;
            MiniWeatherIcon(ctx, m_brush.Get(), x0 + iconW * 0.5f, y0 + 12.0f, 24.0f, bar::WeatherService::KindOf(w.code, w.day), 0.0f);
            m_brush->SetColor(D2D1::ColorF(0.93f, 0.92f, 0.98f, 1.0f));
            ctx->DrawTextLayout(D2D1::Point2F(x0 + iconW + 8.0f, y0), tl.Get(), m_brush.Get());
        }
    }
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
    DrawText(ctx, L"↓", m_fontMedium.Get(), D2D1::RectF(18.0f, 40.0f, 32.0f, 60.0f), kAccent2Light);
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
    DrawText(ctx, L"↑", m_fontMedium.Get(), D2D1::RectF(upX - 15.0f, 40.0f, upX, 60.0f), kAccent2Light);
    DrawText(ctx, up, m_fontMedium.Get(), D2D1::RectF(upX, 40.0f, 250.0f, 60.0f), kText);

    float peak = 48.0f;   // KB/s; never zoom into noise
    for (float v : m.netDownHistory) peak = std::max(peak, v);
    for (float v : m.netUpHistory) peak = std::max(peak, v);
    const core::Rect spark{18.0f, 70.0f, 228.0f, 24.0f};
    DrawSpark(ctx, m.netUpHistory, peak, spark, A(kAccent2Light, 0.50f), false);
    DrawSpark(ctx, m.netDownHistory, peak, spark, kAccent2, true);
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
        // small drive glyph on the left (vector, no icon font needed): body, lighter top lip, accent LED
        {
            const float ix = 18.0f, iy = y + 8.0f, iw = 22.0f, ih = 11.0f;
            m_brush->SetColor(D2D1::ColorF(0.74f, 0.72f, 0.84f, 0.95f));
            ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ix, iy, ix + iw, iy + ih), 3.0f, 3.0f), m_brush.Get());
            m_brush->SetColor(D2D1::ColorF(0.95f, 0.94f, 1.0f, 0.95f));
            ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ix + 1.0f, iy + 0.5f, ix + iw - 1.0f, iy + 4.0f), 2.0f, 2.0f), m_brush.Get());
            m_brush->SetColor(D2D1::ColorF(kAccent2Light.r, kAccent2Light.g, kAccent2Light.b, 1.0f));
            ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ix + 4.5f, iy + 7.5f), 1.1f, 1.1f), m_brush.Get());
        }
        const float x0 = 48.0f, x1 = 246.0f;   // text and bar column
        DrawText(ctx, nm, m_fontSmall.Get(), D2D1::RectF(x0, y, 170.0f, y + 14.0f), kText);
        DrawText(ctx, usage, m_fontTinyR.Get(), D2D1::RectF(110.0f, y, x1, y + 14.0f), kMuted);
        const float frac = std::clamp(static_cast<float>(d.usedGB / d.totalGB), 0.0f, 1.0f);
        const D2D1_RECT_F track = D2D1::RectF(x0, y + 19.0f, x1, y + 25.0f);
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.09f));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(track, 3.0f, 3.0f), m_brush.Get());
        const float fw = std::max((track.right - track.left) * frac, 5.0f);
        const D2D1_RECT_F fill = D2D1::RectF(track.left, track.top, track.left + fw, track.bottom);
        // soft glow under the fill, then deep accent -> light accent across the fill itself (measured on the concept)
        m_brush->SetColor(D2D1::ColorF(kAccent2.r, kAccent2.g, kAccent2.b, 0.20f));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(fill.left - 1.0f, fill.top - 2.0f, fill.right + 1.0f, fill.bottom + 2.0f), 4.0f, 4.0f), m_brush.Get());
        if (auto g = m_dcomp->CreateLinearGradientBrush({{0.0f, core::Color::Lerp(kAccent2, kVioletDeep, 0.25f)}, {1.0f, core::Color::Lerp(kAccent2, kAccent2Light, 0.85f)}},
                                                        D2D1::Point2F(fill.left, 0), D2D1::Point2F(fill.right, 0)))
            ctx->FillRoundedRectangle(D2D1::RoundedRect(fill, 3.0f, 3.0f), g.Get());
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
    const core::Color kRain{0.45f, 0.72f, 1.0f, 1.0f};
    auto hline = [&](float y, float a) { m_brush->SetColor(D2D1::ColorF(kMuted.r, kMuted.g, kMuted.b, a)); ctx->DrawLine(D2D1::Point2F(18.0f, y), D2D1::Point2F(246.0f, y), m_brush.Get(), 1.0f); };

    // header: icon, big temperature, condition + city, hi/lo + feels like on the right
    MiniWeatherIcon(ctx, m_brush.Get(), 36.0f, 36.0f, 44.0f, kind, 0.0f);   // static: the cards redraw once a second, a spinning sun would stutter
    wchar_t tb[16]; swprintf_s(tb, L"%d°", w.tempC);
    DrawText(ctx, tb, m_fontLarge.Get(), D2D1::RectF(68.0f, 10.0f, 170.0f, 44.0f), kText);
    DrawText(ctx, bar::WeatherService::Describe(w.code, w.day), m_fontSmall.Get(), D2D1::RectF(69.0f, 42.0f, 250.0f, 58.0f), core::Color{0.90f, 0.89f, 0.96f, 1.0f});
    DrawText(ctx, w.city, m_fontTinyL.Get(), D2D1::RectF(69.0f, 58.0f, 170.0f, 71.0f), kMuted);
    wchar_t hl[40]; swprintf_s(hl, L"↑%d°  ↓%d°", w.hiC, w.loC);
    DrawText(ctx, hl, m_fontTinyR.Get(), D2D1::RectF(150.0f, 16.0f, 246.0f, 30.0f), kText);
    wchar_t fl[48]; swprintf_s(fl, i18n::Pick(L"Feels like %d°", L"Hissedilen %d°"), w.feelsC);
    DrawText(ctx, fl, m_fontTinyR.Get(), D2D1::RectF(130.0f, 32.0f, 246.0f, 46.0f), kMuted);

    // stat row: humidity / wind / UV or rain
    hline(78.0f, 0.18f);
    struct Stat { wchar_t v[24]; const wchar_t* l; };
    Stat st[3];
    swprintf_s(st[0].v, L"%%%d", static_cast<int>(std::lround(w.humidity))); st[0].l = i18n::Pick(L"Humidity", L"Nem");
    swprintf_s(st[1].v, L"%d km/h", static_cast<int>(std::lround(w.windKmh))); st[1].l = i18n::Pick(L"Wind", L"Rüzgar");
    if (w.precipMm > 0.05f) { swprintf_s(st[2].v, L"%.1f mm", static_cast<double>(w.precipMm)); st[2].l = i18n::Pick(L"Rain", L"Yağış"); }
    else { swprintf_s(st[2].v, L"%d", static_cast<int>(std::lround(w.uv))); st[2].l = L"UV"; }
    for (int i = 0; i < 3; ++i) {
        const float x0 = 18.0f + i * 76.0f;
        DrawText(ctx, st[i].v, m_fontSmall.Get(), D2D1::RectF(x0, 82.0f, x0 + 76.0f, 98.0f), kText);
        DrawText(ctx, st[i].l, m_fontTinyL.Get(), D2D1::RectF(x0, 97.0f, x0 + 76.0f, 110.0f), kMuted);
    }
    hline(116.0f, 0.18f);

    // next hours: temperature curve, then icon + temperature + rain chance per column
    const int n = std::min<int>(5, static_cast<int>(w.hours.size()));
    if (n >= 2) {
        std::vector<float> v;
        int lo = 99, hi = -99;
        for (int i = 0; i < n; ++i) { lo = std::min(lo, w.hours[i].tempC); hi = std::max(hi, w.hours[i].tempC); }
        for (int i = 0; i < n; ++i) v.push_back(static_cast<float>(w.hours[i].tempC - lo + 1));
        DrawSpark(ctx, v, static_cast<float>(hi - lo + 2), core::Rect{27.0f, 150.0f, 210.0f, 14.0f}, kAccent2, true);
    }
    for (int i = 0; i < n; ++i) {
        const float cx = 27.0f + i * (210.0f / std::max(n - 1, 1));
        const auto& h = w.hours[i];
        wchar_t hb[12]; if (i == 0) wcscpy_s(hb, i18n::Pick(L"Now", L"Şimdi")); else swprintf_s(hb, L"%02d:00", h.hour);
        DrawText(ctx, hb, m_fontTiny.Get(), D2D1::RectF(cx - 24, 121.0f, cx + 24, 133.0f), kMuted);
        MiniWeatherIcon(ctx, m_brush.Get(), cx, 144.0f, 18.0f, bar::WeatherService::KindOf(h.code, h.day), 0.0f);
        wchar_t tb2[12]; swprintf_s(tb2, L"%d°", h.tempC);
        DrawText(ctx, tb2, m_fontTiny.Get(), D2D1::RectF(cx - 24, 166.0f, cx + 24, 180.0f), kText);
        if (h.rainPct >= 20) { wchar_t rb[12]; swprintf_s(rb, L"%%%d", h.rainPct); DrawText(ctx, rb, m_fontTiny.Get(), D2D1::RectF(cx - 24, 180.0f, cx + 24, 193.0f), kRain); }
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
        const D2D1_RECT_F pf = D2D1::RectF(82.0f, 50.0f, 82.0f + std::max(168.0f * frac, 3.0f), 53.0f);
        if (auto g = m_dcomp->CreateLinearGradientBrush({{0.0f, kAccent2}, {1.0f, kAccent2Light}}, D2D1::Point2F(pf.left, 0), D2D1::Point2F(pf.right, 0)))
            ctx->FillRoundedRectangle(D2D1::RoundedRect(pf, 1.5f, 1.5f), g.Get());
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
        glyph(playing ? 0xE769 : 0xE768, m_btnPlay, kAccent2Light);
        glyph(0xE893, m_btnNext, core::Color{1, 1, 1, 0.85f});
    }
}

LRESULT DesktopWidgetWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            if (m_type == WidgetType::NowPlaying) {
                const float px = GET_X_LPARAM(lParam) / m_dpiScale - kPad, py = GET_Y_LPARAM(lParam) / m_dpiScale - kPad;
                auto& ms = media::MediaService::Instance();
                if (m_btnPrev.Contains(px, py)) { ms.PreviousTrack(); RedrawSoon(); return 0; }
                if (m_btnPlay.Contains(px, py)) { ms.PlayPause(); RedrawSoon(); return 0; }
                if (m_btnNext.Contains(px, py)) { ms.NextTrack(); RedrawSoon(); return 0; }
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

        case WM_RBUTTONUP: {
            if (m_isDragging) return 0;
            POINT pt; GetCursorPos(&pt);
            const int cmd = ui::ShowContextMenu({
                ui::MenuItem(1, L"Bu widget'ı gizle", 0xE711),
                ui::MenuItem(2, L"Tüm widget'ları gizle", 0xE8BB, true),
            }, pt, false);
            if (cmd == 1 || cmd == 2) {
                auto& cfg = config::ConfigManager::Instance().GetMutable();
                if (cmd == 2) cfg.widgets.enabled = false;
                else switch (m_type) {
                    case WidgetType::SystemMonitor: cfg.widgets.sysMonWidget.enabled = false; break;
                    case WidgetType::ClockCalendar: cfg.widgets.clockWidget.enabled = false; break;
                    case WidgetType::NetworkMeter:  cfg.widgets.netWidget.enabled = false; break;
                    case WidgetType::Storage:       cfg.widgets.storageWidget.enabled = false; break;
                    case WidgetType::Weather:       cfg.widgets.weatherWidget.enabled = false; break;
                    case WidgetType::NowPlaying:    cfg.widgets.nowWidget.enabled = false; break;
                }
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
    RedrawSoon(2500);   // smooth first seconds: the glow cache builds on the second frame
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
    const bool themeChanged = render::glass::SyncTheme();
    if (!m_visible) return;
    // The cards change with the metrics and the clock (once a second), so one redraw per second keeps the GPU idle.
    // A theme change / click / show runs at 5 Hz for a moment (the glow cache needs a second frame to build).
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (themeChanged) RedrawSoon();
    const bool animating = IsAnimating() || m_frameByFrame;
    const ULONGLONG interval = animating ? 0 : (now < g_fastUntil.load() ? 200 : 1000);
    if (now - last < interval) return;
    last = now;
    for (auto& w : m_widgets) {
        w->Render();
    }
}

void WidgetManager::SetVisible(bool visible) {
    m_visible = visible;
    if (visible) RedrawSoon();
    for (auto& w : m_widgets) {
        w->Show(visible);
    }
}

} // namespace aether::widgets
