// AetherIntro: the greeting animation shown when Aether starts (logon / first run).
//   AetherIntro.exe           short version (~4 s)
//   AetherIntro.exe --first   welcome version (~6.5 s), launched by the installer
// Any click, Esc, Enter or Space skips it. A file %APPDATA%\Aether\no-intro disables the logon intro (AetherShell checks it).
#include <windows.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include "aether/render/dcomp_layer.hpp"
#include "aether/render/mini_gfx.hpp"

using namespace aether;
using aether::mg::Color;
using aether::mg::Rect;
using Microsoft::WRL::ComPtr;

namespace {

bool g_first = false;
float g_T = 4.2f;

float Clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }
float Smooth(float x) { x = Clamp01(x); return x * x * (3.0f - 2.0f * x); }
float EaseOutBack(float x) { x = Clamp01(x); const float c1 = 1.70158f, c3 = c1 + 1.0f; return 1.0f + c3 * std::pow(x - 1.0f, 3.0f) + c1 * std::pow(x - 1.0f, 2.0f); }

bool Turkish() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH; }

// The Aether logo is embedded as RCDATA resource 301 (aether.png); decode it with WIC.
ComPtr<ID2D1Bitmap1> LoadLogoBitmap(ID2D1DeviceContext* ctx) {
    ComPtr<ID2D1Bitmap1> out;
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC hr = FindResourceW(mod, MAKEINTRESOURCEW(301), RT_RCDATA);
    HGLOBAL hg = hr ? LoadResource(mod, hr) : nullptr;
    const void* data = hg ? LockResource(hg) : nullptr;
    const DWORD size = hr ? SizeofResource(mod, hr) : 0;
    if (!ctx || !data || !size) return out;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || !wic) return out;
    ComPtr<IWICStream> st;
    if (FAILED(wic->CreateStream(&st)) ||
        FAILED(st->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<void*>(data)), size))) return out;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec))) return out;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return out;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return out;
    ctx->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &out);
    return out;
}

struct Particle { float x, y, r, speed, phase; };

class Intro {
public:
    bool Create(HINSTANCE inst) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = Proc; wc.hInstance = inst; wc.lpszClassName = L"AetherIntroWindow"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        m_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Aether", WS_POPUP,
                                 mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, nullptr, nullptr, inst, this);
        if (!m_hwnd) return false;
        m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
        RECT rc; GetClientRect(m_hwnd, &rc);
        m_dc = std::make_unique<render::DCompLayer>();
        if (!m_dc->Initialize(m_hwnd, rc.right, rc.bottom)) return false;
        m_g.Init(m_dc.get());
        render::glass::SyncTheme();                      // follow the theme chosen in Aether settings (%APPDATA%\Aether\theme.txt)
        m_W = rc.right / m_scale; m_H = rc.bottom / m_scale;
        unsigned seed = 7;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
        for (int i = 0; i < 80; ++i) m_parts.push_back({rnd() * m_W, rnd() * m_H, 0.8f + rnd() * 2.2f, 6.0f + rnd() * 22.0f, rnd() * 6.28f});
        QueryPerformanceFrequency(&m_freq); QueryPerformanceCounter(&m_t0);
        ShowWindow(m_hwnd, SW_SHOWNA);
        SetForegroundWindow(m_hwnd);
        SetTimer(m_hwnd, 1, 8, nullptr);
        return true;
    }
    void Run() {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }

