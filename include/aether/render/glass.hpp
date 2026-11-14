#pragma once
// "Violet glass" look shared by the Start menu, the bar pills, the flyouts and the desktop widgets.
//
// Windows offers no reliable shaped blur for a DirectComposition window, so the glass is drawn: a nearly
// opaque dark-violet body, inner light blooms, a gradient rim and a real Gaussian outer glow.
//
//  1. Before DCompLayer::BeginDraw(): auto sil = RecordSilhouette(layer, {geo1, geo2, ...});
//  2. Inside the draw:                 DrawGlassGlow(ctx, sil.Get(), scale);  then DrawGlassBody(...) per panel.
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <wrl/client.h>
#include <algorithm>
#include <initializer_list>
#include <map>
#include <vector>
#include <utility>
#include <cmath>
#include <string>
#include <cstdlib>
#include <windows.h>
#include "aether/core/types.hpp"
#include "aether/render/dcomp_layer.hpp"

#pragma comment(lib, "dxguid.lib")

namespace aether::render {

namespace glass {
// Live palette: every panel reads these, a theme change just rewrites them.
inline core::Color kViolet{0.545f, 0.361f, 0.965f, 1.0f};       // accent
inline core::Color kVioletLight{0.722f, 0.592f, 1.0f, 1.0f};    // highlight
inline core::Color kVioletDeep{0.388f, 0.227f, 0.882f, 1.0f};   // gradient end
inline core::Color A(const core::Color& c, float a) { return core::Color{c.r, c.g, c.b, a}; }

// Second accent: the theme accent rotated a little around the colour wheel (violet -> vivid purple, blue -> indigo,
// green -> teal ...). Rings, bars, glow and the clock use it so every theme gets the same two-tone depth as the
// concept without a second hand-picked palette per theme. Flat (Windows 11 style) themes keep a single colour.
inline core::Color kAccent2{0.722f, 0.300f, 0.965f, 1.0f};
inline core::Color kAccent2Light{0.800f, 0.592f, 1.0f, 1.0f};
// Text tints follow the theme too (almost white, a breath of the highlight colour).
inline core::Color kText{0.95f, 0.94f, 0.98f, 1.0f};
inline core::Color kMuted{0.69f, 0.66f, 0.78f, 1.0f};

// Rotates the hue of `c` by `deg` degrees (HSV), optionally scaling the saturation. Alpha is kept.
inline core::Color HueShift(const core::Color& c, float deg, float satScale = 1.0f) {
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    float h = 0.0f;
    if (d > 1e-5f) {
        if (mx == c.r)      h = std::fmod((c.g - c.b) / d, 6.0f);
        else if (mx == c.g) h = (c.b - c.r) / d + 2.0f;
        else                h = (c.r - c.g) / d + 4.0f;
        h *= 60.0f;
        if (h < 0.0f) h += 360.0f;
    }
    const float s = mx > 1e-5f ? std::min(1.0f, d / mx * satScale) : 0.0f;
    h = std::fmod(h + deg + 720.0f, 360.0f) / 60.0f;
    const float ch = mx * s, x = ch * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f)), m = mx - ch;
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(h)) {
        case 0: r = ch; g = x; break;
        case 1: r = x; g = ch; break;
        case 2: g = ch; b = x; break;
        case 3: g = x; b = ch; break;
        case 4: r = x; b = ch; break;
        default: r = ch; b = x; break;
    }
    return core::Color{r + m, g + m, b + m, c.a};
}

