#include "aether/bar/panels.hpp"
#include "aether/bar/notifications.hpp"
#include "aether/bar/wifi.hpp"
#include "aether/bar/netstate.hpp"
#include "aether/bar/ui_common.hpp"
#include "aether/core/i18n.hpp"
#include "aether/render/glass.hpp"
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cwctype>

namespace aether::bar {

using core::Color;
using core::Rect;
using core::Vec2;
namespace gl = ui;
namespace i18n = aether::i18n;

namespace {
inline D2D1_RECT_F RF(const Rect& r) { return D2D1::RectF(r.Left(), r.Top(), r.Right(), r.Bottom()); }
inline Color W(float a) { return Color{1.0f, 1.0f, 1.0f, a}; }
inline float Frac(float v) { return v - std::floor(v); }
inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline Color Mix(const Color& a, const Color& b, float t) { return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t}; }
constexpr wchar_t kBell = 0xEA8F;
}

// =============================================================================================
//  Weather
// =============================================================================================
void WeatherPanel::OnShow() { WeatherService::Instance().RefreshNow(); }

void WeatherPanel::OnClick(int id, const Vec2&) {
    if (id == 1) WeatherService::Instance().RefreshNow();
    else if (id == 2) { ShellExecuteW(nullptr, L"open", L"msnweather:", nullptr, nullptr, SW_SHOWNORMAL); Hide(); }
}

void WeatherPanel::EnsureFonts() {
    if (m_fHuge && m_fontScale == S()) return;
    auto dw = m_dcomp->GetDWriteFactory();
    if (!dw) return;
    m_fHuge.Reset();
    dw->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 64.0f * S(), L"en-us", &m_fHuge);
    if (m_fHuge) { m_fHuge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); m_fHuge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); m_fHuge->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    m_fontScale = S();
}

void WeatherPanel::Moon(ID2D1DeviceContext* c, float cx, float cy, float r, const Color& col) {
    auto f = m_dcomp->GetD2DFactory();
    ComPtr<ID2D1EllipseGeometry> a, b;
    f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), &a);
    f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx + r * 0.50f, cy - r * 0.28f), r * 0.86f, r * 0.86f), &b);
    ComPtr<ID2D1PathGeometry> pg; f->CreatePathGeometry(&pg);
    if (!a || !b || !pg) return;
    ComPtr<ID2D1GeometrySink> sk;
    if (FAILED(pg->Open(&sk))) return;
    a->CombineWithGeometry(b.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, 0.2f, sk.Get());
    sk->Close();
    m_brush->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a));
    c->FillGeometry(pg.Get(), m_brush.Get());
}

// Small vector weather icon (kind = WeatherService::Kind), gently animated.
void WeatherPanel::Icon(ID2D1DeviceContext* c, float cx, float cy, float size, int kind) {
    using K = WeatherService::Kind;
    const float u = size * 0.5f, t = m_time;
    auto circle = [&](float x, float y, float r, const Color& col) { m_brush->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a)); c->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), m_brush.Get()); };
    auto cloud = [&](float x, float y, float k, const Color& col) {
        circle(x - 0.55f * k, y + 0.12f * k, 0.42f * k, col); circle(x - 0.05f * k, y - 0.15f * k, 0.55f * k, col); circle(x + 0.52f * k, y + 0.10f * k, 0.44f * k, col);
        m_brush->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a));
        c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - 0.9f * k, y + 0.10f * k, x + 0.92f * k, y + 0.55f * k), 0.22f * k, 0.22f * k), m_brush.Get());
    };
    const Color sun{1.0f, 0.80f, 0.22f, 1.0f}, white{0.94f, 0.96f, 1.0f, 1.0f}, grey{0.66f, 0.70f, 0.80f, 1.0f}, blue{0.45f, 0.72f, 1.0f, 1.0f};
    auto sunAt = [&](float x, float y, float r) {
        circle(x, y, r, sun);
        m_brush->SetColor(D2D1::ColorF(sun.r, sun.g, sun.b, 0.9f));
        for (int i = 0; i < 8; ++i) { const float a = i * 0.7853982f + t * 0.4f; c->DrawLine(D2D1::Point2F(x + std::cos(a) * r * 1.4f, y + std::sin(a) * r * 1.4f), D2D1::Point2F(x + std::cos(a) * r * 1.8f, y + std::sin(a) * r * 1.8f), m_brush.Get(), size * 0.07f, nullptr); }
    };
    const float bob = std::sin(t * 1.4f) * size * 0.02f;
    switch (static_cast<K>(kind)) {
        case K::Clear: sunAt(cx, cy, u * 0.55f); break;
        case K::PartlyCloudy: sunAt(cx - u * 0.28f, cy - u * 0.28f, u * 0.44f); cloud(cx + u * 0.12f, cy + u * 0.2f + bob, u * 0.78f, white); break;
        case K::Cloudy: cloud(cx, cy + u * 0.05f + bob, u * 0.95f, white); break;
        case K::Fog:
            cloud(cx, cy - u * 0.2f, u * 0.85f, grey); m_brush->SetColor(D2D1::ColorF(grey.r, grey.g, grey.b, 0.9f));
            c->DrawLine(D2D1::Point2F(cx - u * 0.8f, cy + u * 0.6f), D2D1::Point2F(cx + u * 0.8f, cy + u * 0.6f), m_brush.Get(), size * 0.08f, nullptr); break;
        case K::Rain:
            cloud(cx, cy - u * 0.25f, u * 0.85f, grey); m_brush->SetColor(D2D1::ColorF(blue.r, blue.g, blue.b, 1.0f));
            for (int i = -1; i <= 1; ++i) { const float ph = Frac(t * 1.2f + i * 0.33f); c->DrawLine(D2D1::Point2F(cx + i * u * 0.5f - ph * u * 0.1f, cy + u * (0.4f + ph * 0.3f)), D2D1::Point2F(cx + i * u * 0.5f - ph * u * 0.1f - u * 0.1f, cy + u * (0.62f + ph * 0.3f)), m_brush.Get(), size * 0.08f, nullptr); }
            break;
        case K::Snow:
            cloud(cx, cy - u * 0.25f, u * 0.85f, grey);
            for (int i = -1; i <= 1; ++i) { const float ph = Frac(t * 0.6f + i * 0.31f); circle(cx + i * u * 0.5f + std::sin(t * 2 + i) * u * 0.08f, cy + u * (0.45f + ph * 0.5f), size * 0.05f, white); }
            break;
        case K::Storm: {
            cloud(cx, cy - u * 0.25f, u * 0.85f, grey);
            const float fl = Frac(t * 0.8f) < 0.2f ? 1.0f : 0.75f;
            m_brush->SetColor(D2D1::ColorF(sun.r, sun.g, sun.b, fl));
            c->DrawLine(D2D1::Point2F(cx + u * 0.1f, cy + u * 0.3f), D2D1::Point2F(cx - u * 0.15f, cy + u * 0.62f), m_brush.Get(), size * 0.09f, nullptr);
            c->DrawLine(D2D1::Point2F(cx - u * 0.15f, cy + u * 0.62f), D2D1::Point2F(cx + u * 0.1f, cy + u * 0.62f), m_brush.Get(), size * 0.09f, nullptr);
            c->DrawLine(D2D1::Point2F(cx + u * 0.1f, cy + u * 0.62f), D2D1::Point2F(cx - u * 0.1f, cy + u * 0.95f), m_brush.Get(), size * 0.09f, nullptr);
            break; }
        case K::Night: Moon(c, cx, cy, u * 0.62f, Color{0.88f, 0.90f, 1.0f, 1.0f}); break;
        case K::NightCloudy: Moon(c, cx - u * 0.3f, cy - u * 0.3f, u * 0.42f, Color{0.88f, 0.90f, 1.0f, 1.0f}); cloud(cx + u * 0.12f, cy + u * 0.2f + bob, u * 0.78f, white); break;
    }
}

