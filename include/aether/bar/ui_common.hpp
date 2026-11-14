#pragma once
// Shared helpers for every Aether shell window: modern cursors, icon conversion, glyph codes.
#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "aether/core/types.hpp"
#include "aether/render/dcomp_layer.hpp"
#include "aether/render/glass.hpp"

namespace aether::bar::ui {

using Microsoft::WRL::ComPtr;

// ---- palette: one violet accent used by every Aether shell window ---------------------------------
// (references into the live glass theme, so a theme change recolours the whole shell)
inline core::Color& kPurple = render::glass::kViolet;
inline core::Color& kPurpleLight = render::glass::kVioletLight;
inline core::Color& kPurpleDeep = render::glass::kVioletDeep;
inline constexpr core::Color WithA(const core::Color& c, float a) { return core::Color{c.r, c.g, c.b, a}; }

constexpr wchar_t kGlyphSearch = 0xE721, kGlyphPower = 0xE7E8, kGlyphSettings = 0xE713, kGlyphChevronUp = 0xE70E,
                  kGlyphChevronLeft = 0xE76B, kGlyphChevronRight = 0xE76C, kGlyphVolume = 0xE767, kGlyphMute = 0xE74F,
                  kGlyphWifi = 0xE701, kGlyphEthernet = 0xE839, kGlyphGlobe = 0xE774, kGlyphLock = 0xE72E,
                  kGlyphFolder = 0xE8B7, kGlyphDisplay = 0xE7F4, kGlyphBluetooth = 0xE702, kGlyphMusic = 0xE8D6,
                  kGlyphBack = 0xE72B, kGlyphForward = 0xE72A, kGlyphUp = 0xE74A, kGlyphRefresh = 0xE72C,
                  kGlyphHome = 0xE80F, kGlyphDelete = 0xE74D, kGlyphCopy = 0xE8C8, kGlyphCut = 0xE8C6,
                  kGlyphPaste = 0xE77F, kGlyphRename = 0xE8AC, kGlyphNewFolder = 0xE8F4, kGlyphView = 0xE8FD,
                  kGlyphSort = 0xE8CB, kGlyphClose = 0xE711, kGlyphDrive = 0xEDA2, kGlyphDownload = 0xE896,
                  kGlyphDocument = 0xE8A5, kGlyphPicture = 0xE91B, kGlyphDesktop = 0xE7F4, kGlyphInfo = 0xE946,
                  kGlyphVideo = 0xE714, kGlyphPin = 0xE718, kGlyphCalendar = 0xE787, kGlyphSleep = 0xE708,
                  kGlyphRestart = 0xE777, kGlyphSignOut = 0xF3B1, kGlyphApps = 0xE71D,
                  kGlyphMic = 0xE720, kGlyphCheck = 0xE73E, kGlyphArrowDown = 0xE74B, kGlyphArrowUp = 0xE74A, kGlyphEnter = 0xE751;

struct Pt { float x, y; };

// Procedural modern cursors: dark body with a violet edge (arrow) / glowing ring (link), 4x4 supersampled.
inline HCURSOR MakeCursor(bool link, int size) {
    const int N = size;
    const float k = N / 32.0f;
    std::vector<Pt> poly = {{3, 2}, {3, 24.5f}, {8.6f, 19.2f}, {12.6f, 28.2f}, {16.4f, 26.6f}, {12.4f, 17.8f}, {19.8f, 17.8f}};
    for (auto& p : poly) { p.x *= k; p.y *= k; }
    const float outline = 1.7f * k;

    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi); bi.bV5Width = N; bi.bV5Height = -N; bi.bV5Planes = 1; bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP mask = CreateBitmap(N, N, 1, 1, nullptr);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;