struct ThemeDef {
    const wchar_t* nameEn; const wchar_t* nameTr;
    core::Color accent, light, deep;
    core::Color bodyTop, bodyBot;     // alpha included
    float glow;                       // multiplier of the coloured halo
    float bloom;                      // multiplier of the inner light
    float rimWhite;                   // alpha of the far end of the rim
    float shadow;                     // black drop shadow alpha
    bool bold{false};                 // "classic dock" look: solid lilac start tile, tinted active tile, brighter rim
    bool flat{false};                 // Windows-11 like: neutral dark pill, no halo, thin grey rim
    float hue{20.0f};                 // degrees the second accent is rotated from the accent (kAccent2)
    float sat{1.0f};                  // saturation scale of the second accent
};
inline const ThemeDef* Themes(int* count) {
    static const ThemeDef t[] = {
        // Violet body colours were measured from the concept (deep saturated purple, not grey-violet)
        {L"Violet", L"Mor",        {0.545f,0.361f,0.965f,1}, {0.722f,0.592f,1.0f,1}, {0.388f,0.227f,0.882f,1}, {0.088f,0.052f,0.138f,0.935f}, {0.046f,0.028f,0.074f,0.950f}, 1.0f, 1.0f, 0.16f, 0.60f, false, false, 20.0f, 1.12f},
        {L"OLED Black", L"OLED Siyah", {0.80f,0.72f,1.0f,1}, {0.93f,0.90f,1.0f,1}, {0.55f,0.45f,0.95f,1}, {0.0f,0.0f,0.0f,0.985f}, {0.0f,0.0f,0.0f,0.99f}, 0.30f, 0.15f, 0.22f, 0.50f, false, false, 10.0f, 1.0f},
        {L"Clear Glass", L"Şeffaf Cam", {0.45f,0.72f,1.0f,1}, {0.78f,0.90f,1.0f,1}, {0.25f,0.50f,0.95f,1}, {0.16f,0.18f,0.24f,0.50f}, {0.08f,0.10f,0.15f,0.58f}, 0.55f, 0.55f, 0.38f, 0.45f, false, false, 12.0f, 1.0f},
        {L"Ocean", L"Okyanus",     {0.20f,0.55f,1.0f,1}, {0.52f,0.78f,1.0f,1}, {0.10f,0.35f,0.90f,1}, {0.050f,0.090f,0.160f,0.955f}, {0.030f,0.050f,0.100f,0.965f}, 1.0f, 1.0f, 0.16f, 0.60f, false, false, 12.0f, 1.0f},
        {L"Emerald", L"Zümrüt",    {0.12f,0.78f,0.52f,1}, {0.46f,0.95f,0.74f,1}, {0.06f,0.55f,0.38f,1}, {0.045f,0.110f,0.095f,0.955f}, {0.025f,0.065f,0.055f,0.965f}, 0.95f, 1.0f, 0.16f, 0.60f, false, false, 14.0f, 1.0f},
        {L"Rose", L"Gül",          {0.96f,0.30f,0.55f,1}, {1.0f,0.58f,0.75f,1}, {0.78f,0.16f,0.40f,1}, {0.140f,0.065f,0.095f,0.955f}, {0.085f,0.040f,0.060f,0.965f}, 1.0f, 1.0f, 0.16f, 0.60f, false, false, -14.0f, 1.0f},
        {L"Classic", L"Klasik",    {0.38f,0.80f,1.0f,1}, {0.55f,0.86f,1.0f,1}, {0.0f,0.47f,0.83f,1}, {0.125f,0.125f,0.135f,0.900f}, {0.090f,0.090f,0.100f,0.925f}, 0.0f, 0.0f, 0.18f, 0.45f, false, true, 0.0f, 1.0f},
        {L"Lavender", L"Lavanta",  {0.72f,0.58f,1.0f,1}, {0.86f,0.78f,1.0f,1}, {0.52f,0.38f,0.95f,1}, {0.165f,0.105f,0.262f,0.84f}, {0.100f,0.062f,0.178f,0.90f}, 0.90f, 1.0f, 0.28f, 0.45f, true, false, 16.0f, 1.05f},
        {L"Sunset", L"Gün Batımı", {1.0f,0.55f,0.18f,1}, {1.0f,0.76f,0.45f,1}, {0.90f,0.34f,0.10f,1}, {0.150f,0.095f,0.060f,0.955f}, {0.090f,0.055f,0.035f,0.965f}, 0.95f, 1.0f, 0.16f, 0.60f, false, false, -12.0f, 1.0f},
    };
    if (count) *count = static_cast<int>(sizeof(t) / sizeof(t[0]));
    return t;
}
inline int& ThemeIndex() { static int i = 6; return i; }   // first launch: Classic
inline void SetTheme(int i) {
    int n = 0; const ThemeDef* t = Themes(&n);
    if (i < 0 || i >= n) i = 0;
    ThemeIndex() = i; kViolet = t[i].accent; kVioletLight = t[i].light; kVioletDeep = t[i].deep;
    if (t[i].flat) { kAccent2 = t[i].accent; kAccent2Light = t[i].light; }
    else           { kAccent2 = HueShift(t[i].accent, t[i].hue, t[i].sat); kAccent2Light = HueShift(t[i].light, t[i].hue); }
    auto mix = [](const core::Color& a, const core::Color& b, float k) { return core::Color{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, 1.0f}; };
    kText = mix(core::Color{0.975f, 0.975f, 0.980f, 1.0f}, t[i].light, 0.10f);
    kMuted = mix(core::Color{0.680f, 0.680f, 0.720f, 1.0f}, t[i].light, 0.22f);
}
inline const ThemeDef& CurrentTheme() { int n; return Themes(&n)[ThemeIndex()]; }