// The animated sky of the hero card.
void WeatherPanel::Scene(ID2D1DeviceContext* c, const Rect& r, const WeatherNow& w) {
    using K = WeatherService::Kind;
    const float s = S(), t = m_time;
    const K k = WeatherService::KindOf(w.code, w.day);
    const bool night = k == K::Night || k == K::NightCloudy || !w.day;
    Color top, bot;
    switch (k) {
        case K::Clear:        top = {0.16f, 0.42f, 0.92f, 1}; bot = {0.46f, 0.74f, 0.99f, 1}; break;
        case K::PartlyCloudy: top = {0.26f, 0.48f, 0.86f, 1}; bot = {0.56f, 0.76f, 0.96f, 1}; break;
        case K::Cloudy:       top = {0.30f, 0.35f, 0.44f, 1}; bot = {0.52f, 0.57f, 0.66f, 1}; break;
        case K::Fog:          top = {0.42f, 0.46f, 0.52f, 1}; bot = {0.64f, 0.67f, 0.72f, 1}; break;
        case K::Rain:         top = {0.17f, 0.21f, 0.30f, 1}; bot = {0.34f, 0.40f, 0.50f, 1}; break;
        case K::Snow:         top = {0.42f, 0.50f, 0.64f, 1}; bot = {0.78f, 0.84f, 0.93f, 1}; break;
        case K::Storm:        top = {0.09f, 0.09f, 0.17f, 1}; bot = {0.26f, 0.22f, 0.38f, 1}; break;
        default:              top = {0.04f, 0.05f, 0.16f, 1}; bot = {0.17f, 0.12f, 0.38f, 1}; break;
    }
    if (night && k != K::Night && k != K::NightCloudy) { top = Mix(top, Color{0.02f, 0.03f, 0.10f, 1}, 0.55f); bot = Mix(bot, Color{0.08f, 0.07f, 0.20f, 1}, 0.55f); }
    else if (k == K::NightCloudy) { top = {0.07f, 0.08f, 0.20f, 1}; bot = {0.20f, 0.18f, 0.38f, 1}; }

    auto geo = m_dcomp->CreateSquircleGeometry(r, 26 * s);
    if (!geo) return;
    if (auto g = m_dcomp->CreateLinearGradientBrush(top, bot, D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x + r.width * 0.3f, r.Bottom()))) c->FillGeometry(geo.Get(), g.Get());
    D2D1_LAYER_PARAMETERS1 clip = D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE);
    c->PushLayer(&clip, nullptr);

    auto circle = [&](float x, float y, float rad, const Color& col) { m_brush->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a)); c->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), rad, rad), m_brush.Get()); };
    auto line = [&](float x1, float y1, float x2, float y2, const Color& col, float wd) { m_brush->SetColor(D2D1::ColorF(col.r, col.g, col.b, col.a)); c->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), m_brush.Get(), wd, nullptr); };
    auto glow = [&](float x, float y, float rad, const Color& col) { if (auto g = m_dcomp->CreateRadialGradientBrush(col, Color{col.r, col.g, col.b, 0.0f}, D2D1::Point2F(x, y), rad, rad)) c->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), rad, rad), g.Get()); };

    // stars
    if (night) for (int i = 0; i < 44; ++i) {
        const float x = r.x + Frac(i * 0.7548776f) * r.width, y = r.y + Frac(i * 0.5698403f) * r.height * 0.75f;
        const float tw = 0.5f + 0.5f * std::sin(t * (0.8f + (i % 5) * 0.35f) + i * 1.7f);
        circle(x, y, (0.6f + (i % 3) * 0.45f) * s, W(0.20f + 0.70f * tw));
    }
    // sun / moon
    const float sx = r.Right() - 82 * s, sy = r.y + 70 * s;
    if (!night && (k == K::Clear || k == K::PartlyCloudy)) {
        const float R = (k == K::Clear ? 30.0f : 24.0f) * s, px = k == K::Clear ? sx : sx - 24 * s;
        glow(px, sy, R * 4.6f, Color{1.0f, 0.86f, 0.45f, 0.50f + 0.08f * std::sin(t * 1.3f)});
        for (int i = 0; i < 14; ++i) {
            const float a = i * 0.4487989f + t * 0.22f, l2 = R * (1.75f + 0.28f * std::sin(t * 2.0f + i * 1.3f));
            line(px + std::cos(a) * R * 1.32f, sy + std::sin(a) * R * 1.32f, px + std::cos(a) * l2, sy + std::sin(a) * l2, Color{1.0f, 0.93f, 0.62f, 0.55f}, 2.2f * s);
        }
        if (auto g = m_dcomp->CreateRadialGradientBrush(Color{1.0f, 0.98f, 0.80f, 1.0f}, Color{1.0f, 0.78f, 0.25f, 1.0f}, D2D1::Point2F(px - R * 0.2f, sy - R * 0.2f), R * 1.2f, R * 1.2f))
            c->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, sy), R, R), g.Get());
    } else if (night && (k == K::Night || k == K::NightCloudy || w.code <= 2)) {
        glow(sx, sy, 100 * s, Color{0.62f, 0.70f, 1.0f, 0.32f});
        Moon(c, sx, sy, 26 * s, Color{0.93f, 0.95f, 1.0f, 1.0f});
    }
    // clouds (drifting, parallax)
    const bool heavy = k == K::Cloudy || k == K::Rain || k == K::Storm || k == K::Snow || k == K::NightCloudy || k == K::Fog;
    const int nClouds = heavy ? 5 : (k == K::PartlyCloudy ? 3 : 0);
    Color cc = W(1.0f);
    if (k == K::Rain) cc = Color{0.60f, 0.66f, 0.76f, 1}; else if (k == K::Storm) cc = Color{0.40f, 0.38f, 0.52f, 1}; else if (k == K::Cloudy) cc = Color{0.78f, 0.82f, 0.90f, 1};
    else if (k == K::Snow) cc = Color{0.92f, 0.95f, 1.0f, 1}; else if (night) cc = Color{0.46f, 0.50f, 0.68f, 1};
    for (int i = 0; i < nClouds; ++i) {
        const float u = (40 + (i % 3) * 16) * s, span = r.width + u * 4, sp = (5.0f + i * 3.2f) * s;
        const float x = r.x - u * 2 + std::fmod(i * 131.0f * s + t * sp, span), y = r.y + (24 + i * 30 + (i % 2) * 10) * s;
        const float alpha = heavy ? (0.20f + 0.07f * i) : 0.88f;
        D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), alpha, nullptr, D2D1_LAYER_OPTIONS1_NONE);
        c->PushLayer(&lp, nullptr);
        circle(x - 0.55f * u, y + 0.12f * u, 0.42f * u, cc); circle(x - 0.05f * u, y - 0.15f * u, 0.55f * u, cc); circle(x + 0.52f * u, y + 0.10f * u, 0.44f * u, cc);
        m_brush->SetColor(D2D1::ColorF(cc.r, cc.g, cc.b, 1.0f));
        c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - 0.9f * u, y + 0.10f * u, x + 0.92f * u, y + 0.55f * u), 0.22f * u, 0.22f * u), m_brush.Get());
        c->PopLayer();
    }
    // fog bands
    if (k == K::Fog) for (int i = 0; i < 4; ++i) {
        const float off = std::sin(t * 0.22f + i * 1.4f) * 46 * s;
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.14f + 0.03f * (i % 2)));
        c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x + 20 * s + off, r.y + (60 + i * 38) * s, r.Right() - 20 * s + off, r.y + (84 + i * 38) * s), 12 * s, 12 * s), m_brush.Get());
    }
    // rain
    if (k == K::Rain || k == K::Storm) {
        const bool strong = k == K::Storm || w.code == 65 || w.code == 82 || w.code == 67;
        const int n = strong ? 96 : 60;
        for (int i = 0; i < n; ++i) {
            const float speed = (430 + (i % 7) * 70) * s, len = (11 + (i % 4) * 4) * s, x0 = r.x + Frac(i * 0.61803f) * (r.width + 50 * s) - 10 * s;
            const float y = r.y + std::fmod(t * speed + i * 53.3f * s, r.height + len * 2) - len;
            const float x = x0 - (y - r.y) * 0.16f;
            line(x, y, x - len * 0.16f, y + len, Color{0.78f, 0.88f, 1.0f, 0.50f}, 1.3f * s);
            if (i % 8 == 0) { const float ph = Frac(t * 0.9f + i * 0.137f); m_brush->SetColor(D2D1::ColorF(0.8f, 0.9f, 1.0f, (1 - ph) * 0.30f)); c->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x0 - r.height * 0.16f, r.Bottom() - 8 * s), ph * 12 * s, ph * 3.5f * s), m_brush.Get(), 1.0f * s); }
        }
    }
    // snow
    if (k == K::Snow) for (int i = 0; i < 64; ++i) {
        const float x = r.x + Frac(i * 0.61803f) * r.width + std::sin(t * 0.8f + i) * 12 * s;
        const float y = r.y + std::fmod(t * (22 + (i % 5) * 9) * s + i * 47 * s, r.height + 10 * s) - 5 * s;
        circle(x, y, (1.2f + (i % 3) * 0.9f) * s, W(0.55f + 0.35f * ((i % 4) / 3.0f)));
    }
    // lightning
    if (k == K::Storm) {
        const float period = 6.5f, cyc = std::fmod(t, period), idx = std::floor(t / period);
        if (cyc < 0.38f) {
            const float fl = (1 - cyc / 0.38f) * (0.65f + 0.35f * std::sin(cyc * 60.0f));
            m_brush->SetColor(D2D1::ColorF(0.85f, 0.88f, 1.0f, 0.30f * std::max(fl, 0.0f)));
            c->FillRectangle(RF(r), m_brush.Get());
            float x = r.x + (0.22f + 0.5f * Frac(idx * 0.618f)) * r.width, y = r.y + 14 * s;
            for (int sgi = 0; sgi < 6; ++sgi) {
                const float dx = ((sgi % 2 ? 1 : -1) * (9 + 5 * Frac(idx + sgi * 0.37f))) * s, dy = r.height * 0.78f / 6;
                for (int pass = 0; pass < 3; ++pass) {
                    const float wd[] = {9.0f, 3.2f, 1.3f}, al[] = {0.20f, 0.75f, 1.0f};
                    line(x, y, x + dx, y + dy, pass == 2 ? Color{1, 1, 1, al[2] * fl} : Color{1.0f, 0.92f, 0.55f, al[pass] * fl}, wd[pass] * s);
                }
                x += dx; y += dy;
            }
        }
    }
    // soft ground gradient so the text stays readable
    if (auto g = m_dcomp->CreateLinearGradientBrush(Color{0, 0, 0, 0.0f}, Color{0, 0, 0, 0.34f}, D2D1::Point2F(r.x, r.y + r.height * 0.45f), D2D1::Point2F(r.x, r.Bottom()))) c->FillRectangle(RF(r), g.Get());
    c->PopLayer();
    if (auto rim = m_dcomp->CreateLinearGradientBrush(W(0.38f), W(0.06f), D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.Right(), r.Bottom()))) c->DrawGeometry(geo.Get(), rim.Get(), 1.1f * s);
}

