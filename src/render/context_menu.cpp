#include "aether/render/context_menu.hpp"
#include "aether/render/dcomp_layer.hpp"
#include "aether/render/glass.hpp"
#include <algorithm>
#include <cmath>
#include <memory>

namespace aether::ui {

using core::Color;
using core::Rect;
using Microsoft::WRL::ComPtr;

namespace {
const wchar_t* kClass = L"AetherContextMenu";
LRESULT CALLBACK MenuProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}
inline float Ease(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }
}

int ShowContextMenu(const std::vector<MenuEntry>& items, POINT pt, bool above) {
    if (items.empty()) return 0;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = MenuProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc); registered = true;
    }
    const float s = std::max(1.0f, static_cast<float>(GetDpiForSystem()) / 96.0f);
    const float pad = 36.0f * s, inner = 8.0f * s, itemH = 38.0f * s, sepH = 13.0f * s, radius = 18.0f * s;

    // fonts + measuring need DirectWrite: use a throw-away window/layer below, so measure after creating it
    const int tmpW = 400, tmpH = 400;
    HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP, 0, 0, tmpW, tmpH, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return 0;
    auto layer = std::make_unique<render::DCompLayer>();
    if (!layer->Initialize(hwnd, tmpW, tmpH)) { DestroyWindow(hwnd); return 0; }
    IDWriteFactory* dw = layer->GetDWriteFactory();
    ComPtr<IDWriteFontCollection> col; dw->GetSystemFontCollection(&col, FALSE);
    auto has = [&](const wchar_t* n) { UINT32 i = 0; BOOL e = FALSE; return col && SUCCEEDED(col->FindFamilyName(n, &i, &e)) && e; };
    const wchar_t* text = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* ico = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : (has(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets" : nullptr);
    ComPtr<IDWriteTextFormat> fLabel, fHint, fIcon;
    auto mk = [&](ComPtr<IDWriteTextFormat>& f, const wchar_t* fam, DWRITE_FONT_WEIGHT wt, float sz, DWRITE_TEXT_ALIGNMENT al) {
        if (!fam) return;
        dw->CreateTextFormat(fam, nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sz * s, L"en-us", &f);
        if (!f) return;
        f->SetTextAlignment(al); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    mk(fLabel, text, DWRITE_FONT_WEIGHT_NORMAL, 13.5f, DWRITE_TEXT_ALIGNMENT_LEADING);
    mk(fHint, text, DWRITE_FONT_WEIGHT_NORMAL, 11.5f, DWRITE_TEXT_ALIGNMENT_TRAILING);
    mk(fIcon, ico, DWRITE_FONT_WEIGHT_NORMAL, 15.0f, DWRITE_TEXT_ALIGNMENT_CENTER);

    float maxLabel = 0, maxHint = 0; bool anyIcon = false;
    for (const auto& e : items) {
        if (e.separator) continue;
        if (e.glyph) anyIcon = true;
        ComPtr<IDWriteTextLayout> tl;
        if (fLabel && SUCCEEDED(dw->CreateTextLayout(e.label.c_str(), static_cast<UINT32>(e.label.size()), fLabel.Get(), 1000, 40, &tl))) { DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); maxLabel = std::max(maxLabel, m.widthIncludingTrailingWhitespace); }
        if (!e.hint.empty() && fHint && SUCCEEDED(dw->CreateTextLayout(e.hint.c_str(), static_cast<UINT32>(e.hint.size()), fHint.Get(), 1000, 40, &tl))) { DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); maxHint = std::max(maxHint, m.widthIncludingTrailingWhitespace); }
    }
    const float iconCol = anyIcon ? 40.0f * s : 14.0f * s;
    const float panelW = std::max(210.0f * s, inner * 2 + iconCol + maxLabel + (maxHint > 0 ? maxHint + 28.0f * s : 0.0f) + 18.0f * s);
    float panelH = inner * 2;
    for (const auto& e : items) panelH += e.separator ? sepH : itemH;

    const int winW = static_cast<int>(panelW + pad * 2), winH = static_cast<int>(panelH + pad * 2);
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    int px = pt.x - static_cast<int>(pad), py = above ? pt.y - winH + static_cast<int>(pad) : pt.y - static_cast<int>(pad);
    if (!above && py + winH - pad > mi.rcMonitor.bottom) py = pt.y - winH + static_cast<int>(pad);
    px = std::clamp(px, static_cast<int>(mi.rcMonitor.left) - static_cast<int>(pad) + 6, static_cast<int>(mi.rcMonitor.right) - winW + static_cast<int>(pad) - 6);
    py = std::clamp(py, static_cast<int>(mi.rcMonitor.top) - static_cast<int>(pad) + 6, static_cast<int>(mi.rcMonitor.bottom) - winH + static_cast<int>(pad) - 6);
    SetWindowPos(hwnd, HWND_TOPMOST, px, py, winW, winH, SWP_NOACTIVATE);
    layer->Resize(winW, winH);
    auto brush = layer->CreateSolidBrush(Color{1.0f, 1.0f, 1.0f, 1.0f});

    // item rects (window coordinates)
    std::vector<Rect> rects(items.size());
    float y = pad + inner;
    for (size_t i = 0; i < items.size(); ++i) {
        const float h = items[i].separator ? sepH : itemH;
        rects[i] = Rect{pad + inner, y, panelW - inner * 2, h};
        y += h;
    }
    const Rect panel{pad, pad, panelW, panelH};

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    std::vector<float> hover(items.size(), 0.0f);
    int result = 0, pressed = -1;
    bool closing = false, prevDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    bool prevEsc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    ULONGLONG last = GetTickCount64(), t0 = last, closeAt = 0;
    const auto accent = render::glass::kVioletLight;

    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { PostQuitMessage(static_cast<int>(msg.wParam)); closing = true; result = 0; closeAt = 1; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        const ULONGLONG now = GetTickCount64();
        const float dt = std::min(0.05f, (now - last) / 1000.0f); last = now;
        POINT cp; GetCursorPos(&cp);
        const float lx = static_cast<float>(cp.x - px), ly = static_cast<float>(cp.y - py);
        int hot = -1;
        if (!closing) for (size_t k = 0; k < items.size(); ++k) if (!items[k].separator && items[k].enabled && rects[k].Contains(lx, ly)) { hot = static_cast<int>(k); break; }
        for (size_t k = 0; k < items.size(); ++k) hover[k] += ((static_cast<int>(k) == hot ? 1.0f : 0.0f) - hover[k]) * std::min(1.0f, dt * 18.0f);

        const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        const bool lDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (!closing) {
            if (down && !prevDown) {
                if (hot >= 0 && lDown) pressed = hot;
                else if (!panel.Contains(lx, ly)) { result = 0; closing = true; closeAt = now; }
            }
            if (!down && prevDown && pressed >= 0) {
                if (pressed == hot) { result = items[pressed].id; closing = true; closeAt = now; }
                pressed = -1;
            }
            if (esc && !prevEsc) { result = 0; closing = true; closeAt = now; }
            if (GetForegroundWindow() == nullptr) { result = 0; closing = true; closeAt = now; }
        }
        prevDown = down; prevEsc = esc;

        const float openT = Ease((now - t0) / 170.0f);
        float alpha = openT;
        if (closing) {
            if (closeAt == 1) break;
            alpha = openT * (1.0f - Ease((now - closeAt) / 110.0f));
            if (now - closeAt >= 110) break;
        }

        // ---- draw
        auto body = layer->CreateSquircleGeometry(panel, radius);
        auto sil = render::RecordSilhouette(layer.get(), {body.Get()}, s, 0.8f);
        auto ctx = layer->BeginDraw();
        if (ctx) {
            const float sc = 0.96f + 0.04f * alpha;
            ctx->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(above ? panel.Center().x : panel.x + 20 * s, above ? panel.Bottom() : panel.y)));
            D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), alpha, nullptr, D2D1_LAYER_OPTIONS1_NONE);
            ctx->PushLayer(&lp, nullptr);
            render::DrawGlassGlow(ctx, sil.Get(), s, 0.8f);
            if (body) render::DrawGlassBody(ctx, layer.get(), brush.Get(), body.Get(), panel, s, 1.0f);
            for (size_t k = 0; k < items.size(); ++k) {
                const auto& e = items[k]; const Rect& r = rects[k];
                const float stag = Ease((now - t0 - k * 14.0f) / 150.0f);
                if (e.separator) {
                    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.10f * stag));
                    ctx->FillRectangle(D2D1::RectF(r.x + 10 * s, r.y + r.height / 2, r.Right() - 10 * s, r.y + r.height / 2 + 1.0f), brush.Get());
                    continue;
                }
                const float h = hover[k];
                if (h > 0.01f) {
                    if (auto g = layer->CreateSquircleGeometry(r, 12.0f * s)) {
                        brush->SetColor(D2D1::ColorF(e.danger ? 1.0f : accent.r, e.danger ? 0.35f : accent.g, e.danger ? 0.40f : accent.b, 0.20f * h));
                        ctx->FillGeometry(g.Get(), brush.Get());
                    }
                    brush->SetColor(D2D1::ColorF(e.danger ? 1.0f : accent.r, e.danger ? 0.45f : accent.g, e.danger ? 0.50f : accent.b, 0.95f * h));
                    ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x + 3 * s, r.y + 11 * s, r.x + 6 * s, r.Bottom() - 11 * s), 1.5f * s, 1.5f * s), brush.Get());
                }
                const float dim = (e.enabled ? 1.0f : 0.4f) * stag;
                Color tc = e.danger ? Color{1.0f, 0.52f, 0.55f, 1.0f} : Color{1.0f, 1.0f, 1.0f, 0.92f};
                tc.a *= dim;
                if (e.glyph && fIcon) {
                    wchar_t g[2] = {e.glyph, 0};
                    brush->SetColor(D2D1::ColorF(e.danger ? 1.0f : accent.r, e.danger ? 0.52f : accent.g, e.danger ? 0.55f : accent.b, (0.75f + 0.25f * h) * dim));
                    const D2D1_RECT_F ir = D2D1::RectF(r.x + 8 * s, r.y, r.x + 8 * s + 28 * s, r.Bottom());
                    ctx->DrawText(g, 1, fIcon.Get(), &ir, brush.Get());
                }
                brush->SetColor(D2D1::ColorF(tc.r, tc.g, tc.b, tc.a));
                const D2D1_RECT_F lr = D2D1::RectF(r.x + iconCol, r.y, r.Right() - 10 * s, r.Bottom());
                ctx->DrawText(e.label.c_str(), static_cast<UINT32>(e.label.size()), fLabel.Get(), &lr, brush.Get());
                if (!e.hint.empty() && fHint) {
                    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.42f * dim));
                    const D2D1_RECT_F hr = D2D1::RectF(r.x + iconCol, r.y, r.Right() - 12 * s, r.Bottom());
                    ctx->DrawText(e.hint.c_str(), static_cast<UINT32>(e.hint.size()), fHint.Get(), &hr, brush.Get());
                }
            }
            ctx->PopLayer();
            ctx->SetTransform(D2D1::Matrix3x2F::Identity());
            layer->EndDraw();
        }
        Sleep(8);
    }
    layer.reset();
    DestroyWindow(hwnd);
    return result;
}

} // namespace aether::ui