// The chosen theme lives in %APPDATA%\Aether\theme.txt so every Aether process (island, bar, widgets, settings) follows it.
inline std::wstring ThemeFilePath() {
    wchar_t buf[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring d = std::wstring(buf) + L"\\Aether"; CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\theme.txt";
}
inline void SaveThemeFile(int i) {
    const std::wstring p = ThemeFilePath(); if (p.empty()) return;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char b[16]; int len = wsprintfA(b, "%d", i); DWORD w; WriteFile(h, b, len, &w, nullptr); CloseHandle(h);
}
// Cheap: re-reads the file at most once a second, only when it changed. Returns true when the theme changed.
inline bool SyncTheme() {
    static bool initDone = false;
    if (!initDone) { initDone = true; SetTheme(ThemeIndex()); }   // default theme colours (Classic) before any file exists
    static ULONGLONG last = 0; static FILETIME lastWrite{};
    const ULONGLONG now = GetTickCount64();
    if (last && now - last < 1000) return false;
    last = now;
    const std::wstring p = ThemeFilePath(); if (p.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return false;
    if (fa.ftLastWriteTime.dwLowDateTime == lastWrite.dwLowDateTime && fa.ftLastWriteTime.dwHighDateTime == lastWrite.dwHighDateTime) return false;
    lastWrite = fa.ftLastWriteTime;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char b[16] = {}; DWORD r = 0; ReadFile(h, b, 15, &r, nullptr); CloseHandle(h);
    const int i = atoi(b); const int old = ThemeIndex(); SetTheme(i);
    return ThemeIndex() != old;
}
}


// ---- glow cache -------------------------------------------------------------------------------------
// The outer glow is three large Gaussian blurs of the panel silhouette. They only change when the panel's shape,
// scale or theme changes, so once a shape has been stable for two frames the finished glow is kept as a bitmap and
// simply blitted afterwards (identical pixels, a fraction of the GPU work). While a shape is still animating the
// live effects are used exactly as before.
struct GlowLayer { float sigma, dx, dy; core::Color c; };
struct CachedGlow { ComPtr<ID2D1Bitmap1> bmp; float ox{0}, oy{0}, w{0}, h{0}; explicit operator bool() const { return bmp != nullptr; } };

namespace glowcache {
struct Entry { uint64_t key{0}; ComPtr<ID2D1Device> dev; CachedGlow glow; ULONGLONG used{0}; };
inline std::vector<Entry>& Entries() { static std::vector<Entry> e; return e; }
inline std::vector<std::pair<ID2D1Device*, uint64_t>>& LastKeys() { static std::vector<std::pair<ID2D1Device*, uint64_t>> v; return v; }
inline void Mix(uint64_t& h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); }
inline void MixF(uint64_t& h, float f) { Mix(h, static_cast<uint64_t>(static_cast<int64_t>(f * 64.0f))); }
} // namespace glowcache

