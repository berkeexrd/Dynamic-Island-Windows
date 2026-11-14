#pragma once
// Frosted-glass backdrop for windows that Direct2D cannot blur by itself: a snapshot of the screen
// behind the window is shrunk 4x, blurred on the CPU (tiny image, ~1 ms) and drawn, scaled back up,
// under the glass body. Capture before the window is shown (or while it is hidden) so the window itself
// is not part of the picture. Failure is harmless - the caller simply keeps its plain translucent look.
#include <windows.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <vector>
#include "aether/core/types.hpp"

namespace aether::render {

struct Backdrop {
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bmp;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> noise;   // 64x64 grain, tiled over the glass (acrylic-style)
    int w{0}, h{0};
    explicit operator bool() const { return bmp != nullptr; }
    void Reset() { bmp.Reset(); w = h = 0; }
};

namespace detail {
inline void BoxBlurBgra(uint8_t* px, int w, int h, int r, int passes) {
    if (w < 2 || h < 2 || r < 1) return;
    std::vector<uint8_t> tmp(static_cast<size_t>(w) * h * 4);
    auto pass = [&](const uint8_t* src, uint8_t* dst, int n, int stride, int lines, int lineStride) {
        const int win = 2 * r + 1;
        for (int l = 0; l < lines; ++l) {
            const uint8_t* s = src + static_cast<size_t>(l) * lineStride;
            uint8_t* d = dst + static_cast<size_t>(l) * lineStride;
            for (int c = 0; c < 3; ++c) {
                int sum = 0;
                for (int i = -r; i <= r; ++i) sum += s[std::clamp(i, 0, n - 1) * stride + c];
                for (int i = 0; i < n; ++i) {
                    d[i * stride + c] = static_cast<uint8_t>(sum / win);
                    sum += s[std::clamp(i + r + 1, 0, n - 1) * stride + c] - s[std::clamp(i - r, 0, n - 1) * stride + c];
                }
            }
            for (int i = 0; i < n; ++i) d[i * stride + 3] = 255;
        }
    };
    for (int p = 0; p < passes; ++p) {
        pass(px, tmp.data(), w, 4, h, w * 4);
        pass(tmp.data(), px, h, w * 4, w, 4);
    }
}
}  // namespace detail

// (sx, sy, pw, ph) = screen rectangle in physical pixels. `radius` = box radius on the 1/4 size image.
inline bool CaptureBackdrop(ID2D1DeviceContext* ctx, int sx, int sy, int pw, int ph, int radius, Backdrop& out) {
    if (!ctx || pw < 8 || ph < 8) return false;
    const int dw = std::max(8, (pw + 3) / 4), dh = std::max(8, (ph + 3) / 4);
    HDC scr = GetDC(nullptr);
    if (!scr) return false;
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = pw;
    bi.bmiHeader.biHeight = -ph;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = mem ? CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    bool ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        const BOOL blt = BitBlt(mem, 0, 0, pw, ph, scr, sx, sy, SRCCOPY);   // one plain copy: a few ms, no scaling filter
        SelectObject(mem, old);
        if (blt) {
            GdiFlush();
            // average 4x4 blocks into the reduced image
            std::vector<uint8_t> reduced(static_cast<size_t>(dw) * dh * 4);
            const uint8_t* src = static_cast<const uint8_t*>(bits);
            for (int y = 0; y < dh; ++y) {
                const int y0 = y * 4, y1 = std::min(ph, y0 + 4);
                for (int x = 0; x < dw; ++x) {
                    const int x0 = x * 4, x1 = std::min(pw, x0 + 4);
                    unsigned sum[3] = {0, 0, 0};
                    int n = 0;
                    for (int yy = y0; yy < y1; ++yy) {
                        const uint8_t* row = src + (static_cast<size_t>(yy) * pw + x0) * 4;
                        for (int xx = x0; xx < x1; ++xx, row += 4, ++n) { sum[0] += row[0]; sum[1] += row[1]; sum[2] += row[2]; }
                    }
                    uint8_t* d = &reduced[(static_cast<size_t>(y) * dw + x) * 4];
                    if (n == 0) n = 1;
                    d[0] = static_cast<uint8_t>(sum[0] / n); d[1] = static_cast<uint8_t>(sum[1] / n); d[2] = static_cast<uint8_t>(sum[2] / n); d[3] = 255;
                }
            }
            detail::BoxBlurBgra(reduced.data(), dw, dh, radius, 3);
            const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> bmp;
            if (SUCCEEDED(ctx->CreateBitmap(D2D1::SizeU(dw, dh), reduced.data(), dw * 4, &bp, &bmp)) && bmp) {
                out.bmp = bmp; out.w = dw; out.h = dh; ok = true;
            }
            if (ok && !out.noise) {
                std::vector<uint32_t> g(64 * 64);
                uint32_t seed = 0x1234567u;
                for (auto& px : g) {
                    seed = seed * 1664525u + 1013904223u;
                    const uint32_t n = (seed >> 24) & 0xFF;      // premultiplied white with random alpha
                    px = (n << 24) | (n << 16) | (n << 8) | n;
                }
                ctx->CreateBitmap(D2D1::SizeU(64, 64), g.data(), 64 * 4, &bp, &out.noise);
            }
        }
    }
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    return ok;
}

// Fills `geo` with the backdrop. `dest` = where the captured rectangle sits in the current drawing space.
inline void FillBackdrop(ID2D1DeviceContext* ctx, const Backdrop& b, ID2D1Geometry* geo, const core::Rect& dest, float opacity = 1.0f) {
    if (!ctx || !b.bmp || !geo || b.w <= 0 || b.h <= 0) return;
    Microsoft::WRL::ComPtr<ID2D1BitmapBrush> br;
    if (FAILED(ctx->CreateBitmapBrush(b.bmp.Get(),
            D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &br)) || !br) return;
    br->SetTransform(D2D1::Matrix3x2F::Scale(dest.width / b.w, dest.height / b.h) * D2D1::Matrix3x2F::Translation(dest.x, dest.y));
    br->SetOpacity(opacity);
    ctx->FillGeometry(geo, br.Get());
    if (b.noise) {   // very faint grain so the blur reads as frosted glass, not a smooth gradient
        Microsoft::WRL::ComPtr<ID2D1BitmapBrush> nb;
        if (SUCCEEDED(ctx->CreateBitmapBrush(b.noise.Get(),
                D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR), &nb)) && nb) {
            nb->SetOpacity(0.045f);
            ctx->FillGeometry(geo, nb.Get());
        }
    }
}

}  // namespace aether::render