void WeatherPanel::OnDraw(ID2D1DeviceContext* c, const Rect& P) {
    EnsureFonts();
    const float s = S();
    const WeatherNow w = WeatherService::Instance().Get();
    const Color acc = render::glass::kVioletLight;
    const float x = P.x + 14 * s, wid = P.width - 28 * s;
    Rect hero{x, P.y + 14 * s, wid, 214 * s};

    if (!w.valid) {
        Fill(c, hero, 26 * s, W(0.06f));
        const float cx = hero.Center().x, cy = hero.Center().y - 10 * s;
        for (int i = 0; i < 10; ++i) {
            const float a = m_time * 5.0f - i * 0.5f;
            m_brush->SetColor(D2D1::ColorF(acc.r, acc.g, acc.b, 1.0f - i * 0.09f));
            c->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + std::cos(a) * 20 * s, cy + std::sin(a) * 20 * s), (3.2f - i * 0.2f) * s, (3.2f - i * 0.2f) * s), m_brush.Get());
        }
        Text(c, i18n::Pick(L"Loading weather...", L"Hava durumu yükleniyor..."), m_fBody.Get(), Rect{hero.x, cy + 34 * s, hero.width, 28 * s}, W(0.6f));
        Rect retry{hero.Center().x - 60 * s, cy + 70 * s, 120 * s, 34 * s};
        Fill(c, retry, 17 * s, retry.Contains(m_mouse) ? W(0.14f) : W(0.08f)); AddHit(retry, 1);
        Text(c, i18n::Pick(L"Retry", L"Tekrar dene"), m_fCenter.Get(), retry, W(0.85f));
        return;
    }

    using K = WeatherService::Kind;
    Scene(c, hero, w);
    // hero text (soft shadow first)
    auto shadowText = [&](const std::wstring& t, IDWriteTextFormat* f, const Rect& rr, const Color& col) { Text(c, t, f, Rect{rr.x + 1 * s, rr.y + 1.5f * s, rr.width, rr.height}, Color{0, 0, 0, 0.28f}); Text(c, t, f, rr, col); };
    shadowText(w.city.empty() ? std::wstring(i18n::Pick(L"Your location", L"Konumun")) : w.city, m_fTitle.Get(), Rect{hero.x + 22 * s, hero.y + 14 * s, 220 * s, 28 * s}, W(0.96f));
    wchar_t tb[16]; swprintf_s(tb, L"%d°", w.tempC);
    shadowText(tb, m_fHuge.Get(), Rect{hero.x + 20 * s, hero.y + 46 * s, 200 * s, 84 * s}, W(1.0f));
    shadowText(WeatherService::Describe(w.code, w.day), m_fTitle.Get(), Rect{hero.x + 22 * s, hero.y + 132 * s, 260 * s, 28 * s}, W(0.95f));
    wchar_t hl[48]; swprintf_s(hl, L"%ls %d°   %ls %d°", i18n::Pick(L"High", L"Y"), w.hiC, i18n::Pick(L"Low", L"D"), w.loC);
    shadowText(hl, m_fBody.Get(), Rect{hero.x + 22 * s, hero.y + 160 * s, 260 * s, 26 * s}, W(0.85f));
    // refresh
    Rect rf{hero.Right() - 46 * s, hero.y + 12 * s, 34 * s, 34 * s};
    Fill(c, rf, 17 * s, rf.Contains(m_mouse) ? W(0.26f) : W(0.12f)); AddHit(rf, 1);
    Glyph(c, gl::kGlyphRefresh, rf, W(0.92f));
    (void)K::Clear;

    // detail cards
    const float gy = hero.Bottom() + 10 * s, cw = (wid - 16 * s) / 3, ch = 58 * s;
    struct Cell { const wchar_t* label; std::wstring value; };
    wchar_t b1[24], b2[24], b3[24], b4[24], b5[24], b6[40];
    swprintf_s(b1, L"%d°", w.feelsC); swprintf_s(b2, L"%d%%", w.humidity); swprintf_s(b3, L"%d km/h", w.windKmh); swprintf_s(b4, L"%d hPa", w.pressure); swprintf_s(b5, L"%d", w.uv);
    swprintf_s(b6, L"%ls / %ls", w.sunrise.empty() ? L"--" : w.sunrise.c_str(), w.sunset.empty() ? L"--" : w.sunset.c_str());
    const Cell cells[6] = {{i18n::Pick(L"Feels like", L"Hissedilen"), b1}, {i18n::Pick(L"Humidity", L"Nem"), b2}, {i18n::Pick(L"Wind", L"Rüzgar"), b3},
                           {i18n::Pick(L"Pressure", L"Basınç"), b4}, {i18n::Pick(L"UV index", L"UV indeksi"), b5}, {i18n::Pick(L"Sunrise / sunset", L"Doğuş / batış"), b6}};
    for (int i = 0; i < 6; ++i) {
        Rect r{x + (i % 3) * (cw + 8 * s), gy + (i / 3) * (ch + 8 * s), cw, ch};
        Fill(c, r, 16 * s, W(0.06f)); Stroke(c, r, 16 * s, W(0.07f));
        Text(c, cells[i].label, m_fSmall.Get(), Rect{r.x + 12 * s, r.y + 6 * s, r.width - 16 * s, 22 * s}, W(0.48f));
        Text(c, cells[i].value, i == 5 ? m_fBody.Get() : m_fTitle.Get(), Rect{r.x + 12 * s, r.y + 27 * s, r.width - 16 * s, 26 * s}, W(0.96f));
        if (i == 2) {   // wind arrow
            const float bear = (w.windDeg + 180.0f) * 0.0174533f, ax = r.Right() - 22 * s, ay = r.y + 17 * s, dx = std::sin(bear) * 9 * s, dy = -std::cos(bear) * 9 * s;
            m_brush->SetColor(D2D1::ColorF(acc.r, acc.g, acc.b, 0.95f));
            c->DrawLine(D2D1::Point2F(ax - dx, ay - dy), D2D1::Point2F(ax + dx, ay + dy), m_brush.Get(), 1.8f * s, nullptr);
            const float h1 = bear + 2.5f, h2 = bear - 2.5f;
            c->DrawLine(D2D1::Point2F(ax + dx, ay + dy), D2D1::Point2F(ax + dx + std::sin(h1) * 5 * s, ay + dy - std::cos(h1) * 5 * s), m_brush.Get(), 1.8f * s, nullptr);
            c->DrawLine(D2D1::Point2F(ax + dx, ay + dy), D2D1::Point2F(ax + dx + std::sin(h2) * 5 * s, ay + dy - std::cos(h2) * 5 * s), m_brush.Get(), 1.8f * s, nullptr);
        }
    }

    // hourly
    Rect hc{x, gy + 2 * (ch + 8 * s) + 2 * s, wid, 104 * s};
    Fill(c, hc, 18 * s, W(0.06f)); Stroke(c, hc, 18 * s, W(0.07f));
    const int cols = 8; const float colW = (hc.width - 16 * s) / cols;
    for (int i = 0; i < cols && i < static_cast<int>(w.hours.size()); ++i) {
        const auto& h = w.hours[i];
        const float cx = hc.x + 8 * s + colW * (i + 0.5f);
        wchar_t hb[8]; swprintf_s(hb, L"%02d", h.hour);
        Text(c, i == 0 ? std::wstring(i18n::Pick(L"Now", L"Şimdi")) : std::wstring(hb), m_fSmall.Get(), Rect{cx - colW / 2, hc.y + 6 * s, colW, 20 * s}, W(i == 0 ? 0.95f : 0.55f));
        m_brush->SetColor(D2D1::ColorF(0, 0, 0, 0));
        Icon(c, cx, hc.y + 44 * s, 26 * s, static_cast<int>(WeatherService::KindOf(h.code, h.day)));
        wchar_t tb2[12]; swprintf_s(tb2, L"%d°", h.tempC);
        Text(c, tb2, m_fCenter.Get(), Rect{cx - colW / 2, hc.y + 62 * s, colW, 20 * s}, W(0.95f));
        if (h.rainPct >= 10) { wchar_t rb[8]; swprintf_s(rb, L"%d%%", h.rainPct); Text(c, rb, m_fSmall.Get(), Rect{cx - colW / 2, hc.y + 80 * s, colW, 18 * s}, Color{0.50f, 0.76f, 1.0f, 0.95f}); }
    }

    // daily
    Rect dc{x, hc.Bottom() + 8 * s, wid, P.Bottom() - 14 * s - (hc.Bottom() + 8 * s)};
    Fill(c, dc, 18 * s, W(0.06f)); Stroke(c, dc, 18 * s, W(0.07f));
    int lo = 99, hi = -99;
    for (const auto& d : w.days) { lo = std::min(lo, d.loC); hi = std::max(hi, d.hiC); }
    const int rows = std::min<int>(5, static_cast<int>(w.days.size()));
    const float rowH = (dc.height - 12 * s) / std::max(rows, 1);
    for (int i = 0; i < rows; ++i) {
        const auto& d = w.days[i];
        const float y = dc.y + 6 * s + i * rowH;
        Rect rr{dc.x + 6 * s, y, dc.width - 12 * s, rowH};
        std::wstring nm = i == 0 ? std::wstring(i18n::Pick(L"Today", L"Bugün")) : std::wstring(i18n::Weekday(d.dow)).substr(0, 3);
        Text(c, nm, m_fBody.Get(), Rect{rr.x + 10 * s, y, 62 * s, rowH}, W(i == 0 ? 0.97f : 0.80f));
        Icon(c, rr.x + 88 * s, y + rowH / 2, 22 * s, static_cast<int>(WeatherService::KindOf(d.code, true)));
        if (d.rainPct >= 10) { wchar_t rb[8]; swprintf_s(rb, L"%d%%", d.rainPct); Text(c, rb, m_fSmall.Get(), Rect{rr.x + 108 * s, y, 40 * s, rowH}, Color{0.50f, 0.76f, 1.0f, 0.95f}); }
        wchar_t lb[10], hb[10]; swprintf_s(lb, L"%d°", d.loC); swprintf_s(hb, L"%d°", d.hiC);
        Text(c, lb, m_fBody.Get(), Rect{rr.x + 150 * s, y, 36 * s, rowH}, W(0.55f));
        const float bx = rr.x + 190 * s, bw = rr.Right() - 50 * s - bx, by = y + rowH / 2;
        const float span = static_cast<float>(std::max(hi - lo, 1));
        Fill(c, Rect{bx, by - 2.5f * s, bw, 5 * s}, 2.5f * s, W(0.12f));
        const float a = bx + bw * (d.loC - lo) / span, b = bx + bw * (d.hiC - lo) / span;
        if (auto g = m_dcomp->CreateLinearGradientBrush(Color{0.45f, 0.75f, 1.0f, 1}, Color{1.0f, 0.70f, 0.30f, 1}, D2D1::Point2F(bx, by), D2D1::Point2F(bx + bw, by)))
            if (auto geo = m_dcomp->CreateSquircleGeometry(Rect{a, by - 2.5f * s, std::max(b - a, 5.0f * s), 5 * s}, 2.5f * s)) c->FillGeometry(geo.Get(), g.Get());
        Text(c, hb, m_fBody.Get(), Rect{rr.Right() - 44 * s, y, 38 * s, rowH}, W(0.96f));
    }
}