inline CachedGlow StableGlow(DCompLayer* layer, std::initializer_list<ID2D1Geometry*> geos, std::initializer_list<GlowLayer> layers,
                             float margin, ID2D1CommandList* pre = nullptr) {
    using namespace glowcache;
    CachedGlow none;
    ID2D1DeviceContext* ctx = layer ? layer->GetContext() : nullptr;
    if (!ctx || !layer->GetTargetBitmap()) return none;
    ComPtr<ID2D1Device> dev; ctx->GetDevice(&dev);
    if (!dev) return none;

    uint64_t key = 1469598103934665603ULL;
    D2D1_RECT_F u{1e9f, 1e9f, -1e9f, -1e9f};
    for (ID2D1Geometry* g : geos) {
        if (!g) continue;
        D2D1_RECT_F r{}; float area = 0;
        if (FAILED(g->GetBounds(nullptr, &r))) return none;
        g->ComputeArea(nullptr, &area);
        MixF(key, r.left); MixF(key, r.top); MixF(key, r.right); MixF(key, r.bottom); MixF(key, area);
        u.left = std::min(u.left, r.left); u.top = std::min(u.top, r.top); u.right = std::max(u.right, r.right); u.bottom = std::max(u.bottom, r.bottom);
    }
    if (u.right <= u.left || u.bottom <= u.top) return none;
    for (const GlowLayer& l : layers) { MixF(key, l.sigma); MixF(key, l.dx); MixF(key, l.dy); MixF(key, l.c.r); MixF(key, l.c.g); MixF(key, l.c.b); MixF(key, l.c.a); }
    Mix(key, reinterpret_cast<uint64_t>(dev.Get()));

    auto& es = Entries();
    const ULONGLONG now = GetTickCount64();
    for (auto& e : es) if (e.key == key && e.dev.Get() == dev.Get()) { e.used = now; return e.glow; }

    // not cached yet: only build once the same shape was seen on the previous frame too
    auto& lk = LastKeys();
    uint64_t* last = nullptr;
    for (auto& p : lk) if (p.first == dev.Get()) { last = &p.second; break; }
    if (!last) { lk.push_back({dev.Get(), 0}); last = &lk.back().second; }
    if (*last != key) { *last = key; return none; }

    const float ox = std::floor(u.left - margin), oy = std::floor(u.top - margin);
    const float w = std::ceil(u.right + margin) - ox, h = std::ceil(u.bottom + margin) - oy;
    if (w < 2 || h < 2 || w > 8192 || h > 8192) return none;

    ComPtr<ID2D1CommandList> cl;
    if (pre) cl = pre;
    else {
        if (FAILED(ctx->CreateCommandList(&cl)) || !cl) return none;
        ctx->SetTarget(cl.Get()); ctx->BeginDraw(); ctx->SetTransform(D2D1::Matrix3x2F::Identity());
        ComPtr<ID2D1SolidColorBrush> b; ctx->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &b);
        for (ID2D1Geometry* g : geos) if (g && b) ctx->FillGeometry(g, b.Get());
        const HRESULT hr = ctx->EndDraw(); ctx->SetTarget(layer->GetTargetBitmap()); cl->Close();
        if (FAILED(hr)) return none;
    }

    CachedGlow out;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(ctx->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), nullptr, 0, bp, &out.bmp)) || !out.bmp) return none;
    ctx->SetTarget(out.bmp.Get());
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    for (const GlowLayer& l : layers) {
        ComPtr<ID2D1Effect> fx;
        if (FAILED(ctx->CreateEffect(CLSID_D2D1Shadow, &fx)) || !fx) continue;
        fx->SetInput(0, cl.Get());
        fx->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, l.sigma);
        fx->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(l.c.r, l.c.g, l.c.b, l.c.a));
        fx->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, D2D1_SHADOW_OPTIMIZATION_QUALITY);
        ComPtr<ID2D1Image> img; fx->GetOutput(&img);
        if (img) ctx->DrawImage(img.Get(), D2D1::Point2F(l.dx - ox, l.dy - oy));
    }
    const HRESULT hr = ctx->EndDraw();
    ctx->SetTarget(layer->GetTargetBitmap());
    if (FAILED(hr)) return none;
    out.ox = ox; out.oy = oy; out.w = w; out.h = h;

    if (es.size() >= 14) {   // evict the least recently used
        size_t o = 0; for (size_t i = 1; i < es.size(); ++i) if (es[i].used < es[o].used) o = i;
        es.erase(es.begin() + static_cast<std::ptrdiff_t>(o));
    }
    es.push_back({key, dev, out, now});
    return out;
}

