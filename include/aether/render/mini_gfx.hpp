#pragma once
// Tiny immediate-mode drawing helper on top of DCompLayer, shared by the small Aether tools (AetherShell, AetherBin ...).
// Everything is in DIPs; the caller sets the DPI scale transform on the context.
#include <windows.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include "aether/render/dcomp_layer.hpp"
#include "aether/render/glass.hpp"

namespace aether::mg {

using core::Color;
using core::Rect;
using Microsoft::WRL::ComPtr;

inline const Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
inline D2D1_RECT_F D(const Rect& r) { return D2D1::RectF(r.x, r.y, r.Right(), r.Bottom()); }
inline D2D1_COLOR_F DC(const Color& c, float a = -1.0f) { return D2D1::ColorF(c.r, c.g, c.b, a < 0 ? c.a : a); }
inline Color Mix(const Color& a, const Color& b, float k) { return Color{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, a.a + (b.a - a.a) * k}; }
inline Color WithA(const Color& c, float a) { return Color{c.r, c.g, c.b, a}; }
inline float Ease(float v, float target, float dt, float speed) { return v + (target - v) * (1.0f - std::exp(-speed * dt)); }

class Gfx {
public:
    render::DCompLayer* dc{nullptr};
    ID2D1DeviceContext* ctx{nullptr};
    ComPtr<ID2D1SolidColorBrush> brush;
    std::wstring textFont{L"Segoe UI Variable Text"}, iconFont{L"Segoe Fluent Icons"};

    void Init(render::DCompLayer* layer) {
        dc = layer;
        brush = dc->CreateSolidBrush(kWhite);
        ComPtr<IDWriteFontCollection> col;
        dc->GetDWriteFactory()->GetSystemFontCollection(&col, FALSE);
        auto has = [&](const wchar_t* n) { UINT32 i = 0; BOOL ex = FALSE; return col && SUCCEEDED(col->FindFamilyName(n, &i, &ex)) && ex; };
        if (!has(L"Segoe Fluent Icons")) iconFont = L"Segoe MDL2 Assets";
        if (!has(L"Segoe UI Variable Text")) textFont = L"Segoe UI";
    }
    ID2D1DeviceContext* Begin() { ctx = dc ? dc->BeginDraw() : nullptr; return ctx; }
    void End() { if (dc && ctx) dc->EndDraw(); ctx = nullptr; }

