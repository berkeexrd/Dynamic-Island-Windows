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
};
inline const ThemeDef* Themes(int* count) {
    static const ThemeDef t[] = {
        {L"Violet", L"Mor",        {0.545f,0.361f,0.965f,1}, {0.722f,0.592f,1.0f,1}, {0.388f,0.227f,0.882f,1}, {0.098f,0.080f,0.150f,0.955f}, {0.052f,0.045f,0.088f,0.965f}, 1.0f, 1.0f, 0.16f, 0.60f},
        {L"OLED Black", L"OLED Siyah", {0.80f,0.72f,1.0f,1}, {0.93f,0.90f,1.0f,1}, {0.55f,0.45f,0.95f,1}, {0.0f,0.0f,0.0f,0.985f}, {0.0f,0.0f,0.0f,0.99f}, 0.30f, 0.15f, 0.22f, 0.50f},
        {L"Clear Glass", L"Şeffaf Cam", {0.45f,0.72f,1.0f,1}, {0.78f,0.90f,1.0f,1}, {0.25f,0.50f,0.95f,1}, {0.16f,0.18f,0.24f,0.50f}, {0.08f,0.10f,0.15f,0.58f}, 0.55f, 0.55f, 0.38f, 0.45f},
        {L"Ocean", L"Okyanus",     {0.20f,0.55f,1.0f,1}, {0.52f,0.78f,1.0f,1}, {0.10f,0.35f,0.90f,1}, {0.050f,0.090f,0.160f,0.955f}, {0.030f,0.050f,0.100f,0.965f}, 1.0f, 1.0f, 0.16f, 0.60f},
        {L"Emerald", L"Zümrüt",    {0.12f,0.78f,0.52f,1}, {0.46f,0.95f,0.74f,1}, {0.06f,0.55f,0.38f,1}, {0.045f,0.110f,0.095f,0.955f}, {0.025f,0.065f,0.055f,0.965f}, 0.95f, 1.0f, 0.16f, 0.60f},
        {L"Rose", L"Gül",          {0.96f,0.30f,0.55f,1}, {1.0f,0.58f,0.75f,1}, {0.78f,0.16f,0.40f,1}, {0.140f,0.065f,0.095f,0.955f}, {0.085f,0.040f,0.060f,0.965f}, 1.0f, 1.0f, 0.16f, 0.60f},
        {L"Classic", L"Klasik",    {0.38f,0.80f,1.0f,1}, {0.55f,0.86f,1.0f,1}, {0.0f,0.47f,0.83f,1}, {0.125f,0.125f,0.135f,0.900f}, {0.090f,0.090f,0.100f,0.925f}, 0.0f, 0.0f, 0.18f, 0.45f, false, true},
        {L"Lavender", L"Lavanta",  {0.72f,0.58f,1.0f,1}, {0.86f,0.78f,1.0f,1}, {0.52f,0.38f,0.95f,1}, {0.215f,0.165f,0.320f,0.80f}, {0.135f,0.105f,0.230f,0.86f}, 0.90f, 1.0f, 0.28f, 0.45f, true},
        {L"Sunset", L"Gün Batımı", {1.0f,0.55f,0.18f,1}, {1.0f,0.76f,0.45f,1}, {0.90f,0.34f,0.10f,1}, {0.150f,0.095f,0.060f,0.955f}, {0.090f,0.055f,0.035f,0.965f}, 0.95f, 1.0f, 0.16f, 0.60f},
    };
    if (count) *count = static_cast<int>(sizeof(t) / sizeof(t[0]));
    return t;
}
inline int& ThemeIndex() { static int i = 6; return i; }   // first launch: Classic
inline void SetTheme(int i) {
    int n = 0; const ThemeDef* t = Themes(&n);
    if (i < 0 || i >= n) i = 0;
    ThemeIndex() = i; kViolet = t[i].accent; kVioletLight = t[i].light; kVioletDeep = t[i].deep;
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

// Dark drop shadow + violet halo around the panels. `strength` scales the violet (1 = Start menu).
inline void DrawGlassGlow(ID2D1DeviceContext* ctx, ID2D1CommandList* sil, float s, float strength = 1.0f) {
    using namespace glass;
    {
        auto& reg = SilhouetteGlows();
        auto it = reg.find(sil);
        if (it != reg.end()) { DrawCachedGlow(ctx, it->second); return; }
    }
    DrawBlurredSilhouette(ctx, sil, core::Color{0.0f, 0.0f, 0.0f, CurrentTheme().shadow}, 16.0f * s, 0.0f, 9.0f * s);
    DrawBlurredSilhouette(ctx, sil, A(kViolet, 0.42f * strength * CurrentTheme().glow), 13.0f * s);
    DrawBlurredSilhouette(ctx, sil, A(kVioletLight, 0.30f * strength * CurrentTheme().glow), 3.5f * s);
}

// Body of one panel: dark violet fill, light blooms, rim. `brush` is any solid brush (recoloured here).
inline void DrawGlassBody(ID2D1DeviceContext* ctx, DCompLayer* layer, ID2D1SolidColorBrush* brush,
                          ID2D1Geometry* geo, const core::Rect& r, float s, float bloom = 1.0f) {
    using namespace glass;
    if (!ctx || !layer || !geo) return;
    if (auto fill = layer->CreateLinearGradientBrush(CurrentTheme().bodyTop, CurrentTheme().bodyBot,
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
    if (auto g = layer->CreateRadialGradientBrush(A(kViolet, 0.20f * bloom), A(kViolet, 0.0f), D2D1::Point2F(r.x, r.Bottom()), big * 0.55f, r.height * 0.85f))
        ctx->FillGeometry(geo, g.Get());
    if (auto g = layer->CreateRadialGradientBrush(A(kVioletLight, 0.10f * bloom), A(kViolet, 0.0f), D2D1::Point2F(r.Right(), r.Bottom()), big * 0.40f, r.height * 0.60f))
        ctx->FillGeometry(geo, g.Get());
    if (auto g = layer->CreateLinearGradientBrush(core::Color{1.0f, 1.0f, 1.0f, 0.045f}, core::Color{1.0f, 1.0f, 1.0f, 0.0f},
                                                  D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.y + std::min(r.height, 120.0f * s))))
        ctx->FillGeometry(geo, g.Get());
    // soft inner violet edge light
    if (brush) {
        for (int i = 4; i >= 1; --i) {
            brush->SetColor(D2D1::ColorF(kViolet.r, kViolet.g, kViolet.b, 0.045f * (5 - i) / 4.0f * bloom));
            ctx->DrawGeometry(geo, brush, (i * 3.0f) * s);
        }
    }
    ctx->PopLayer();

    // rim: bright violet bottom-left fading to a faint white top-right
    if (auto rim = layer->CreateLinearGradientBrush(A(kVioletLight, 0.85f), core::Color{1.0f, 1.0f, 1.0f, CurrentTheme().rimWhite},
                                                    D2D1::Point2F(r.x, r.Bottom()), D2D1::Point2F(r.Right(), r.y)))
        ctx->DrawGeometry(geo, rim.Get(), 1.2f * s);
}

} // namespace aether::render