inline void DrawCachedGlow(ID2D1DeviceContext* ctx, const CachedGlow& g) {
    if (ctx && g.bmp) ctx->DrawBitmap(g.bmp.Get(), D2D1::RectF(g.ox, g.oy, g.ox + g.w, g.oy + g.h), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

inline std::map<ID2D1CommandList*, CachedGlow>& SilhouetteGlows() { static std::map<ID2D1CommandList*, CachedGlow> m; return m; }
// The panel shapes behind a silhouette: DrawGlassGlow erases them from the glow so the halo exists only OUTSIDE the
// panel (like CSS box-shadow). Without this the shadow + violet fill sat under the body and tinted every translucent theme.
inline std::map<ID2D1CommandList*, std::vector<ComPtr<ID2D1Geometry>>>& SilhouetteGeos() { static std::map<ID2D1CommandList*, std::vector<ComPtr<ID2D1Geometry>>> m; return m; }

// Records the filled silhouettes (window pixel coordinates) into a command list. Must run outside BeginDraw.
inline ComPtr<ID2D1CommandList> RecordSilhouette(DCompLayer* layer, std::initializer_list<ID2D1Geometry*> geos, float s = 0.0f, float strength = 1.0f) {
    ID2D1DeviceContext* ctx = layer ? layer->GetContext() : nullptr;
    if (!ctx || !layer->GetTargetBitmap()) return nullptr;
    ComPtr<ID2D1CommandList> cl;
    if (FAILED(ctx->CreateCommandList(&cl)) || !cl) return nullptr;
    ctx->SetTarget(cl.Get());
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ComPtr<ID2D1SolidColorBrush> b;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &b);
    for (ID2D1Geometry* g : geos) if (g && b) ctx->FillGeometry(g, b.Get());
    const HRESULT hr = ctx->EndDraw();
    ctx->SetTarget(layer->GetTargetBitmap());
    cl->Close();
    if (FAILED(hr)) return nullptr;
    {   // remember the shapes so DrawGlassGlow can cut them out of the halo
        auto& gr = SilhouetteGeos();
        if (gr.size() > 24) gr.clear();
        auto& v = gr[cl.Get()];
        v.clear();
        for (ID2D1Geometry* g : geos) if (g) v.emplace_back(g);
    }
    // glow cache: pre-render (outside BeginDraw) once the shape is stable; DrawGlassGlow blits it
    auto& reg = SilhouetteGlows();
    if (reg.size() > 24) reg.clear();
    reg.erase(cl.Get());
    if (s > 0.0f) {
        using namespace glass;
        const auto& th = CurrentTheme();
        CachedGlow cg = StableGlow(layer, geos, {
            {16.0f * s, 0.0f, 9.0f * s, core::Color{0.0f, 0.0f, 0.0f, th.shadow}},
            {13.0f * s, 0.0f, 0.0f, A(kViolet, 0.42f * strength * th.glow)},
            {14.0f * s, 7.0f * s, 6.0f * s, A(kAccent2, 0.18f * strength * th.glow)},   // second hue, leans to the lower right
            {3.5f * s, 0.0f, 0.0f, A(kVioletLight, 0.30f * strength * th.glow)}}, 62.0f * s + 4.0f, cl.Get());
        if (cg) reg[cl.Get()] = cg;
    }
    return cl;
}

// Blurred, tinted copy of the silhouette (drawn with the context's current transform and layers).
inline void DrawBlurredSilhouette(ID2D1DeviceContext* ctx, ID2D1CommandList* sil, const core::Color& c, float sigma, float dx = 0.0f, float dy = 0.0f) {
    if (!ctx || !sil) return;
    ComPtr<ID2D1Effect> fx;
    if (FAILED(ctx->CreateEffect(CLSID_D2D1Shadow, &fx)) || !fx) return;
    fx->SetInput(0, sil);
    fx->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, sigma);
    fx->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(c.r, c.g, c.b, c.a));
    fx->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, D2D1_SHADOW_OPTIMIZATION_QUALITY);
    ComPtr<ID2D1Image> out;
    fx->GetOutput(&out);
    if (out) ctx->DrawImage(out.Get(), D2D1::Point2F(dx, dy));
}