    void FillRect(const Rect& r, const Color& c) { if (c.a <= 0.001f) return; brush->SetColor(DC(c)); ctx->FillRectangle(D(r), brush.Get()); }
    void FillRR(const Rect& r, float rad, const Color& c) {
        if (c.a <= 0.001f) return;
        brush->SetColor(DC(c));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(D(r), rad, rad), brush.Get());
    }
    void StrokeRR(const Rect& r, float rad, const Color& c, float w = 1.0f) {
        if (c.a <= 0.001f) return;
        brush->SetColor(DC(c));
        const float h = w * 0.5f;
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x + h, r.y + h, r.Right() - h, r.Bottom() - h), rad, rad), brush.Get(), w);
    }
    void VGradientRR(const Rect& r, float rad, const Color& top, const Color& bot) {
        if (auto g = dc->CreateLinearGradientBrush(top, bot, D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.Bottom())))
            ctx->FillRoundedRectangle(D2D1::RoundedRect(D(r), rad, rad), g.Get());
    }
    void Glow(const Rect& r, float rad, const Color& c, float spread, float strength) {
        if (render::glass::CurrentTheme().flat) return;
        const int N = 10;
        for (int i = 0; i < N; ++i) {
            const float t = (i + 0.5f) / N, grow = t * spread;
            const float a = strength * (1.0f - t) * (1.0f - t) * 0.30f;
            if (a < 0.004f) continue;
            StrokeRR(Rect{r.x - grow, r.y - grow, r.width + 2 * grow, r.height + 2 * grow}, rad + grow, WithA(c, a), spread / N * 1.9f);
        }
    }

    IDWriteTextFormat* Fmt(float size, int weight, bool icon) {
        const uint32_t key = static_cast<uint32_t>(size * 10) | (static_cast<uint32_t>(weight / 100) << 14) | (icon ? 1u << 22 : 0u);
        auto it = m_fmts.find(key);
        if (it != m_fmts.end()) return it->second.Get();
        ComPtr<IDWriteTextFormat> f;
        if (FAILED(dc->GetDWriteFactory()->CreateTextFormat(icon ? iconFont.c_str() : textFont.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight),
                                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f))) return nullptr;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_fmts[key] = f;
        return f.Get();
    }
    void ClearFormats() { m_fmts.clear(); }

    // One line of text inside `r` (vertically centred).
    void Text(const std::wstring& s, const Rect& r, float size, int weight, const Color& c, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING, bool shadow = false) {
        auto* f = Fmt(size, weight, false);
        if (!f || s.empty()) return;
        ComPtr<IDWriteTextLayout> lay;
        if (FAILED(dc->GetDWriteFactory()->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, r.width, r.height, &lay)) || !lay) return;
        lay->SetTextAlignment(al);
        lay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ell;
        if (SUCCEEDED(dc->GetDWriteFactory()->CreateEllipsisTrimmingSign(lay.Get(), &ell))) lay->SetTrimming(&tr, ell.Get());
        if (shadow) { brush->SetColor(D2D1::ColorF(0, 0, 0, 0.45f * c.a)); ctx->DrawTextLayout(D2D1::Point2F(r.x + 1, r.y + 2), lay.Get(), brush.Get()); }
        brush->SetColor(DC(c));
        ctx->DrawTextLayout(D2D1::Point2F(r.x, r.y), lay.Get(), brush.Get());
    }
    // Wrapped text, at most `maxLines` lines, ellipsis at the end.
    void TextBox(const std::wstring& s, const Rect& r, float size, int weight, const Color& c, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_CENTER) {
        auto* f = Fmt(size, weight, false);
        if (!f || s.empty()) return;
        ComPtr<IDWriteTextLayout> lay;
        if (FAILED(dc->GetDWriteFactory()->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, r.width, r.height, &lay)) || !lay) return;
        lay->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
        lay->SetTextAlignment(al);
        DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ell;
        if (SUCCEEDED(dc->GetDWriteFactory()->CreateEllipsisTrimmingSign(lay.Get(), &ell))) lay->SetTrimming(&tr, ell.Get());
        brush->SetColor(DC(c));
        ctx->DrawTextLayout(D2D1::Point2F(r.x, r.y), lay.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    float TextWidth(const std::wstring& s, float size, int weight) {
        auto* f = Fmt(size, weight, false);
        if (!f || s.empty()) return 0;
        ComPtr<IDWriteTextLayout> lay;
        if (FAILED(dc->GetDWriteFactory()->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 4000.0f, 100.0f, &lay)) || !lay) return 0;
        DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm);
        return tm.widthIncludingTrailingWhitespace;
    }
    void Glyph(wchar_t g, const Rect& r, float size, const Color& c) {
        auto* f = Fmt(size, 400, true);
        if (!f) return;
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        brush->SetColor(DC(c));
        ctx->DrawText(&g, 1, f, D(r), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

private:
    std::unordered_map<uint32_t, ComPtr<IDWriteTextFormat>> m_fmts;
};

// ---- bitmaps ----------------------------------------------------------------------------------------------------------------
// Shell icon / thumbnail of a path as a BGRA pixel array (alpha forced to opaque when the source has none).
inline bool ShellImage(const std::wstring& path, int px, bool thumb, int& w, int& h, std::vector<uint32_t>& out) {
    ComPtr<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) || !f) return false;
    HBITMAP hbm = nullptr;
    const SIIGBF flags = thumb ? SIIGBF_BIGGERSIZEOK : static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK);
    if (FAILED(f->GetImage(SIZE{px, px}, flags, &hbm)) || !hbm) return false;
    BITMAP bm{}; GetObjectW(hbm, sizeof(bm), &bm);
    bool ok = false;
    if (bm.bmWidth > 0 && bm.bmHeight > 0) {
        w = bm.bmWidth; h = bm.bmHeight;
        out.assign(static_cast<size_t>(w) * h, 0);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        HDC dcc = GetDC(nullptr);
        if (GetDIBits(dcc, hbm, 0, h, out.data(), &bi, DIB_RGB_COLORS)) {
            bool any = false;
            for (uint32_t p : out) if (p >> 24) { any = true; break; }
            if (!any) for (auto& p : out) if (thumb || (p & 0x00FFFFFF)) p |= 0xFF000000;
            ok = true;
        }
        ReleaseDC(nullptr, dcc);
    }
    DeleteObject(hbm);
    return ok;
}

inline ComPtr<ID2D1Bitmap1> BitmapFromPixels(ID2D1DeviceContext* ctx, const std::vector<uint32_t>& px, int w, int h) {
    ComPtr<ID2D1Bitmap1> bmp;
    if (!ctx || w <= 0 || h <= 0 || px.size() < static_cast<size_t>(w) * h) return bmp;
    ctx->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4,
                      D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmp);
    return bmp;
}

inline ComPtr<ID2D1Bitmap1> BitmapFromHicon(ID2D1DeviceContext* ctx, HICON icon) {
    ComPtr<ID2D1Bitmap1> out;
    if (!ctx || !icon) return out;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || !wic) return out;
    ComPtr<IWICBitmap> wb;
    if (FAILED(wic->CreateBitmapFromHICON(icon, &wb)) || !wb) return out;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) || FAILED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return out;
    ctx->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &out);
    return out;
}

} // namespace aether::mg