// =============================================================================================
//  Notifications
// =============================================================================================
void NotifyPanel::OnShow() { m_scroll = m_scrollTarget = 0; m_refresh = 0; m_shown = m_time; NotificationFeed::Instance().RequestRefresh(); }
void NotifyPanel::OnTick(float dt) {
    m_refresh += dt;
    if (m_refresh > 3.0f) { m_refresh = 0; NotificationFeed::Instance().RequestRefresh(); }
    m_scroll += (m_scrollTarget - m_scroll) * std::min(1.0f, dt * 14.0f);
}
void NotifyPanel::OnWheel(float n) { m_scrollTarget = std::max(0.0f, m_scrollTarget - n * 70.0f * S()); }

void NotifyPanel::OnClick(int id, const Vec2&) {
    if (id == 1) NotificationFeed::Instance().DismissAll();
    else if (id == 2) { ShellExecuteW(nullptr, L"open", L"ms-settings:notifications", nullptr, nullptr, SW_SHOWNORMAL); Hide(); }
    else if (id >= 1000) {
        const size_t idx = static_cast<size_t>(id - 1000);
        if (idx < m_ids.size()) NotificationFeed::Instance().Dismiss(m_ids[idx]);
    }
}

static std::wstring AgoText(long long unixSec) {
    const long long now = static_cast<long long>(std::time(nullptr));
    long long d = std::max(0LL, now - unixSec);
    wchar_t b[40];
    if (d < 60) return i18n::Pick(L"Just now", L"Şimdi");
    if (d < 3600) { swprintf_s(b, i18n::Pick(L"%lld min ago", L"%lld dk önce"), d / 60); return b; }
    if (d < 86400) { swprintf_s(b, i18n::Pick(L"%lld h ago", L"%lld sa önce"), d / 3600); return b; }
    swprintf_s(b, i18n::Pick(L"%lld d ago", L"%lld gün önce"), d / 86400); return b;
}