private:
    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dc;
    mg::Gfx m_g;
    float m_scale{1.0f}, m_W{0}, m_H{0};
    std::vector<Particle> m_parts;
    ComPtr<ID2D1Bitmap1> m_logo;
    LARGE_INTEGER m_freq{}, m_t0{};
    bool m_armed{false};

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Intro* self = nullptr;
        if (m == WM_NCCREATE) { self = static_cast<Intro*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        else self = reinterpret_cast<Intro*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, m, w, l);
        switch (m) {
        case WM_TIMER: self->Frame(); return 0;
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
        case WM_LBUTTONDOWN: case WM_KEYDOWN: DestroyWindow(h); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    void Radial(float cx, float cy, float r, const Color& c0, const Color& c1) {
        if (c0.a <= 0.002f && c1.a <= 0.002f) return;
        auto* ctx = m_g.ctx;
        D2D1_GRADIENT_STOP st[2] = {{0.0f, D2D1::ColorF(c0.r, c0.g, c0.b, c0.a)}, {1.0f, D2D1::ColorF(c1.r, c1.g, c1.b, c1.a)}};
        ComPtr<ID2D1GradientStopCollection> sc;
        if (FAILED(ctx->CreateGradientStopCollection(st, 2, &sc))) return;
        ComPtr<ID2D1RadialGradientBrush> br;
        if (FAILED(ctx->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), r, r), sc.Get(), &br))) return;
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), br.Get());
    }

    void Frame() {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        const float t = static_cast<float>(now.QuadPart - m_t0.QuadPart) / static_cast<float>(m_freq.QuadPart);
        if (t >= g_T) { DestroyWindow(m_hwnd); return; }
        // skip: Esc / Enter / Space, or a click (armed once the buttons were released, so the click that started us does not skip)
        const bool pressed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState(VK_RETURN) & 0x8000) || (GetAsyncKeyState(VK_SPACE) & 0x8000);
        if (!pressed) m_armed = true;
        if (pressed && m_armed && t > 0.4f) { DestroyWindow(m_hwnd); return; }
        if (t > 0.3f && GetForegroundWindow() != m_hwnd) SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        auto* ctx = m_g.Begin();
        if (!ctx) return;
        if (!m_logo) m_logo = LoadLogoBitmap(ctx);
        render::glass::SyncTheme();                      // pick up a theme change mid-intro too
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale));
        const float W = m_W, H = m_H, cx = W * 0.5f, cy = H * 0.5f - 34.0f;
        const float fadeIn = Smooth(t / 0.55f), fadeOut = 1.0f - Smooth((t - (g_T - 0.7f)) / 0.7f);
        const float A = fadeIn * fadeOut;
        // Live theme palette (updates if the theme changed after we started).
        const auto& th = render::glass::CurrentTheme();
        const Color accent{render::glass::kViolet}, light{render::glass::kVioletLight}, accent2{render::glass::kAccent2};

        m_g.FillRect(Rect{0, 0, W, H}, Color{th.bodyBot.r, th.bodyBot.g, th.bodyBot.b, A});
        // slow aurora in the theme's own colours
        const float R = std::max(W, H);
        Radial(W * (0.28f + 0.06f * std::sin(t * 0.7f)), H * (0.30f + 0.05f * std::cos(t * 0.6f)), R * 0.55f, mg::WithA(accent, 0.30f * A), mg::WithA(accent, 0.0f));
        Radial(W * (0.74f + 0.05f * std::cos(t * 0.5f)), H * (0.70f + 0.06f * std::sin(t * 0.8f)), R * 0.50f, mg::WithA(accent2, 0.20f * A), mg::WithA(accent2, 0.0f));
        Radial(W * 0.62f, H * (0.22f + 0.04f * std::sin(t * 0.9f)), R * 0.40f, mg::WithA(light, 0.14f * A), mg::WithA(light, 0.0f));

        // drifting particles
        for (const auto& p : m_parts) {
            const float y = std::fmod(p.y - t * p.speed + H * 4.0f, H);
            const float tw = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(t * 1.6f + p.phase));
            m_g.FillRR(Rect{p.x - p.r, y - p.r, p.r * 2, p.r * 2}, p.r, Color{light.r, light.g, light.b, 0.55f * tw * A * Smooth((t - 0.2f) / 1.0f)});
        }

        // the Aether logo itself (from the embedded PNG), with a soft theme-coloured halo
        const float logoK = EaseOutBack((t - 0.25f) / 1.1f);
        const float logoSize = 96.0f;
        const float s = logoSize * std::max(0.0f, logoK) * (1.0f + 0.02f * std::sin(t * 2.2f));
        if (m_logo && s > 1.0f) {
            Radial(cx, cy, s * 1.7f, mg::WithA(accent, 0.30f * A), mg::WithA(accent, 0.0f));
            D2D1_RECT_F dst = D2D1::RectF(cx - s * 0.5f, cy - s * 0.5f, cx + s * 0.5f, cy + s * 0.5f);
            ctx->DrawBitmap(m_logo.Get(), &dst, A, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }

        // title, letter by letter
        const std::wstring title = L"AETHER";
        const float fs = 62.0f, gap = 16.0f;
        float total = 0; std::vector<float> wd;
        for (wchar_t ch : title) { const float w = m_g.TextWidth(std::wstring(1, ch), fs, 300); wd.push_back(w); total += w + gap; }
        total -= gap;
        float x = cx - total * 0.5f; const float ty = cy + 78.0f;
        for (size_t i = 0; i < title.size(); ++i) {
            const float a = Smooth((t - (1.05f + 0.11f * i)) / 0.55f) * A;
            if (a > 0.003f) {
                const float oy = (1.0f - a) * 16.0f;
                m_g.Text(std::wstring(1, title[i]), Rect{x - 6, ty + oy + 2, wd[i] + 12, fs + 14}, fs, 300, Color{accent.r, accent.g, accent.b, 0.35f * a}, DWRITE_TEXT_ALIGNMENT_CENTER);
                m_g.Text(std::wstring(1, title[i]), Rect{x - 6, ty + oy, wd[i] + 12, fs + 14}, fs, 300, Color{1, 1, 1, a}, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            x += wd[i] + gap;
        }
        const bool tr = Turkish();
        const std::wstring sub = g_first ? (tr ? L"Hoş geldin. Masaüstün yeniden doğuyor." : L"Welcome. Your desktop, reborn.") : (tr ? L"Masaüstün hazırlanıyor" : L"Preparing your desktop");
        const float sa = Smooth((t - 2.0f) / 0.8f) * A;
        m_g.Text(sub, Rect{0, ty + fs + 22, W, 30}, 17.0f, 300, Color{render::glass::kText.r, render::glass::kText.g, render::glass::kText.b, 0.85f * sa}, DWRITE_TEXT_ALIGNMENT_CENTER);

        if (g_first) m_g.Text(tr ? L"Atlamak için tıkla" : L"Click to skip", Rect{0, H - 56, W, 20}, 11.5f, 400, Color{1, 1, 1, 0.38f * sa}, DWRITE_TEXT_ALIGNMENT_CENTER);
        m_g.End();
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Aether.Intro.SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (cmd && wcsstr(cmd, L"--first")) { g_first = true; g_T = 6.5f; }
    Intro intro;
    if (!intro.Create(inst)) return 1;
    intro.Run();
    return 0;
}