// Erases the panel shapes from what has been drawn so far (the halo), so a translucent body shows the desktop and not
// a muddy shadow. Skipped for flat themes, which were tuned with the shadow behind the pill.
//   Set AETHER_GLASS_PUNCH_OUT to 0 (e.g. in CMake: -DAETHER_GLASS_PUNCH_OUT=0) to go back to the old behaviour
//   if a GPU driver renders the 1 px anti-aliased edge of D2D1_PRIMITIVE_BLEND_COPY badly.
#ifndef AETHER_GLASS_PUNCH_OUT
#define AETHER_GLASS_PUNCH_OUT 1
#endif
inline void PunchOutPanels(ID2D1DeviceContext* ctx, ID2D1CommandList* sil) {
#if AETHER_GLASS_PUNCH_OUT
    if (!ctx || !sil || glass::CurrentTheme().flat) return;
    auto& gr = SilhouetteGeos();
    auto it = gr.find(sil);
    if (it == gr.end() || it->second.empty()) return;
    ComPtr<ID2D1SolidColorBrush> clear;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), &clear);
    if (!clear) return;
    ctx->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
    for (auto& g : it->second) if (g) ctx->FillGeometry(g.Get(), clear.Get());
    ctx->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
#else
    (void)ctx; (void)sil;
#endif
}

// Dark drop shadow + violet halo around the panels. `strength` scales the violet (1 = Start menu).
// Two-hue halo (theme accent + its second accent), cut out of the panel shapes afterwards.
inline void DrawGlassGlow(ID2D1DeviceContext* ctx, ID2D1CommandList* sil, float s, float strength = 1.0f) {
    using namespace glass;
    auto& reg = SilhouetteGlows();
    auto it = reg.find(sil);
    if (it != reg.end()) {
        DrawCachedGlow(ctx, it->second);
    } else {
        DrawBlurredSilhouette(ctx, sil, core::Color{0.0f, 0.0f, 0.0f, CurrentTheme().shadow}, 16.0f * s, 0.0f, 9.0f * s);
        DrawBlurredSilhouette(ctx, sil, A(kViolet, 0.42f * strength * CurrentTheme().glow), 13.0f * s);
        DrawBlurredSilhouette(ctx, sil, A(kAccent2, 0.18f * strength * CurrentTheme().glow), 14.0f * s, 7.0f * s, 6.0f * s);
        DrawBlurredSilhouette(ctx, sil, A(kVioletLight, 0.30f * strength * CurrentTheme().glow), 3.5f * s);
    }
    PunchOutPanels(ctx, sil);
}