void NotifyPanel::OnDraw(ID2D1DeviceContext* c, const Rect& P) {
    const float s = S();
    const Color acc = render::glass::kVioletLight;
    auto& feed = NotificationFeed::Instance();
    const auto items = feed.Items();
    m_ids.clear();
    const float x = P.x + 18 * s, w = P.width - 36 * s;

    Text(c, i18n::Pick(L"Notifications", L"Bildirimler"), m_fTitle.Get(), Rect{x + 4 * s, P.y + 16 * s, 200 * s, 30 * s}, W(0.96f));
    if (!items.empty()) {
        Rect clr{P.Right() - 18 * s - 132 * s, P.y + 16 * s, 132 * s, 30 * s};
        Fill(c, clr, 15 * s, clr.Contains(m_mouse) ? W(0.16f) : W(0.08f)); AddHit(clr, 1);
        Text(c, i18n::Pick(L"Clear all", L"Tümünü temizle"), m_fCenter.Get(), clr, W(0.88f));
    }

    const float viewTop = P.y + 60 * s, viewBottom = P.Bottom() - 64 * s;
    const float cardH = 88 * s, gap = 8 * s;
    m_contentH = items.empty() ? 0.0f : static_cast<float>(items.size()) * (cardH + gap);
    m_scrollTarget = std::clamp(m_scrollTarget, 0.0f, std::max(0.0f, m_contentH - (viewBottom - viewTop)));
    const float since = m_time - m_shown;

    c->PushAxisAlignedClip(D2D1::RectF(P.x, viewTop - 2 * s, P.Right(), viewBottom), D2D1_ANTIALIAS_MODE_ALIASED);
    if (items.empty()) {
        const float cx = P.Center().x, cy = (viewTop + viewBottom) / 2 - 24 * s;
        const float sway = std::sin(m_time * 1.8f) * 0.14f;
        // glowing bell with a swinging clapper
        if (auto g = m_dcomp->CreateRadialGradientBrush(Color{acc.r, acc.g, acc.b, 0.30f}, Color{acc.r, acc.g, acc.b, 0.0f}, D2D1::Point2F(cx, cy), 90 * s, 90 * s))
            c->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 90 * s, 90 * s), g.Get());
        D2D1_MATRIX_3X2_F old; c->GetTransform(&old);
        c->SetTransform(D2D1::Matrix3x2F::Rotation(sway * 57.2958f, D2D1::Point2F(cx, cy - 30 * s)) * old);
        Glyph(c, kBell, Rect{cx - 50 * s, cy - 50 * s, 100 * s, 100 * s}, W(0.9f), true);
        c->SetTransform(old);
        Text(c, i18n::Pick(L"You're all caught up", L"Hepsi tamam"), m_fCTitle.Get(), Rect{P.x, cy + 54 * s, P.width, 30 * s}, W(0.92f));
        Text(c, i18n::Pick(L"No new notifications", L"Yeni bildirim yok"), m_fCSmall.Get(), Rect{P.x, cy + 82 * s, P.width, 24 * s}, W(0.5f));
        if (!feed.Readable())
            Text(c, i18n::Pick(L"Windows notifications can't be read on this PC", L"Bu PC'de Windows bildirimleri okunamıyor"), m_fCSmall.Get(), Rect{P.x + 20 * s, cy + 108 * s, P.width - 40 * s, 40 * s}, Color{1.0f, 0.7f, 0.5f, 0.8f});
    }
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& n = items[i];
        m_ids.push_back(n.id);
        const float delay = std::min(0.05f * static_cast<float>(i), 0.4f);
        const float t = Clamp01((since - delay) / 0.30f), e = 1 - (1 - t) * (1 - t) * (1 - t);
        const float y = viewTop + static_cast<float>(i) * (cardH + gap) - m_scroll + (1 - e) * 26 * s;
        if (y > viewBottom || y + cardH < viewTop) continue;
        Rect r{x + (1 - e) * 24 * s, y, w, cardH};
        D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), e, nullptr, D2D1_LAYER_OPTIONS1_NONE);
        c->PushLayer(&lp, nullptr);
        const bool hot = r.Contains(m_mouse);
        Fill(c, r, 18 * s, hot ? W(0.11f) : W(0.07f)); Stroke(c, r, 18 * s, W(hot ? 0.16f : 0.08f));
        // app icon (resolved from the Start-menu entry); falls back to a gradient badge with the initial
        Rect badge{r.x + 14 * s, r.y + 14 * s, 36 * s, 36 * s};
        AppVisual av;
        const bool haveIcon = feed.AppVisualFor(n.appId, av) && av.size > 0;
        std::wstring appLabel = (haveIcon && !av.name.empty()) ? av.name : n.app;
        bool drewIcon = false;
        if (haveIcon) {
            auto& bmp = m_icons[n.appId];
            if (!bmp) {
                D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                c->CreateBitmap(D2D1::SizeU(av.size, av.size), av.px.data(), av.size * 4, bp, &bmp);
            }
            if (bmp) {
                Fill(c, badge, 11 * s, W(0.07f));
                const float in = 3 * s;
                c->DrawBitmap(bmp.Get(), D2D1::RectF(badge.x + in, badge.y + in, badge.Right() - in, badge.Bottom() - in), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                drewIcon = true;
            }
        }
        if (!drewIcon) {
            if (auto g = m_dcomp->CreateLinearGradientBrush(render::glass::kVioletLight, render::glass::kVioletDeep, D2D1::Point2F(badge.x, badge.y), D2D1::Point2F(badge.Right(), badge.Bottom())))
                if (auto geo = m_dcomp->CreateSquircleGeometry(badge, 12 * s)) c->FillGeometry(geo.Get(), g.Get());
            std::wstring ini = n.app.empty() ? L"?" : std::wstring(1, static_cast<wchar_t>(towupper(n.app[0])));
            Text(c, ini, m_fCenter.Get(), badge, W(1.0f));
        }
        const float tx = badge.Right() + 12 * s, tw = r.Right() - tx - 40 * s;
        Text(c, appLabel, m_fSmall.Get(), Rect{tx, r.y + 8 * s, tw - 64 * s, 20 * s}, W(0.55f));
        Text(c, AgoText(n.arrivalUnix), m_fSmall.Get(), Rect{r.Right() - 118 * s, r.y + 8 * s, 78 * s, 20 * s}, W(0.40f));
        Text(c, n.title, m_fBody.Get(), Rect{tx, r.y + 28 * s, tw, 24 * s}, W(0.97f));
        Text(c, n.body, m_fSmall.Get(), Rect{tx, r.y + 52 * s, tw, 32 * s}, W(0.62f));
        if (hot) {
            Rect x1{r.Right() - 34 * s, r.y + 8 * s, 26 * s, 26 * s};
            const bool xh = x1.Contains(m_mouse);
            Fill(c, x1, 13 * s, xh ? W(0.22f) : W(0.10f));
            Glyph(c, gl::kGlyphClose, x1, W(0.9f));
            if (y > viewTop - 2 * s && y + cardH < viewBottom + 2 * s) AddHit(x1, 1000 + static_cast<int>(i));
        }
        c->PopLayer();
    }
    c->PopAxisAlignedClip();

    // footer: Windows notification settings
    Rect st{x, P.Bottom() - 52 * s, w, 38 * s};
    Fill(c, st, 19 * s, st.Contains(m_mouse) ? W(0.13f) : W(0.07f)); AddHit(st, 2);
    Text(c, i18n::Pick(L"Notification settings", L"Bildirim ayarları"), m_fCenter.Get(), st, W(0.88f));
}