    auto segDist = [](float px, float py, Pt a, Pt b) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        float t = ((px - a.x) * dx + (py - a.y) * dy) / (dx * dx + dy * dy);
        t = std::clamp(t, 0.0f, 1.0f);
        const float cx = a.x + t * dx - px, cy = a.y + t * dy - py;
        return std::sqrt(cx * cx + cy * cy);
    };
    auto inside = [&](float px, float py) {
        bool in = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            if (((poly[i].y > py) != (poly[j].y > py)) &&
                (px < (poly[j].x - poly[i].x) * (py - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)) in = !in;
        }
        return in;
    };

    auto* out = static_cast<uint32_t*>(bits);
    const float cxr = 16.0f * k, cyr = 16.0f * k, R = 9.5f * k;
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
        float r = 0, g = 0, b = 0, a = 0;
        for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {
            const float px = x + (sx + 0.5f) / 4.0f, py = y + (sy + 0.5f) / 4.0f;
            float cr = 0, cg = 0, cb = 0, ca = 0;
            if (!link) {
                float d = 1e9f;
                for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) d = std::min(d, segDist(px, py, poly[j], poly[i]));
                const bool in = inside(px, py);
                if (d < outline * 0.5f) { cr = 0.62f; cg = 0.42f; cb = 1.0f; ca = 1.0f; }
                else if (in) { cr = 0.06f; cg = 0.06f; cb = 0.09f; ca = 1.0f; }
            } else {
                const float d = std::sqrt((px - cxr) * (px - cxr) + (py - cyr) * (py - cyr));
                if (std::fabs(d - R) < 1.6f * k) { cr = 0.62f; cg = 0.42f; cb = 1.0f; ca = 1.0f; }
                else if (d < R - 1.6f * k) { cr = 0.06f; cg = 0.06f; cb = 0.09f; ca = (d < 2.6f * k) ? 1.0f : 0.78f;
                                             if (d < 2.6f * k) { cr = 0.62f; cg = 0.42f; cb = 1.0f; } }
                else if (std::fabs(d - R) < 3.4f * k) { cr = 0.62f; cg = 0.42f; cb = 1.0f; ca = 0.22f; }
            }
            r += cr * ca; g += cg * ca; b += cb * ca; a += ca;
        }
        const float inv = 1.0f / 16.0f;
        out[y * N + x] = (static_cast<uint32_t>(std::clamp(a * inv, 0.0f, 1.0f) * 255.0f) << 24) |
                         (static_cast<uint32_t>(std::clamp(r * inv, 0.0f, 1.0f) * 255.0f) << 16) |
                         (static_cast<uint32_t>(std::clamp(g * inv, 0.0f, 1.0f) * 255.0f) << 8) |
                          static_cast<uint32_t>(std::clamp(b * inv, 0.0f, 1.0f) * 255.0f);
    }
    ICONINFO ii = {};
    ii.fIcon = FALSE;
    ii.xHotspot = link ? static_cast<DWORD>(16 * k) : static_cast<DWORD>(3 * k);
    ii.yHotspot = link ? static_cast<DWORD>(16 * k) : static_cast<DWORD>(2 * k);
    ii.hbmMask = mask; ii.hbmColor = color;
    HCURSOR cur = reinterpret_cast<HCURSOR>(CreateIconIndirect(&ii));
    DeleteObject(color); DeleteObject(mask);
    return cur;
}

inline HCURSOR Arrow() { static HCURSOR c = MakeCursor(false, std::max(32, GetSystemMetrics(SM_CXCURSOR))); return c ? c : LoadCursorW(nullptr, IDC_ARROW); }
inline HCURSOR Link()  { static HCURSOR c = MakeCursor(true,  std::max(32, GetSystemMetrics(SM_CXCURSOR))); return c ? c : LoadCursorW(nullptr, IDC_HAND); }
inline void ApplyCursor(bool link) { SetCursor(link ? Link() : Arrow()); }

inline ComPtr<ID2D1Bitmap1> IconToBitmap(ID2D1DeviceContext* ctx, HICON icon, int px) {
    ComPtr<ID2D1Bitmap1> bmp;
    if (!ctx || !icon) return bmp;
    // Render at the icon's own native size (GDI would otherwise stretch a 16 px image with poor filtering);
    // the GPU then scales it with a high-quality filter. Large icons (>= requested size) are drawn at the requested size.
    {
        ICONINFO ii{};
        if (GetIconInfo(icon, &ii)) {
            BITMAP bm{};
            HBITMAP src = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
            if (src && GetObject(src, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmWidth < px) px = bm.bmWidth;
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
        }
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = px; bi.bmiHeader.biHeight = -px;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        HGDIOBJ old = SelectObject(dc, dib);
        memset(bits, 0, static_cast<size_t>(px) * px * 4);
        DrawIconEx(dc, 0, 0, icon, px, px, 0, nullptr, DI_NORMAL);
        GdiFlush();
        auto* p = static_cast<uint32_t*>(bits);
        bool any = false;
        for (int i = 0; i < px * px; ++i) if (p[i] >> 24) { any = true; break; }
        if (!any) for (int i = 0; i < px * px; ++i) if (p[i] & 0x00FFFFFF) p[i] |= 0xFF000000;
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ctx->CreateBitmap(D2D1::SizeU(px, px), bits, px * 4, props, &bmp);
        SelectObject(dc, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(dc);
    return bmp;
}

} // namespace aether::bar::ui