// Body of one panel: dark violet fill, light blooms, rim. `brush` is any solid brush (recoloured here).
inline void DrawGlassBody(ID2D1DeviceContext* ctx, DCompLayer* layer, ID2D1SolidColorBrush* brush,
                          ID2D1Geometry* geo, const core::Rect& r, float s, float bloom = 1.0f, float fillScale = 1.0f) {
    // fillScale < 1 thins the body's tint so a frosted backdrop drawn underneath (see backdrop.hpp) shows through
    using namespace glass;
    if (!ctx || !layer || !geo) return;
    core::Color bodyTop = CurrentTheme().bodyTop, bodyBot = CurrentTheme().bodyBot;
    if (!CurrentTheme().flat) { bodyTop.a *= fillScale; bodyBot.a *= fillScale; }
    if (auto fill = layer->CreateLinearGradientBrush(bodyTop, bodyBot,
                                                     D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.Right(), r.Bottom())))
        ctx->FillGeometry(geo, fill.Get());

    D2D1_LAYER_PARAMETERS1 clip = D2D1::LayerParameters1(D2D1::InfiniteRect(), geo, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                        D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE);
    ctx->PushLayer(&clip, nullptr);
    bloom *= CurrentTheme().bloom;
    if (CurrentTheme().flat) {   // Windows-11 style: plain dark pill with a hairline rim
        ctx->PopLayer();
        if (auto rim = layer->CreateLinearGradientBrush(core::Color{1.0f, 1.0f, 1.0f, 0.20f}, core::Color{1.0f, 1.0f, 1.0f, 0.07f}, D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.Bottom())))
            ctx->DrawGeometry(geo, rim.Get(), 1.0f * s);
        return;
    }
    const float big = std::max(r.width, r.height);
    // violet light leaking in from the bottom-left, a faint one bottom-right, a cool sheen across the top
    if (auto g = layer->CreateRadialGradientBrush(A(kViolet, 0.065f * bloom), A(kViolet, 0.0f), D2D1::Point2F(r.x, r.Bottom()), big * 0.55f, r.height * 0.85f))
        ctx->FillGeometry(geo, g.Get());
    if (auto g = layer->CreateRadialGradientBrush(A(kVioletLight, 0.035f * bloom), A(kViolet, 0.0f), D2D1::Point2F(r.Right(), r.Bottom()), big * 0.40f, r.height * 0.60f))
        ctx->FillGeometry(geo, g.Get());
    if (auto g = layer->CreateLinearGradientBrush(A(kAccent2Light, 0.030f), A(kAccent2Light, 0.0f),
                                                  D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.y + std::min(r.height, 120.0f * s))))
        ctx->FillGeometry(geo, g.Get());
    // edge hot-spots: like the concept, the glass catches light at a couple of places - a bloom leaking in from the
    // rim, strongest at the bottom edge and a weaker one at the top, so the panel looks lit rather than evenly outlined
    {
        const float hs = std::min(1.0f, r.width / (260.0f * s));   // small pills get a gentler spot
        auto spot = [&](float fx, bool bottom, float strength) {
            const float cy = bottom ? r.Bottom() : r.y;
            const float rx = std::max(50.0f * s, r.width * 0.30f), ry = std::min(r.height * 0.5f, std::max(46.0f * s, r.height * 0.20f));
            if (auto g = layer->CreateRadialGradientBrush(A(kVioletLight, 0.30f * strength * hs * bloom), A(kViolet, 0.0f),
                                                          D2D1::Point2F(r.x + r.width * fx, cy), rx, ry))
                ctx->FillGeometry(geo, g.Get());
        };
        spot(0.46f, true, 1.0f);
        spot(0.60f, false, 0.65f);
    }
    // soft inner violet edge light
    if (brush) {
        for (int i = 4; i >= 1; --i) {
            brush->SetColor(D2D1::ColorF(kViolet.r, kViolet.g, kViolet.b, 0.045f * (5 - i) / 4.0f * bloom));
            ctx->DrawGeometry(geo, brush, (i * 3.0f) * s);
        }
    }
    ctx->PopLayer();

    // rim: theme highlight bottom-left -> second hue in the middle -> faint white top-right (the concept's rim is a
    // soft lilac hairline, much dimmer than a solid bright stroke)
    if (auto rim = layer->CreateLinearGradientBrush({{0.0f, A(kVioletLight, 0.40f)},
                                                     {0.55f, A(kAccent2Light, 0.22f)},
                                                     {1.0f, core::Color{1.0f, 1.0f, 1.0f, CurrentTheme().rimWhite}}},
                                                    D2D1::Point2F(r.x, r.Bottom()), D2D1::Point2F(r.Right(), r.y)))
        ctx->DrawGeometry(geo, rim.Get(), 1.2f * s);

    // bright rim segments at the same hot-spots (hairline + a soft shine just outside / inside of it)
    {
        const core::Color hotC = core::Color::Lerp(core::Color{1.0f, 1.0f, 1.0f, 1.0f}, kVioletLight, 0.45f);
        auto rimSpot = [&](float fx, bool bottom, float half, float a) {
            const D2D1_RECT_F clipR = bottom ? D2D1::RectF(r.x - 2.0f * s, r.y + r.height * 0.5f, r.Right() + 2.0f * s, r.Bottom() + 3.0f * s)
                                             : D2D1::RectF(r.x - 2.0f * s, r.y - 3.0f * s, r.Right() + 2.0f * s, r.y + r.height * 0.5f);
            const float lo = std::max(0.0f, fx - half), hi = std::min(1.0f, fx + half);
            auto mk = [&](float peak) {
                return layer->CreateLinearGradientBrush({{0.0f, A(hotC, 0.0f)}, {lo, A(hotC, 0.0f)}, {fx, A(hotC, peak)}, {hi, A(hotC, 0.0f)}, {1.0f, A(hotC, 0.0f)}},
                                                        D2D1::Point2F(r.x, 0.0f), D2D1::Point2F(r.Right(), 0.0f));
            };
            ctx->PushAxisAlignedClip(clipR, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            if (auto soft = mk(a * 0.30f)) ctx->DrawGeometry(geo, soft.Get(), 6.0f * s);
            if (auto line = mk(a)) ctx->DrawGeometry(geo, line.Get(), 1.5f * s);
            ctx->PopAxisAlignedClip();
        };
        const float hs = std::min(1.0f, r.width / (260.0f * s));
        rimSpot(0.46f, true, 0.24f, 0.85f * hs);
        rimSpot(0.60f, false, 0.20f, 0.60f * hs);
    }
}

} // namespace aether::render