// =============================================================================================
//  Network
// =============================================================================================
void NetPanel::OnShow() {
    m_scroll = m_scrollTarget = 0; m_poll = 0; m_shown = m_time; m_sel.clear(); m_pass.clear(); m_expand = 0;
    WifiService::Instance().ClearStatus();
    WifiService::Instance().Refresh(true);
}
void NetPanel::OnTick(float dt) {
    m_poll += dt;
    if (m_poll > 12.0f) { m_poll = 0; WifiService::Instance().Refresh(true); }
    m_scroll += (m_scrollTarget - m_scroll) * std::min(1.0f, dt * 14.0f);
    m_expand += ((m_sel.empty() ? 0.0f : 1.0f) - m_expand) * std::min(1.0f, dt * 12.0f);
}
void NetPanel::OnWheel(float n) { m_scrollTarget = std::max(0.0f, m_scrollTarget - n * 60.0f * S()); }

void NetPanel::OnChar(wchar_t ch) {
    if (m_sel.empty()) return;
    if (ch == 8) { if (!m_pass.empty()) m_pass.pop_back(); }
    else if (ch >= 32 && m_pass.size() < 63) m_pass.push_back(ch);
}
void NetPanel::OnKey(WPARAM vk) {
    if (vk == VK_RETURN && !m_sel.empty() && m_pass.size() >= (m_selAuth == 9 ? 1u : 8u)) { WifiService::Instance().Connect(m_sel, m_pass, false, m_selSecure, m_selAuth); m_sel.clear(); m_pass.clear(); }
}

void NetPanel::OnClick(int id, const Vec2&) {
    auto& wf = WifiService::Instance();
    const WifiState st = wf.Get();
    if (id == 1) { wf.SetRadio(!st.radioOn); }
    else if (id == 2) wf.Refresh(true);
    else if (id == 3) { ShellExecuteW(nullptr, L"open", L"ms-settings:network", nullptr, nullptr, SW_SHOWNORMAL); Hide(); }
    else if (id == 4) { ShellExecuteW(nullptr, L"open", L"ms-settings:network-airplanemode", nullptr, nullptr, SW_SHOWNORMAL); Hide(); }
    else if (id == 5) wf.Disconnect();
    else if (id == 6) m_showPass = !m_showPass;
    else if (id == 7) { if (!m_sel.empty() && m_pass.size() >= (m_selAuth == 9 ? 1u : 8u)) { wf.Connect(m_sel, m_pass, false, m_selSecure, m_selAuth); m_sel.clear(); m_pass.clear(); } }
    else if (id == 8) { m_sel.clear(); m_pass.clear(); }
    else if (id == 9) { ShellExecuteW(nullptr, L"open", L"ncpa.cpl", nullptr, nullptr, SW_SHOWNORMAL); Hide(); }
    else if (id >= 100 && id < 400) {
        const size_t idx = static_cast<size_t>(id - 100);
        if (idx >= st.nets.size()) return;
        const auto& n = st.nets[idx];
        if (n.connected) return;
        if (n.known || !n.secure) { wf.Connect(n.ssid, L"", n.known, n.secure, n.authAlgo); m_sel.clear(); }
        else { m_sel = n.ssid; m_pass.clear(); m_selSecure = true; m_selAuth = n.authAlgo; }
    }
}

void NetPanel::Bars(ID2D1DeviceContext* c, float x, float y, float h, int signal, const Color& col) {
    const float s = S();
    const int lit = signal >= 75 ? 4 : signal >= 50 ? 3 : signal >= 25 ? 2 : 1;
    for (int i = 0; i < 4; ++i) {
        const float bh = h * (0.35f + 0.65f * (i + 1) / 4.0f), bx = x + i * 5 * s;
        m_brush->SetColor(D2D1::ColorF(col.r, col.g, col.b, i < lit ? col.a : 0.22f));
        c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, y + h - bh, bx + 3.2f * s, y + h), 1.4f * s, 1.4f * s), m_brush.Get());
    }
}

void NetPanel::OnDraw(ID2D1DeviceContext* c, const Rect& P) {
    const float s = S();
    const Color acc = render::glass::kVioletLight, accD = render::glass::kVioletDeep;
    auto& wf = WifiService::Instance();
    const WifiState st = wf.Get();
    const NetState ns = QueryNetState();
    const float x = P.x + 18 * s, w = P.width - 36 * s;

    // header + Wi-Fi switch
    Text(c, i18n::Pick(L"Network", L"Ağ"), m_fTitle.Get(), Rect{x + 4 * s, P.y + 16 * s, 160 * s, 30 * s}, W(0.96f));
    {
        const float target = st.radioOn ? 1.0f : 0.0f;
        m_switch += (target - m_switch) * 0.18f;
        Rect sw{P.Right() - 18 * s - 52 * s, P.y + 19 * s, 52 * s, 26 * s};
        Fill(c, sw, 13 * s, Mix(W(0.14f), Color{acc.r, acc.g, acc.b, 0.95f}, m_switch));
        const float kx = sw.x + 3 * s + m_switch * 26 * s;
        m_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.97f));
        c->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx + 10 * s, sw.Center().y), 10 * s, 10 * s), m_brush.Get());
        AddHit(Rect{sw.x - 6 * s, sw.y - 4 * s, sw.width + 12 * s, sw.height + 8 * s}, st.adapter ? 1 : 0);
        Text(c, i18n::Pick(L"Wi-Fi", L"Wi-Fi"), m_fSmall.Get(), Rect{sw.x - 54 * s, sw.y, 48 * s, sw.height}, W(0.6f));
    }

    // hero card: animated rings + current connection
    Rect hero{x, P.y + 58 * s, w, 112 * s};
    {
        auto geo = m_dcomp->CreateSquircleGeometry(hero, 22 * s);
        if (geo) {
            if (auto g = m_dcomp->CreateLinearGradientBrush(Color{accD.r, accD.g, accD.b, 0.55f}, Color{accD.r * 0.4f, accD.g * 0.4f, accD.b * 0.6f, 0.30f}, D2D1::Point2F(hero.x, hero.y), D2D1::Point2F(hero.Right(), hero.Bottom())))
                c->FillGeometry(geo.Get(), g.Get());
            D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE);
            c->PushLayer(&lp, nullptr);
            const float cx = hero.x + 52 * s, cy = hero.Center().y;
            const bool on = ns.connected && st.radioOn || ns.ethernet;
            if (on) for (int i = 0; i < 3; ++i) {
                const float ph = Frac(m_time * 0.45f + i / 3.0f);
                m_brush->SetColor(D2D1::ColorF(acc.r, acc.g, acc.b, (1 - ph) * 0.45f));
                c->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), (16 + ph * 78) * s, (16 + ph * 78) * s), m_brush.Get(), 1.6f * s);
            }
            if (st.scanning) {   // radar sweep
                const float a = m_time * 3.2f;
                m_brush->SetColor(D2D1::ColorF(acc.r, acc.g, acc.b, 0.55f));
                c->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx + std::cos(a) * 44 * s, cy + std::sin(a) * 44 * s), m_brush.Get(), 1.6f * s, nullptr);
            }
            c->PopLayer();
            if (auto rim = m_dcomp->CreateLinearGradientBrush(W(0.30f), W(0.05f), D2D1::Point2F(hero.x, hero.y), D2D1::Point2F(hero.Right(), hero.Bottom()))) c->DrawGeometry(geo.Get(), rim.Get(), 1.0f * s);
            Rect badge{cx - 22 * s, cy - 22 * s, 44 * s, 44 * s};
            Fill(c, badge, 22 * s, Color{acc.r, acc.g, acc.b, on ? 0.95f : 0.25f});
            Glyph(c, ns.ethernet && !ns.wifi ? gl::kGlyphEthernet : gl::kGlyphWifi, badge, W(on ? 1.0f : 0.6f), true);
            std::wstring title = on ? (ns.name.empty() ? std::wstring(i18n::Pick(L"Connected", L"Bağlı")) : ns.name) : std::wstring(st.radioOn ? i18n::Pick(L"Not connected", L"Bağlı değil") : i18n::Pick(L"Wi-Fi is off", L"Wi-Fi kapalı"));
            std::wstring sub = on ? (ns.internet ? std::wstring(i18n::Pick(L"Connected, internet access", L"Bağlı, internet erişimi var")) : std::wstring(i18n::Pick(L"No internet access", L"İnternet erişimi yok"))) : std::wstring(i18n::Pick(L"Choose a network below", L"Aşağıdan bir ağ seç"));
            Text(c, title, m_fTitle.Get(), Rect{hero.x + 92 * s, hero.y + 22 * s, hero.width - 108 * s, 26 * s}, W(0.97f));
            Text(c, sub, m_fSmall.Get(), Rect{hero.x + 92 * s, hero.y + 46 * s, hero.width - 108 * s, 22 * s}, W(0.62f));
            if (on && !ns.ip.empty()) Text(c, ns.ip, m_fSmall.Get(), Rect{hero.x + 92 * s, hero.y + 64 * s, hero.width - 108 * s, 20 * s}, W(0.42f));
            if (on && ns.wifi) {
                Rect db{hero.x + 92 * s, hero.Bottom() - 34 * s, 104 * s, 26 * s};
                Fill(c, db, 13 * s, db.Contains(m_mouse) ? W(0.20f) : W(0.10f)); AddHit(db, 5);
                Text(c, i18n::Pick(L"Disconnect", L"Bağlantıyı kes"), m_fCenter.Get(), db, W(0.88f));
                // centre text is left-aligned; nudge via dedicated centred format below
            }
        }
    }

    // networks header + rescan
    const float lt = hero.Bottom() + 14 * s;
    Text(c, i18n::Pick(L"Available networks", L"Kullanılabilir ağlar"), m_fSmall.Get(), Rect{x + 4 * s, lt, 220 * s, 24 * s}, W(0.55f));
    {
        Rect rf{P.Right() - 18 * s - 30 * s, lt - 3 * s, 30 * s, 30 * s};
        Fill(c, rf, 15 * s, rf.Contains(m_mouse) ? W(0.16f) : W(0.07f)); AddHit(rf, 2);
        D2D1_MATRIX_3X2_F old; c->GetTransform(&old);
        if (st.scanning) c->SetTransform(D2D1::Matrix3x2F::Rotation(std::fmod(m_time * 360.0f, 360.0f), D2D1::Point2F(rf.Center().x, rf.Center().y)) * old);
        Glyph(c, gl::kGlyphRefresh, rf, W(0.9f));
        c->SetTransform(old);
    }

    // list
    const float viewTop = lt + 32 * s, viewBottom = P.Bottom() - 70 * s;
    const float rowH = 54 * s, gap = 6 * s, expH = 98 * s;
    const float since = m_time - m_shown;
    float contentH = 0;
    c->PushAxisAlignedClip(D2D1::RectF(P.x, viewTop - 2 * s, P.Right(), viewBottom), D2D1_ANTIALIAS_MODE_ALIASED);
    if (!st.adapter) Text(c, i18n::Pick(L"No Wi-Fi adapter found", L"Wi-Fi bağdaştırıcısı bulunamadı"), m_fCBody.Get(), Rect{P.x, viewTop + 30 * s, P.width, 28 * s}, W(0.5f));
    else if (!st.radioOn) Text(c, i18n::Pick(L"Turn Wi-Fi on to see networks", L"Ağları görmek için Wi-Fi'yi aç"), m_fCBody.Get(), Rect{P.x, viewTop + 30 * s, P.width, 28 * s}, W(0.5f));
    else if (st.nets.empty()) Text(c, st.scanning ? i18n::Pick(L"Searching for networks...", L"Ağlar aranıyor...") : i18n::Pick(L"No networks found", L"Ağ bulunamadı"), m_fCBody.Get(), Rect{P.x, viewTop + 30 * s, P.width, 28 * s}, W(0.5f));
    float y = viewTop - m_scroll;
    for (size_t i = 0; i < st.nets.size(); ++i) {
        const auto& n = st.nets[i];
        const bool sel = !m_sel.empty() && n.ssid == m_sel;
        const float h = rowH + (sel ? expH * m_expand : 0.0f);
        const float delay = std::min(0.04f * static_cast<float>(i), 0.4f);
        const float t = Clamp01((since - delay) / 0.28f), e = 1 - (1 - t) * (1 - t) * (1 - t);
        Rect r{x, y + (1 - e) * 14 * s, w, h};
        if (r.Bottom() > viewTop - 4 * s && r.y < viewBottom) {
            D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), e, nullptr, D2D1_LAYER_OPTIONS1_NONE);
            c->PushLayer(&lp, nullptr);
            const bool hot = r.Contains(m_mouse) && m_mouse.y < viewBottom && m_mouse.y > viewTop;
            Fill(c, r, 17 * s, n.connected ? Color{acc.r, acc.g, acc.b, 0.20f} : (hot || sel ? W(0.11f) : W(0.06f)));
            Stroke(c, r, 17 * s, n.connected ? Color{acc.r, acc.g, acc.b, 0.55f} : W(0.07f));
            Bars(c, r.x + 16 * s, r.y + 15 * s, 22 * s, n.signal, n.connected ? acc : W(0.85f));
            Text(c, n.ssid, m_fBody.Get(), Rect{r.x + 50 * s, r.y + 8 * s, r.width - 110 * s, 22 * s}, W(0.97f));
            std::wstring sub = n.connected ? std::wstring(i18n::Pick(L"Connected", L"Bağlı")) : (n.known ? std::wstring(i18n::Pick(L"Saved", L"Kayıtlı")) : (n.secure ? std::wstring(i18n::Pick(L"Secured", L"Korumalı")) : std::wstring(i18n::Pick(L"Open", L"Açık"))));
            Text(c, sub, m_fSmall.Get(), Rect{r.x + 50 * s, r.y + 28 * s, r.width - 110 * s, 20 * s}, n.connected ? acc : W(0.5f));
            if (n.secure && !n.connected) Glyph(c, gl::kGlyphLock, Rect{r.Right() - 44 * s, r.y, 30 * s, rowH}, W(0.5f));
            if (n.connected) Glyph(c, gl::kGlyphCheck, Rect{r.Right() - 44 * s, r.y, 30 * s, rowH}, acc);
            if (r.y + rowH > viewTop && r.y < viewBottom) AddHit(Rect{r.x, std::max(r.y, viewTop), r.width, rowH}, 100 + static_cast<int>(i));
            if (sel && m_expand > 0.05f) {
                // password field
                Rect pf{r.x + 14 * s, r.y + rowH - 2 * s, r.width - 74 * s, 34 * s};
                Fill(c, pf, 12 * s, Color{0, 0, 0, 0.30f}); Stroke(c, pf, 12 * s, Color{acc.r, acc.g, acc.b, 0.65f});
                std::wstring shown = m_showPass ? m_pass : std::wstring(m_pass.size(), L'•');
                const bool caret = std::fmod(m_time, 1.0f) < 0.55f;
                if (m_pass.empty()) Text(c, i18n::Pick(L"Network password", L"Ağ şifresi"), m_fSmall.Get(), Rect{pf.x + 12 * s, pf.y, pf.width - 50 * s, pf.height}, W(0.35f));
                else Text(c, shown + (caret ? L"|" : L""), m_fBody.Get(), Rect{pf.x + 12 * s, pf.y, pf.width - 50 * s, pf.height}, W(0.95f));
                Rect eye{pf.Right() - 34 * s, pf.y, 30 * s, pf.height};
                Glyph(c, gl::kGlyphView, eye, W(m_showPass ? 0.95f : 0.45f)); AddHit(eye, 6);
                Rect ok{r.Right() - 52 * s, pf.y, 40 * s, pf.height};
                const bool can = m_pass.size() >= (m_selAuth == 9 ? 1u : 8u);
                Fill(c, ok, 12 * s, Color{acc.r, acc.g, acc.b, can ? 0.95f : 0.25f});
                Glyph(c, gl::kGlyphEnter, ok, W(can ? 1.0f : 0.5f)); AddHit(ok, 7);
                Rect cancel{r.x + 14 * s, pf.Bottom() + 6 * s, 90 * s, 24 * s};
                Text(c, i18n::Pick(L"Cancel", L"İptal"), m_fSmall.Get(), cancel, cancel.Contains(m_mouse) ? W(0.9f) : W(0.5f)); AddHit(cancel, 8);
            }
            c->PopLayer();
        }
        y += h + gap; contentH += h + gap;
    }
    c->PopAxisAlignedClip();
    m_scrollTarget = std::clamp(m_scrollTarget, 0.0f, std::max(0.0f, contentH - (viewBottom - viewTop)));

    if (!st.status.empty()) Text(c, st.status, m_fSmall.Get(), Rect{x, viewBottom - 22 * s, w, 22 * s}, Color{1.0f, 0.62f, 0.55f, 0.95f});
    if (st.busy) {   // connecting shimmer along the bottom of the list
        const float ph = Frac(m_time * 0.9f);
        m_brush->SetColor(D2D1::ColorF(acc.r, acc.g, acc.b, 0.85f));
        c->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + ph * (w - 80 * s), viewBottom + 2 * s, x + ph * (w - 80 * s) + 80 * s, viewBottom + 5 * s), 1.5f * s, 1.5f * s), m_brush.Get());
    }

    // footer buttons
    const float fy = P.Bottom() - 54 * s, bw = (w - 8 * s) / 2;
    Rect b1{x, fy, bw, 38 * s}, b2{x + bw + 8 * s, fy, bw, 38 * s};
    Fill(c, b1, 19 * s, b1.Contains(m_mouse) ? W(0.14f) : W(0.07f)); AddHit(b1, 3);
    Text(c, i18n::Pick(L"Network settings", L"Ağ ayarları"), m_fCenter.Get(), b1, W(0.88f));
    Fill(c, b2, 19 * s, b2.Contains(m_mouse) ? W(0.14f) : W(0.07f)); AddHit(b2, 4);
    Text(c, i18n::Pick(L"Airplane mode", L"Uçak modu"), m_fCenter.Get(), b2, W(0.88f));
}

} // namespace aether::bar
