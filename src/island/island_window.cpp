#include "aether/island/island_window.hpp"
#include "aether/render/glass.hpp"
#include "aether/config/config_manager.hpp"
#include "aether/core/log.hpp"
#include "aether/core/i18n.hpp"
#include "aether/render/context_menu.hpp"
#include "aether/widgets/widget_window.hpp"
#include <ctime>
#include <cmath>
#include <cwchar>
#include <algorithm>
#include <initializer_list>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <d2d1effects.h>
#include "aether/core/monitors.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dxguid.lib")

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

namespace aether::island {

using core::Color;
using core::Rect;
using core::Vec2;

namespace {

constexpr float kPi = 3.14159265358979f;

// Logical (96 dpi) size of the transparent host window. Every visual is drawn inside it.
constexpr float kWindowW = 880.0f;
constexpr float kWindowH = 300.0f;

// iOS-like palette
const Color kOrange{1.00f, 0.62f, 0.04f, 1.0f};   // microphone in use
const Color kGreen {0.19f, 0.82f, 0.35f, 1.0f};   // camera in use
const Color kRingCpu {0.98f, 0.07f, 0.31f, 1.0f}; // Activity-ring red
const Color kRingRam {0.57f, 0.91f, 0.17f, 1.0f}; // Activity-ring green
const Color kRingGpu {0.12f, 0.92f, 0.94f, 1.0f}; // Activity-ring cyan

inline Color White(float a = 1.0f) { return Color{1.0f, 1.0f, 1.0f, a}; }
inline float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

inline D2D1_RECT_F RectF(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

void RgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
    const float d = mx - mn;
    v = mx;
    s = (mx <= 0.0f) ? 0.0f : d / mx;
    if (d <= 1e-6f) { h = 0.0f; return; }
    if (mx == r)      h = std::fmod((g - b) / d, 6.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else              h = (r - g) / d + 4.0f;
    h *= 60.0f;
    if (h < 0.0f) h += 360.0f;
}

void HsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float rr = 0, gg = 0, bb = 0;
    if      (h < 60.0f)  { rr = c; gg = x; }
    else if (h < 120.0f) { rr = x; gg = c; }
    else if (h < 180.0f) { gg = c; bb = x; }
    else if (h < 240.0f) { gg = x; bb = c; }
    else if (h < 300.0f) { rr = x; bb = c; }
    else                 { rr = c; bb = x; }
    r = rr + m; g = gg + m; b = bb + m;
}

// Picks the most "vibrant" dominant hue of a cover, the way Apple tints Now Playing UI.
Color ExtractAccent(const unsigned char* px, int w, int h) {
    struct Bin { float weight{0}, r{0}, g{0}, b{0}; };
    Bin bins[24];
    const int step = std::max(1, std::min(w, h) / 48);
    int samples = 0;
    float avgV = 0.0f;

    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            const unsigned char* p = px + (static_cast<size_t>(y) * w + x) * 4;
            const float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
            float hh, ss, vv;
            RgbToHsv(r, g, b, hh, ss, vv);
            ++samples;
            avgV += vv;
            if (vv < 0.20f || ss < 0.22f) continue;
            const float weight = ss * (0.35f + vv);
            Bin& bin = bins[static_cast<int>(hh / 15.0f) % 24];
            bin.weight += weight;
            bin.r += r * weight; bin.g += g * weight; bin.b += b * weight;
        }
    }

    int best = -1;
    float bestW = 0.0f;
    for (int i = 0; i < 24; ++i) {
        if (bins[i].weight > bestW) { bestW = bins[i].weight; best = i; }
    }

    // Grayscale / near-black covers: stay neutral instead of inventing a colour
    if (best < 0 || samples == 0 || bestW < samples * 0.03f) {
        const float l = samples ? std::clamp(avgV / samples + 0.35f, 0.75f, 0.96f) : 0.92f;
        return Color{l, l, l + 0.02f, 1.0f};
    }

    float r = bins[best].r / bestW, g = bins[best].g / bestW, b = bins[best].b / bestW;
    float hh, ss, vv;
    RgbToHsv(r, g, b, hh, ss, vv);
    ss = std::clamp(ss, 0.45f, 0.95f);
    vv = std::clamp(vv, 0.82f, 1.00f);
    HsvToRgb(hh, ss, vv, r, g, b);
    return Color{r, g, b, 1.0f};
}

std::wstring FormatTime(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    const int total = static_cast<int>(seconds);
    wchar_t buf[24];
    if (total >= 3600) swprintf_s(buf, L"%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    else               swprintf_s(buf, L"%d:%02d", total / 60, total % 60);
    return buf;
}

// Returns the first installed family from `names`, or an empty string.
std::wstring PickFamily(IDWriteFactory* factory, std::initializer_list<const wchar_t*> names) {
    ComPtr<IDWriteFontCollection> collection;
    if (!factory || FAILED(factory->GetSystemFontCollection(&collection, FALSE)) || !collection) return L"";
    for (const wchar_t* n : names) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(collection->FindFamilyName(n, &index, &exists)) && exists) return n;
    }
    return L"";
}

// Fluent / MDL2 icon code points
constexpr wchar_t kIconMusic   = 0xE8D6;
constexpr wchar_t kIconMute    = 0xE74F;
constexpr wchar_t kIconVol1    = 0xE993;
constexpr wchar_t kIconVol2    = 0xE994;
constexpr wchar_t kIconVol3    = 0xE995;

} // namespace

// =============================================================================================
//  Construction / window
// =============================================================================================

IslandWindow::IslandWindow()
    : m_boundsSpring(Rect{300, 14, 124, 34}, animation::SpringConfig{440.0f, 32.0f, 1.0f, 0.03f})
    , m_radiusSpring(17.0f, animation::SpringConfig{440.0f, 32.0f, 1.0f, 0.03f})
    , m_contentAlpha(1.0f, animation::SpringConfig{520.0f, 40.0f, 1.0f, 0.002f})
    , m_trackAnimProgress(1.0f, animation::SpringConfig{380.0f, 30.0f, 1.0f, 0.002f})
    , m_playMix(0.0f, animation::SpringConfig{600.0f, 40.0f, 1.0f, 0.002f})
    , m_scrubThick(0.0f, animation::SpringConfig{700.0f, 44.0f, 1.0f, 0.002f})
    , m_volDisplay(0.5f, animation::SpringConfig{520.0f, 40.0f, 1.0f, 0.002f}) {
    for (auto& s : m_ringSpring)  { s = animation::Spring<float>(0.0f, animation::SpringConfig{170.0f, 22.0f, 1.0f, 0.05f}); }
    for (auto& s : m_pressSpring) { s = animation::Spring<float>(1.0f, animation::SpringConfig{900.0f, 46.0f, 1.0f, 0.002f}); }
}

IslandWindow::~IslandWindow() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

LRESULT CALLBACK IslandWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    IslandWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<IslandWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<IslandWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void IslandWindow::PositionWindow() {
    const int index = config::ConfigManager::Instance().Get().monitorIndex;
    m_monitorApplied = index;
    HMONITOR mon = core::MonitorByIndex(index);
    MONITORINFO mi = {sizeof(MONITORINFO)};
    GetMonitorInfoW(mon, &mi);

    m_dpiScale = core::MonitorScale(mon);
    m_windowWidth = static_cast<uint32_t>(kWindowW * m_dpiScale);
    m_windowHeight = static_cast<uint32_t>(kWindowH * m_dpiScale);

    const int screenW = mi.rcMonitor.right - mi.rcMonitor.left;
    const int x = mi.rcMonitor.left + (screenW - static_cast<int>(m_windowWidth)) / 2;
    const int y = mi.rcMonitor.top;
    if (m_hwnd) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, static_cast<int>(m_windowWidth), static_cast<int>(m_windowHeight),
                     SWP_NOACTIVATE);
    }
}

bool IslandWindow::Create() {
    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"AetherDynamicIslandClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // Work out DPI first so the swap chain is big enough for expanded states at any scale.
    PositionWindow();

    m_hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"Aether Dynamic Island", WS_POPUP,
        0, 0, static_cast<int>(m_windowWidth), static_cast<int>(m_windowHeight),
        nullptr, nullptr, GetModuleHandle(nullptr), this);

    if (!m_hwnd) {
        AETHER_LOG_ERROR("Failed to create Island window");
        return false;
    }

    PositionWindow();

    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, m_windowWidth, m_windowHeight)) {
        AETHER_LOG_ERROR("Failed to initialize DCompLayer for Island");
        return false;
    }

    m_roundedStroke = m_dcomp->CreateRoundedStrokeStyle();
    m_solid = m_dcomp->CreateSolidBrush(Color{1.0f, 1.0f, 1.0f, 1.0f});

    if (auto ctx = m_dcomp->GetContext()) {
        ctx->CreateEffect(CLSID_D2D1Shadow, &m_shadowEffect);
        ctx->CreateEffect(CLSID_D2D1GaussianBlur, &m_blurEffect);
        if (m_blurEffect) {
            m_blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
        }
    }

    CreateFonts();

    UpdateTargetBounds();
    m_boundsSpring.SnapTo(m_boundsSpring.GetTarget());
    m_radiusSpring.SnapTo(m_radiusSpring.GetTarget());
    UpdateWindowRegion(m_boundsSpring.GetValue());

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    AETHER_LOG_INFO("IslandWindow created (" << m_windowWidth << "x" << m_windowHeight << " @ " << m_dpiScale << "x)");
    return true;
}

void IslandWindow::Show(bool show) {
    if (m_hwnd) {
        ShowWindow(m_hwnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
}

void IslandWindow::CreateFonts() {
    auto dwrite = m_dcomp ? m_dcomp->GetDWriteFactory() : nullptr;
    if (!dwrite) return;

    // Segoe UI Variable is Windows 11 only - fall back gracefully on Windows 10.
    std::wstring display = PickFamily(dwrite, {L"Segoe UI Variable Display", L"Segoe UI"});
    std::wstring text    = PickFamily(dwrite, {L"Segoe UI Variable Text", L"Segoe UI"});
    std::wstring small_  = PickFamily(dwrite, {L"Segoe UI Variable Small", L"Segoe UI"});
    std::wstring icons   = PickFamily(dwrite, {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});
    if (display.empty()) display = L"Segoe UI";
    if (text.empty())    text = L"Segoe UI";
    if (small_.empty())  small_ = L"Segoe UI";
    m_hasIconFont = !icons.empty();

    const float s = m_dpiScale;
    auto make = [&](ComPtr<IDWriteTextFormat>& out, const std::wstring& family, DWRITE_FONT_WEIGHT weight,
                    float size, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT para) {
        out.Reset();
        if (family.empty()) return;
        dwrite->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, size * s, L"en-us", &out);
        if (out) {
            out->SetTextAlignment(align);
            out->SetParagraphAlignment(para);
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    };

    constexpr auto kLeft = DWRITE_TEXT_ALIGNMENT_LEADING;
    constexpr auto kCenter = DWRITE_TEXT_ALIGNMENT_CENTER;
    constexpr auto kRight = DWRITE_TEXT_ALIGNMENT_TRAILING;
    constexpr auto kTop = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    constexpr auto kMid = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;

    make(m_fontTitle,      display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 15.0f, kLeft,   kTop);
    make(m_fontArtist,     text,    DWRITE_FONT_WEIGHT_NORMAL,    13.0f, kLeft,   kTop);
    make(m_fontClock,      display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.5f, kCenter, kMid);
    make(m_fontRingValue,  display, DWRITE_FONT_WEIGHT_BOLD,      15.0f, kCenter, kMid);
    make(m_fontCaption,    small_,  DWRITE_FONT_WEIGHT_SEMI_BOLD, 10.5f, kCenter, kMid);
    make(m_fontTimeLeft,   small_,  DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f, kLeft,   kMid);
    make(m_fontTimeRight,  small_,  DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f, kRight,  kMid);
    make(m_fontNotifTitle, display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f, kLeft,   kMid);
    make(m_fontNotifBody,  text,    DWRITE_FONT_WEIGHT_NORMAL,    12.5f, kLeft,   kTop);
    make(m_fontInitial,    display, DWRITE_FONT_WEIGHT_BOLD,      19.0f, kCenter, kMid);
    make(m_fontIcon,       icons,   DWRITE_FONT_WEIGHT_NORMAL,    18.0f, kCenter, kMid);

    if (m_fontNotifTitle) {
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        dwrite->CreateEllipsisTrimmingSign(m_fontNotifTitle.Get(), &ellipsis);
        m_fontNotifTitle->SetTrimming(&trim, ellipsis.Get());
    }
    if (m_fontNotifBody) {
        m_fontNotifBody->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        dwrite->CreateEllipsisTrimmingSign(m_fontNotifBody.Get(), &ellipsis);
        m_fontNotifBody->SetTrimming(&trim, ellipsis.Get());
    }

    RebuildTrackText();
}

bool IslandWindow::BuildTextBlock(const std::wstring& text, IDWriteTextFormat* fmt, TextBlock& out) {
    out.layout.Reset();
    out.width = out.height = 0.0f;
    auto dwrite = m_dcomp ? m_dcomp->GetDWriteFactory() : nullptr;
    if (!dwrite || !fmt || text.empty()) return false;

    if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                        4096.0f * m_dpiScale, 64.0f * m_dpiScale, &out.layout))) {
        out.layout.Reset();
        return false;
    }
    DWRITE_TEXT_METRICS tm{};
    if (SUCCEEDED(out.layout->GetMetrics(&tm))) {
        out.width = tm.widthIncludingTrailingWhitespace;
        out.height = tm.height;
    }
    return true;
}

void IslandWindow::RebuildTrackText() {
    BuildTextBlock(m_track.title, m_fontTitle.Get(), m_titleBlock);
    BuildTextBlock(m_track.artist, m_fontArtist.Get(), m_artistBlock);
}

// =============================================================================================
//  Album art
// =============================================================================================

void IslandWindow::UpdateAlbumArt(const std::vector<uint8_t>& bytes) {
    m_albumArt.Reset();
    m_artW = m_artH = 0.0f;
    m_accentTarget = Color{0.93f, 0.93f, 0.96f, 1.0f};
    if (bytes.empty()) return;

    int w = 0, h = 0, comp = 0;
    unsigned char* decoded = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (!decoded) return;

    auto ctx = m_dcomp ? m_dcomp->GetContext() : nullptr;
    if (ctx && w > 0 && h > 0) {
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (SUCCEEDED(ctx->CreateBitmap(D2D1::SizeU(w, h), decoded, w * 4, props, &m_albumArt))) {
            m_artW = static_cast<float>(w);
            m_artH = static_cast<float>(h);
            m_accentTarget = ExtractAccent(decoded, w, h);
        }
    }
    stbi_image_free(decoded);
}

// =============================================================================================
//  State / layout
// =============================================================================================

int IslandWindow::LayoutSignature() const {
    const bool media = m_track.hasMedia && !m_track.title.empty();
    int sig = static_cast<int>(m_currentMode);
    sig |= (media ? 1 : 0) << 4;
    sig |= (m_peekTimer > 0.0f ? 1 : 0) << 5;
    sig |= (m_micActive ? 1 : 0) << 6;
    sig |= (m_camActive ? 1 : 0) << 7;
    return sig;
}

void IslandWindow::SetGamingSuspended(bool suspended) {
    if (m_isGamingSuspended == suspended) return;
    m_isGamingSuspended = suspended;
    if (m_isGamingSuspended) {
        UpdateTargetBounds();
    } else {
        // Slide back down out of the bezel
        const float w = 124.0f * m_dpiScale;
        const float h = 34.0f * m_dpiScale;
        m_boundsSpring.SnapTo(Rect{m_windowWidth * 0.5f - w * 0.5f, -40.0f * m_dpiScale, w, h});
        m_contentAlpha.SnapTo(0.15f);
        m_peekTimer = 0.0f;
        SetMode(IslandMode::Idle);
        UpdateTargetBounds();
        m_contentAlpha.SetTarget(1.0f);
    }
}

void IslandWindow::PlayIntro() {
    if (config::ConfigManager::Instance().Get().animation.reducedMotion) return;
    const float w = 124.0f * m_dpiScale, h = 34.0f * m_dpiScale;
    m_boundsSpring.SnapTo(Rect{m_windowWidth * 0.5f - w * 0.5f, -48.0f * m_dpiScale, w, h});
    m_contentAlpha.SnapTo(0.0f);
    UpdateTargetBounds();
    m_contentAlpha.SetTarget(1.0f);
}

void IslandWindow::SetMode(IslandMode mode) {
    if (m_currentMode == mode) return;
    m_currentMode = mode;
    m_peekTimer = 0.0f;
    m_marqueeTime = 0.0f;
    if (mode == IslandMode::SystemExpanded) {
        for (auto& r : m_ringSpring) r.SnapTo(0.0f);
    }
    UpdateTargetBounds();
    m_contentAlpha.SnapTo(0.20f);
    m_contentAlpha.SetTarget(1.0f);
}

void IslandWindow::UpdateTargetBounds() {
    const auto& cfg = config::ConfigManager::Instance().Get();
    const float s = m_dpiScale;
    const float topY = cfg.island.topOffset * s;
    const float centerX = m_windowWidth * 0.5f;
    const bool media = m_track.hasMedia && !m_track.title.empty();
    const bool dots = m_micActive || m_camActive;

    float w = 124.0f, h = 34.0f, radius = 17.0f;

    if (m_isGamingSuspended) {
        const float gw = 60.0f * s, gh = 10.0f * s;
        m_boundsSpring.SetTarget(Rect{centerX - gw * 0.5f, -60.0f * s, gw, gh});
        m_radiusSpring.SetTarget(gh * 0.5f);
        m_contentAlpha.SetTarget(0.0f);
        return;
    }

    switch (m_currentMode) {
        case IslandMode::Idle:
            if (media && m_peekTimer > 0.0f) {
                w = 348.0f; h = 64.0f; radius = 30.0f;
            } else if (media) {
                w = 206.0f + (dots ? 22.0f : 0.0f); h = 34.0f; radius = 17.0f;
            } else {
                w = (cfg.island.showClockIdle ? 124.0f : 112.0f) + (dots ? 36.0f : 0.0f);
                h = 34.0f; radius = 17.0f;
            }
            break;
        case IslandMode::MediaExpanded:  w = 404.0f; h = 178.0f; radius = 38.0f; break;
        case IslandMode::SystemExpanded: w = 392.0f; h = 128.0f; radius = 34.0f; break;
        case IslandMode::VolumeHUD:      w = 236.0f; h = 42.0f;  radius = 21.0f; break;
        case IslandMode::Notification:   w = 384.0f; h = 80.0f;  radius = 32.0f; break;
    }

    const Rect target{centerX - w * s * 0.5f, topY, w * s, h * s};
    if (cfg.animation.reducedMotion) {
        m_boundsSpring.SnapTo(target);
        m_radiusSpring.SnapTo(radius * s);
        m_contentAlpha.SnapTo(1.0f);
    } else {
        m_boundsSpring.SetTarget(target);
        m_radiusSpring.SetTarget(radius * s);
    }
}

void IslandWindow::UpdateWindowRegion(const Rect& bounds) {
    if (!m_hwnd) return;
    if (std::abs(bounds.x - m_lastRegionBounds.x) < 0.5f && std::abs(bounds.y - m_lastRegionBounds.y) < 0.5f &&
        std::abs(bounds.width - m_lastRegionBounds.width) < 0.5f &&
        std::abs(bounds.height - m_lastRegionBounds.height) < 0.5f) {
        return;
    }
    m_lastRegionBounds = bounds;

    // The window region both clips drawing and decides which clicks the island swallows (a foreign
    // window cannot be "click-through" via HTTRANSPARENT), so keep the shadow padding tight when the
    // island is small and only grow it for the big expanded cards.
    const bool big = bounds.height > 60.0f * m_dpiScale;
    const int padX = static_cast<int>((big ? 38.0f : 18.0f) * m_dpiScale);
    const int padTop = static_cast<int>(4.0f * m_dpiScale);
    const int padBottom = static_cast<int>((big ? 46.0f : 22.0f) * m_dpiScale);
    const int x1 = static_cast<int>(bounds.Left()) - padX;
    const int y1 = static_cast<int>(bounds.Top()) - padTop;
    const int x2 = static_cast<int>(bounds.Right()) + padX;
    const int y2 = static_cast<int>(bounds.Bottom()) + padBottom;

    HRGN rgn = (bounds.Bottom() <= 0.0f || x2 <= x1 || y2 <= y1) ? CreateRectRgn(0, 0, 0, 0)
                                                                : CreateRectRgn(x1, y1, x2, y2);
    if (rgn) SetWindowRgn(m_hwnd, rgn, FALSE);
}

void IslandWindow::PushNotification(const NotificationData& notif) {
    m_activeNotification = notif;
    SetMode(IslandMode::Notification);
    // Same mode again (back-to-back notifications): replay the content transition
    m_contentAlpha.SnapTo(0.20f);
    m_contentAlpha.SetTarget(1.0f);
}

void IslandWindow::TriggerVolumeHUD(float volume) {
    volume = Clamp01(volume);
    media::MediaService::Instance().SetVolume(volume);
    m_volumeHudTimer = 2.0f;
    m_volDisplay.SetTarget(volume);
    SetMode(IslandMode::VolumeHUD);
}

// =============================================================================================
//  Per-frame update
// =============================================================================================

bool IslandWindow::IsSpringDriven() const {
    if (m_isGamingSuspended) {
        return !m_boundsSpring.IsResting() || !m_contentAlpha.IsResting();
    }
    if (!m_boundsSpring.IsResting() || !m_radiusSpring.IsResting() || !m_contentAlpha.IsResting() ||
        !m_trackAnimProgress.IsResting() || !m_playMix.IsResting() || !m_scrubThick.IsResting() ||
        !m_volDisplay.IsResting()) {
        return true;
    }
    for (const auto& r : m_ringSpring)  if (!r.IsResting()) return true;
    for (const auto& p : m_pressSpring) if (!p.IsResting()) return true;
    if (m_isHovered || m_dragging != Control::None) return true;
    if (m_currentMode == IslandMode::VolumeHUD || m_currentMode == IslandMode::Notification ||
        m_currentMode == IslandMode::SystemExpanded) {
        return true; // timers / live metrics
    }
    return false;
}

bool IslandWindow::IsResting() const {
    if (IsSpringDriven()) return false;
    if (m_isGamingSuspended) return true;
    if (m_peekTimer > 0.0f || m_marqueeActive) return false;

    const auto track = media::MediaService::Instance().GetTrackLite();
    if (track.hasMedia && track.status == media::PlaybackStatus::Playing) return false; // visualizer
    for (float level : m_barLevels) if (level > 0.02f) return false;                    // bars settling
    if (track.hasMedia && !track.title.empty() && track.title != m_lastTrackTitle) return false;
    return true;
}

void IslandWindow::Update(float dt) {
    const auto& cfg = config::ConfigManager::Instance().Get();
    const float frameDt = std::min(dt, 0.1f);
    const float sdt = dt * std::max(0.1f, cfg.animation.timeScale);

    // Live options: screenshot visibility and target monitor
    const int affinity = cfg.island.hideFromScreenshots ? 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */ : 0 /* WDA_NONE */;
    if (m_hwnd && affinity != m_affinityApplied) {
        SetWindowDisplayAffinity(m_hwnd, affinity);
        m_affinityApplied = affinity;
    }
    if (m_hwnd && cfg.monitorIndex != m_monitorApplied) {
        const uint32_t oldW = m_windowWidth, oldH = m_windowHeight;
        const float oldScale = m_dpiScale;
        PositionWindow();
        if (m_dcomp && (m_windowWidth != oldW || m_windowHeight != oldH)) m_dcomp->Resize(m_windowWidth, m_windowHeight);
        if (m_dpiScale != oldScale) CreateFonts();
        UpdateTargetBounds();
        m_boundsSpring.SnapTo(m_boundsSpring.GetTarget());
        m_radiusSpring.SnapTo(m_radiusSpring.GetTarget());
        m_lastRegionBounds = Rect{-1.0f, -1.0f, -1.0f, -1.0f};
    }

    // ---- data snapshot (UI thread only ever sees a cheap copy without the cover bytes) -------
    auto& media = media::MediaService::Instance();
    m_track = media.GetTrackLite();
    m_micActive = media.IsMicrophoneActive();
    m_camActive = media.IsCameraActive();

    const bool hasMedia = m_track.hasMedia && !m_track.title.empty();
    const bool playing = hasMedia && m_track.status == media::PlaybackStatus::Playing;

    if (m_track.artVersion != m_artVersion) {
        std::vector<uint8_t> bytes;
        uint64_t version = m_artVersion;
        if (media.FetchAlbumArt(version, bytes)) {
            m_artVersion = version;
            UpdateAlbumArt(bytes);
            if (!bytes.empty()) {
                m_trackAnimProgress.SnapTo(0.0f);
                m_trackAnimProgress.SetTarget(1.0f);
            }
        } else {
            m_artVersion = m_track.artVersion;
        }
    }

    if (hasMedia) {
        const bool titleChanged = m_track.title != m_lastTrackTitle;
        if (titleChanged || m_track.artist != m_lastTrackArtist) {
            m_lastTrackTitle = m_track.title;
            m_lastTrackArtist = m_track.artist;
            RebuildTrackText();
            m_marqueeTime = 0.0f;
        }
        if (titleChanged) {
            m_trackAnimProgress.SnapTo(0.0f);
            m_trackAnimProgress.SetTarget(1.0f);
            // Like iOS: a freshly started song briefly "peeks" with its title before tucking away
            if (m_currentMode == IslandMode::Idle && !m_isGamingSuspended && playing && !cfg.animation.reducedMotion) {
                m_peekTimer = 3.4f;
                m_contentAlpha.SnapTo(0.20f);
                m_contentAlpha.SetTarget(1.0f);
            }
        }
    } else if (!m_lastTrackTitle.empty()) {
        m_lastTrackTitle.clear();
        m_lastTrackArtist.clear();
        m_peekTimer = 0.0f;
        m_titleBlock = TextBlock{};
        m_artistBlock = TextBlock{};
    }

    // Layout changes caused by data (media appears, privacy dot lights up, peek starts/ends)
    if (m_peekTimer > 0.0f) {
        m_peekTimer -= frameDt;
        if (m_peekTimer <= 0.0f) {
            m_peekTimer = 0.0f;
            m_contentAlpha.SnapTo(0.35f);
            m_contentAlpha.SetTarget(1.0f);
        }
    }
    const int sig = LayoutSignature();
    if (sig != m_layoutSig) {
        m_layoutSig = sig;
        UpdateTargetBounds();
    }

    // ---- springs -------------------------------------------------------------------------------
    m_boundsSpring.Update(sdt);
    m_radiusSpring.Update(sdt);
    m_contentAlpha.Update(sdt);
    m_trackAnimProgress.Update(sdt);

    m_playMix.SetTarget(playing ? 1.0f : 0.0f);
    m_playMix.Update(sdt);

    const bool scrubActive = (m_currentMode == IslandMode::MediaExpanded) &&
                             (m_dragging == Control::Scrub || m_scrubBar.Contains(m_mousePos));
    m_scrubThick.SetTarget(scrubActive ? 1.0f : 0.0f);
    m_scrubThick.Update(sdt);

    for (int i = 0; i < 3; ++i) {
        const Control c = static_cast<Control>(i + 1); // Prev, Play, Next
        m_pressSpring[i].SetTarget(m_pressed == c ? 0.84f : 1.0f);
        m_pressSpring[i].Update(sdt);
    }

    if (m_currentMode == IslandMode::SystemExpanded) {
        const auto metrics = system::MetricsCollector::Instance().GetMetrics();
        m_ringSpring[0].SetTarget(std::clamp(static_cast<float>(metrics.cpuUsagePercent), 0.0f, 100.0f));
        m_ringSpring[1].SetTarget(std::clamp(static_cast<float>(metrics.ramUsagePercent), 0.0f, 100.0f));
        m_ringSpring[2].SetTarget(std::clamp(static_cast<float>(metrics.gpuUsagePercent), 0.0f, 100.0f));
    }
    for (auto& r : m_ringSpring) r.Update(sdt);

    if (m_currentMode == IslandMode::VolumeHUD && m_dragging != Control::Volume) {
        m_volDisplay.SetTarget(media.GetVolume());
    }
    m_volDisplay.Update(sdt);

    UpdateWindowRegion(m_boundsSpring.GetValue());

    // ---- ambient animation ---------------------------------------------------------------------
    {
        const float k = 1.0f - std::exp(-frameDt * 5.0f);
        m_accent = Color::Lerp(m_accent, m_accentTarget, std::clamp(k, 0.0f, 1.0f));

        // Visualizer is driven by the *real* output level of the default audio device
        const float peak = media.GetAudioPeak();
        const float amp = playing ? std::clamp(std::sqrt(peak) * 1.45f, 0.0f, 1.0f) : 0.0f;
        for (int i = 0; i < 6; ++i) {
            m_barPhase[i] += frameDt * (6.5f + i * 1.9f);
            const float wave = 0.5f + 0.5f * std::sin(m_barPhase[i]) * std::sin(m_barPhase[i] * 0.37f + i);
            const float target = playing ? (0.14f + amp * (0.30f + 0.70f * wave)) : 0.0f;
            const float rate = target > m_barLevels[i] ? 30.0f : 11.0f;
            m_barLevels[i] += (target - m_barLevels[i]) * (1.0f - std::exp(-frameDt * rate));
            m_barLevels[i] = std::clamp(m_barLevels[i], 0.0f, 1.0f);
        }
        m_marqueeTime += frameDt;
    }

    // ---- auto-dismiss timers -------------------------------------------------------------------
    if (m_currentMode == IslandMode::VolumeHUD) {
        if (m_dragging == Control::Volume) m_volumeHudTimer = 2.0f;
        m_volumeHudTimer -= frameDt;
        if (m_volumeHudTimer <= 0.0f) SetMode(IslandMode::Idle);
    } else if (m_currentMode == IslandMode::Notification) {
        m_activeNotification.timeRemaining -= frameDt;
        if (m_activeNotification.timeRemaining <= 0.0) SetMode(IslandMode::Idle);
    }

    // ---- hover expand / collapse with hysteresis -------------------------------------------------
    if (cfg.island.expandOnHover && !m_isPinned) {
        if (m_isHovered) {
            m_unhoverTimer = 0.0f;
            if (m_currentMode == IslandMode::Idle) {
                m_hoverTimer += frameDt;
                if (m_hoverTimer >= cfg.island.hoverDelayMs / 1000.0f) {
                    SetMode(hasMedia ? IslandMode::MediaExpanded : IslandMode::SystemExpanded);
                }
            }
        } else {
            m_hoverTimer = 0.0f;
            if (m_currentMode == IslandMode::MediaExpanded || m_currentMode == IslandMode::SystemExpanded) {
                m_unhoverTimer += frameDt;
                if (m_unhoverTimer >= 0.28f && m_dragging == Control::None) {
                    SetMode(IslandMode::Idle);
                    m_unhoverTimer = 0.0f;
                }
            }
        }
    }
}

// =============================================================================================
//  Rendering pipeline
// =============================================================================================

ID2D1SolidColorBrush* IslandWindow::Br(const Color& c) {
    // One shared brush, recolored per draw call (D2D snapshots the brush state when the call is issued)
    m_solid->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
    return m_solid.Get();
}

ComPtr<ID2D1CommandList> IslandWindow::RecordShadow(ID2D1DeviceContext* ctx, ID2D1Geometry* geo) {
    ComPtr<ID2D1CommandList> cl;
    if (FAILED(ctx->CreateCommandList(&cl))) return nullptr;
    ctx->SetTarget(cl.Get());
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx->FillGeometry(geo, Br(Color{0.0f, 0.0f, 0.0f, 1.0f}));
    const HRESULT hr = ctx->EndDraw();
    ctx->SetTarget(m_dcomp->GetTargetBitmap());
    cl->Close();
    return SUCCEEDED(hr) ? cl : nullptr;
}

ComPtr<ID2D1CommandList> IslandWindow::RecordContent(ID2D1DeviceContext* ctx, const Rect& bounds) {
    ComPtr<ID2D1CommandList> cl;
    if (FAILED(ctx->CreateCommandList(&cl))) return nullptr;
    ctx->SetTarget(cl.Get());
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    DrawContent(ctx, bounds);
    const HRESULT hr = ctx->EndDraw();
    ctx->SetTarget(m_dcomp->GetTargetBitmap());
    cl->Close();
    return SUCCEEDED(hr) ? cl : nullptr;
}

void IslandWindow::DrawContent(ID2D1DeviceContext* ctx, const Rect& bounds) {
    m_marqueeActive = false;
    switch (m_currentMode) {
        case IslandMode::Idle:
            if (m_peekTimer > 0.0f && m_track.hasMedia && !m_track.title.empty()) DrawPeek(ctx, bounds);
            else DrawIdle(ctx, bounds);
            break;
        case IslandMode::MediaExpanded:  DrawMedia(ctx, bounds); break;
        case IslandMode::SystemExpanded: DrawSystem(ctx, bounds); break;
        case IslandMode::VolumeHUD:      DrawVolume(ctx, bounds); break;
        case IslandMode::Notification:   DrawNotification(ctx, bounds); break;
    }
}

void IslandWindow::Render() {
    if (!m_dcomp || !m_solid) return;
    auto ctx = m_dcomp->GetContext();
    if (!ctx) return;

    const auto& app = config::ConfigManager::Instance().Get().appearance;
    const Rect b = m_boundsSpring.GetValue();
    const float radius = m_radiusSpring.GetValue();
    const float s = m_dpiScale;
    const float alpha = Clamp01(m_contentAlpha.GetValue());

    ComPtr<ID2D1PathGeometry> geo;
    if (b.width > 2.0f && b.height > 2.0f && b.Bottom() > 0.0f) {
        geo = m_dcomp->CreateSquircleGeometry(b, radius);
    }

    // Off-screen passes must happen before the main BeginDraw
    ComPtr<ID2D1CommandList> shadowCl, contentCl;
    render::CachedGlow shadowCache;
    const bool expandedShadow = m_currentMode != IslandMode::Idle && m_currentMode != IslandMode::VolumeHUD;
    if (geo && app.enableShadow && m_shadowEffect) {
        // once the island's shape rests, its shadow is kept as a bitmap instead of being re-blurred every frame
        shadowCache = render::StableGlow(m_dcomp.get(), {geo.Get()},
            {{(expandedShadow ? 11.0f : 7.0f) * s, 0.0f, (expandedShadow ? 8.0f : 4.0f) * s,
              Color{0.0f, 0.0f, 0.0f, (expandedShadow ? 0.62f : 0.45f) * app.shadowIntensity}}}, 40.0f * s);
        if (!shadowCache) shadowCl = RecordShadow(ctx, geo.Get());
    }
    const bool blurPass = geo && alpha < 0.985f && m_blurEffect && !m_isGamingSuspended;
    if (blurPass) contentCl = RecordContent(ctx, b);

    auto dc = m_dcomp->BeginDraw();
    if (!dc) return;

    if (!geo) {
        m_dcomp->EndDraw();
        return;
    }

    // 1. Soft contact shadow
    if (shadowCache) {
        render::DrawCachedGlow(dc, shadowCache);
    } else if (shadowCl) {
        const bool expanded = m_currentMode != IslandMode::Idle && m_currentMode != IslandMode::VolumeHUD;
        m_shadowEffect->SetInput(0, shadowCl.Get());
        m_shadowEffect->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, (expanded ? 11.0f : 7.0f) * s);
        m_shadowEffect->SetValue(D2D1_SHADOW_PROP_COLOR,
                                 D2D1::Vector4F(0.0f, 0.0f, 0.0f, (expanded ? 0.62f : 0.45f) * app.shadowIntensity));
        ComPtr<ID2D1Image> out;
        m_shadowEffect->GetOutput(&out);
        dc->DrawImage(out.Get(), D2D1::Point2F(0.0f, (expanded ? 8.0f : 4.0f) * s));
    }

    // 2. Body: true black, just like the hardware cut-out it imitates
    dc->FillGeometry(geo.Get(), Br(Color{0.0f, 0.0f, 0.0f, 1.0f}));

    // 3. Ambient glow in the colour of the album cover (very restrained)
    const bool mediaView = (m_currentMode == IslandMode::MediaExpanded) ||
                           (m_currentMode == IslandMode::Idle && m_peekTimer > 0.0f);
    if (mediaView && m_track.hasMedia) {
        const bool big = m_currentMode == IslandMode::MediaExpanded;
        const D2D1_POINT_2F c = D2D1::Point2F(b.Left() + (big ? 48.0f : 30.0f) * s, b.Top() + (big ? 46.0f : 32.0f) * s);
        auto glow = m_dcomp->CreateRadialGradientBrush(
            Color{m_accent.r, m_accent.g, m_accent.b, (big ? 0.30f : 0.26f) * (0.4f + 0.6f * alpha)},
            Color{m_accent.r, m_accent.g, m_accent.b, 0.0f}, c, (big ? 230.0f : 150.0f) * s, (big ? 150.0f : 100.0f) * s);
        if (glow) dc->FillGeometry(geo.Get(), glow.Get());
    }

    // 4. Content - crossfades with a gaussian blur while the shape morphs (the iOS signature)
    if (contentCl) {
        m_blurEffect->SetInput(0, contentCl.Get());
        m_blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, (1.0f - alpha) * 9.0f * s);
        ComPtr<ID2D1Image> out;
        m_blurEffect->GetOutput(&out);

        D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(
            D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
            alpha, nullptr, D2D1_LAYER_OPTIONS1_NONE);
        dc->PushLayer(&lp, nullptr);
        const float sc = 0.94f + 0.06f * alpha;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(b.Center().x, b.Center().y)));
        dc->DrawImage(out.Get());
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->PopLayer();
    } else if (!m_isGamingSuspended) {
        D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(
            D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
            1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE);
        dc->PushLayer(&lp, nullptr); // content never leaks outside the (animated) shape
        DrawContent(dc, b);
        dc->PopLayer();
    }

    // 5. Hairline rim light so the black shape still reads on dark wallpapers
    auto rim = m_dcomp->CreateLinearGradientBrush(
        Color{1.0f, 1.0f, 1.0f, 0.16f}, Color{1.0f, 1.0f, 1.0f, 0.03f},
        D2D1::Point2F(b.Center().x, b.Top()), D2D1::Point2F(b.Center().x, b.Bottom()));
    if (rim) dc->DrawGeometry(geo.Get(), rim.Get(), 1.0f * s);

    // 6. Accent pulse on track change
    const float pulse = 1.0f - Clamp01(m_trackAnimProgress.GetValue());
    if (pulse > 0.01f && m_track.hasMedia) {
        dc->DrawGeometry(geo.Get(), Br(Color{m_accent.r, m_accent.g, m_accent.b, pulse * 0.55f}), 1.6f * s);
    }

    m_dcomp->EndDraw();
}

// =============================================================================================
//  Drawing helpers
// =============================================================================================

void IslandWindow::DrawLabel(ID2D1DeviceContext* ctx, const std::wstring& text, IDWriteTextFormat* fmt,
                             const D2D1_RECT_F& rect, const Color& color) {
    if (!fmt || text.empty()) return;
    ctx->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fmt, &rect, Br(color));
}

void IslandWindow::DrawGlyph(ID2D1DeviceContext* ctx, wchar_t glyph, const D2D1_RECT_F& rect, const Color& color) {
    if (!m_hasIconFont || !m_fontIcon) return;
    wchar_t buf[2] = {glyph, 0};
    ctx->DrawText(buf, 1, m_fontIcon.Get(), &rect, Br(color));
}

// Long titles scroll horizontally (with soft edge fades), short ones just sit still.
void IslandWindow::DrawMarquee(ID2D1DeviceContext* ctx, const TextBlock& block, float left, float top,
                               float availWidth, const Color& color) {
    if (!block.layout || availWidth <= 4.0f) return;
    const float s = m_dpiScale;

    if (block.width <= availWidth + 0.5f) {
        ctx->DrawTextLayout(D2D1::Point2F(left, top), block.layout.Get(), Br(color));
        return;
    }

    m_marqueeActive = true;
    const float gap = 42.0f * s;
    const float travel = block.width + gap;
    const float speed = 34.0f * s;
    const float pause = 1.8f;
    const float cycle = pause + travel / speed;
    const float t = std::fmod(m_marqueeTime, cycle);
    const float offset = t < pause ? 0.0f : (t - pause) * speed;

    const float fade = std::min(18.0f * s, availWidth * 0.2f);
    const float leftAlpha = 1.0f - Clamp01(offset / fade);
    D2D1_GRADIENT_STOP stops[4] = {
        {0.0f, D2D1::ColorF(0, 0, 0, leftAlpha)},
        {fade / availWidth, D2D1::ColorF(0, 0, 0, 1.0f)},
        {1.0f - fade / availWidth, D2D1::ColorF(0, 0, 0, 1.0f)},
        {1.0f, D2D1::ColorF(0, 0, 0, 0.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> coll;
    ComPtr<ID2D1LinearGradientBrush> mask;
    if (SUCCEEDED(ctx->CreateGradientStopCollection(stops, 4, &coll)) &&
        SUCCEEDED(ctx->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(left, 0.0f), D2D1::Point2F(left + availWidth, 0.0f)),
            coll.Get(), &mask))) {
        D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(
            D2D1::RectF(left, top - 2.0f * s, left + availWidth, top + block.height + 2.0f * s), nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f, mask.Get(), D2D1_LAYER_OPTIONS1_NONE);
        ctx->PushLayer(&lp, nullptr);
        ctx->DrawTextLayout(D2D1::Point2F(left - offset, top), block.layout.Get(), Br(color));
        ctx->DrawTextLayout(D2D1::Point2F(left - offset + travel, top), block.layout.Get(), Br(color));
        ctx->PopLayer();
    } else {
        ctx->DrawTextLayout(D2D1::Point2F(left, top), block.layout.Get(), Br(color));
    }
}

void IslandWindow::DrawArt(ID2D1DeviceContext* ctx, const Rect& rect, float cornerRadius, float scale, float alpha) {
    auto shape = m_dcomp->CreateSquircleGeometry(rect, cornerRadius);
    if (!shape) return;
    const float s = m_dpiScale;

    D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(
        D2D1::InfiniteRect(), shape.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
        1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE);
    ctx->PushLayer(&lp, nullptr);

    if (m_albumArt && m_artW > 0.0f && m_artH > 0.0f) {
        // Centre-crop to a square so wide thumbnails (YouTube) are not squashed
        const float side = std::min(m_artW, m_artH);
        const D2D1_RECT_F src = RectF((m_artW - side) * 0.5f, (m_artH - side) * 0.5f,
                                      (m_artW + side) * 0.5f, (m_artH + side) * 0.5f);
        const float sz = rect.width * scale;
        const float cx = rect.Center().x, cy = rect.Center().y;
        const D2D1_RECT_F dst = RectF(cx - sz * 0.5f, cy - sz * 0.5f, cx + sz * 0.5f, cy + sz * 0.5f);
        ctx->DrawBitmap(m_albumArt.Get(), &dst, alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src, nullptr);
    } else {
        auto bg = m_dcomp->CreateLinearGradientBrush(
            Color{0.27f, 0.27f, 0.30f, 1.0f}, Color{0.12f, 0.12f, 0.14f, 1.0f},
            D2D1::Point2F(rect.Left(), rect.Top()), D2D1::Point2F(rect.Right(), rect.Bottom()));
        if (bg) ctx->FillRectangle(RectF(rect.Left(), rect.Top(), rect.Right(), rect.Bottom()), bg.Get());
        DrawGlyph(ctx, kIconMusic, RectF(rect.Left(), rect.Top(), rect.Right(), rect.Bottom()), White(0.55f));
    }
    ctx->PopLayer();

    ctx->DrawGeometry(shape.Get(), Br(White(0.14f)), 1.0f * s);
}

// Reactive bars: height follows the real output level of the sound card
void IslandWindow::DrawVisualizer(ID2D1DeviceContext* ctx, float rightX, float centerY, float maxHeight,
                                  int bars, const Color& color) {
    if (!config::ConfigManager::Instance().Get().island.showAudioVisualizer) return;
    const float s = m_dpiScale;
    const float bw = 3.0f * s;
    const float gap = 2.4f * s;
    const float total = bars * bw + (bars - 1) * gap;
    const float left = rightX - total;
    for (int i = 0; i < bars; ++i) {
        const float h = bw + (maxHeight - bw) * m_barLevels[i];
        const float x = left + i * (bw + gap);
        ctx->FillRoundedRectangle(
            D2D1::RoundedRect(RectF(x, centerY - h * 0.5f, x + bw, centerY + h * 0.5f), bw * 0.5f, bw * 0.5f),
            Br(color));
    }
}

// Orange (microphone) / green (camera) dots, exactly like the iOS status indicators
void IslandWindow::DrawPrivacyDots(ID2D1DeviceContext* ctx, float rightX, float centerY) {
    const float s = m_dpiScale;
    float x = rightX;
    auto dot = [&](const Color& c) {
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, centerY), 7.0f * s, 7.0f * s), Br(Color{c.r, c.g, c.b, 0.22f}));
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, centerY), 3.6f * s, 3.6f * s), Br(c));
        x -= 15.0f * s;
    };
    if (m_camActive) dot(kGreen);
    if (m_micActive) dot(kOrange);
}

void IslandWindow::FillTriangle(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& a, const D2D1_POINT_2F& b,
                                const D2D1_POINT_2F& c, const Color& color, float rounding) {
    ComPtr<ID2D1PathGeometry> tri;
    auto factory = m_dcomp->GetD2DFactory();
    if (!factory || FAILED(factory->CreatePathGeometry(&tri))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(tri->Open(&sink))) return;
    sink->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(b);
    sink->AddLine(c);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    ctx->FillGeometry(tri.Get(), Br(color));
    // A round-joined outline of the same colour softens the corners like SF Symbols do
    ctx->DrawGeometry(tri.Get(), Br(color), rounding, m_roundedStroke.Get());
}

void IslandWindow::DrawSkip(ID2D1DeviceContext* ctx, const Vec2& c, float size, bool forward, float alpha) {
    // Two triangles side by side (SF Symbol "forward.fill" / "backward.fill")
    const float h = size * 0.64f;
    const float w = size * 0.50f;
    const float dir = forward ? 1.0f : -1.0f;
    const float stroke = size * 0.12f;
    const Color col = White(alpha);
    for (int i = 0; i < 2; ++i) {
        const float xa = c.x - dir * w + dir * w * static_cast<float>(i);
        FillTriangle(ctx,
                     D2D1::Point2F(xa, c.y - h * 0.5f),
                     D2D1::Point2F(xa + dir * w, c.y),
                     D2D1::Point2F(xa, c.y + h * 0.5f), col, stroke);
    }
}

// Play <-> pause crossfade with scale, driven by a spring (`mix`: 0 = play, 1 = pause)
void IslandWindow::DrawPlayPause(ID2D1DeviceContext* ctx, const Vec2& c, float size, float mix, float alpha) {
    mix = Clamp01(mix);
    const float playA = (1.0f - mix) * alpha;
    const float pauseA = mix * alpha;

    if (playA > 0.01f) {
        const float k = 0.75f + 0.25f * (1.0f - mix);
        const float sz = size * k;
        const float ox = sz * 0.06f; // optical centring of the triangle
        FillTriangle(ctx,
                     D2D1::Point2F(c.x - sz * 0.34f + ox, c.y - sz * 0.50f),
                     D2D1::Point2F(c.x + sz * 0.50f + ox, c.y),
                     D2D1::Point2F(c.x - sz * 0.34f + ox, c.y + sz * 0.50f), White(playA), sz * 0.14f);
    }
    if (pauseA > 0.01f) {
        const float k = 0.75f + 0.25f * mix;
        const float sz = size * k;
        const float bw = sz * 0.27f, gap = sz * 0.20f, bh = sz * 0.96f;
        const float r = bw * 0.3f;
        ctx->FillRoundedRectangle(D2D1::RoundedRect(RectF(c.x - gap * 0.5f - bw, c.y - bh * 0.5f, c.x - gap * 0.5f, c.y + bh * 0.5f), r, r), Br(White(pauseA)));
        ctx->FillRoundedRectangle(D2D1::RoundedRect(RectF(c.x + gap * 0.5f, c.y - bh * 0.5f, c.x + gap * 0.5f + bw, c.y + bh * 0.5f), r, r), Br(White(pauseA)));
    }
}

// =============================================================================================
//  Views
// =============================================================================================

void IslandWindow::DrawIdle(ID2D1DeviceContext* ctx, const Rect& b) {
    const float s = m_dpiScale;
    const float cy = b.Center().y;
    const bool media = m_track.hasMedia && !m_track.title.empty();
    const int dotCount = (m_micActive ? 1 : 0) + (m_camActive ? 1 : 0);
    const float dotsWidth = dotCount > 0 ? (dotCount * 15.0f + 6.0f) * s : 0.0f;

    if (media) {
        const float tp = Clamp01(m_trackAnimProgress.GetValue());
        const float art = 22.0f * s;
        DrawArt(ctx, Rect{b.Left() + 8.0f * s, cy - art * 0.5f, art, art}, 7.0f * s, 0.80f + 0.20f * tp, tp);

        float right = b.Right() - 16.0f * s;
        if (dotCount > 0) {
            DrawPrivacyDots(ctx, right - 4.0f * s, cy);
            right -= dotsWidth;
        }
        const bool playing = m_track.status == media::PlaybackStatus::Playing;
        DrawVisualizer(ctx, right, cy, 16.0f * s, 5, playing ? m_accent : Color{1.0f, 1.0f, 1.0f, 0.45f});
        return;
    }

    if (config::ConfigManager::Instance().Get().island.showClockIdle) {
        std::time_t t = std::time(nullptr);
        std::tm now{};
        localtime_s(&now, &t);
        wchar_t buf[16];
        swprintf_s(buf, L"%02d:%02d", now.tm_hour, now.tm_min);
        DrawLabel(ctx, buf, m_fontClock.Get(), RectF(b.Left(), b.Top(), b.Right() - dotsWidth, b.Bottom()), White(0.92f));
    }
    if (dotCount > 0) DrawPrivacyDots(ctx, b.Right() - 18.0f * s, cy);
}

// The short "now playing" banner shown when a new song starts
void IslandWindow::DrawPeek(ID2D1DeviceContext* ctx, const Rect& b) {
    const float s = m_dpiScale;
    const float cy = b.Center().y;
    const float tp = Clamp01(m_trackAnimProgress.GetValue());

    const float art = 42.0f * s;
    DrawArt(ctx, Rect{b.Left() + 12.0f * s, cy - art * 0.5f, art, art}, 11.0f * s, 0.82f + 0.18f * tp, tp);

    const bool playing = m_track.status == media::PlaybackStatus::Playing;
    const float barsRight = b.Right() - 20.0f * s;
    DrawVisualizer(ctx, barsRight, cy, 24.0f * s, 5, playing ? m_accent : Color{1.0f, 1.0f, 1.0f, 0.45f});

    const float textLeft = b.Left() + 12.0f * s + art + 12.0f * s;
    const float textRight = barsRight - 24.6f * s - 14.0f * s;
    const float slide = (1.0f - tp) * 6.0f * s;
    const float totalH = m_titleBlock.height + m_artistBlock.height + 1.0f * s;
    const float top = cy - totalH * 0.5f + slide;
    DrawMarquee(ctx, m_titleBlock, textLeft, top, textRight - textLeft, White(tp));
    DrawMarquee(ctx, m_artistBlock, textLeft, top + m_titleBlock.height + 1.0f * s, textRight - textLeft, White(0.60f * tp));
}

void IslandWindow::DrawMedia(ID2D1DeviceContext* ctx, const Rect& b) {
    const float s = m_dpiScale;
    const float pad = 22.0f * s;
    const float tp = Clamp01(m_trackAnimProgress.GetValue());
    const bool playing = m_track.status == media::PlaybackStatus::Playing;

    // --- header: cover, title / artist, level bars -------------------------------------------
    const float art = 58.0f * s;
    const Rect artRect{b.Left() + pad, b.Top() + 20.0f * s, art, art};
    DrawArt(ctx, artRect, 15.0f * s, 0.86f + 0.14f * tp, tp);

    const float barsRight = b.Right() - pad;
    DrawVisualizer(ctx, barsRight, artRect.Center().y, 26.0f * s, 5, playing ? m_accent : Color{1.0f, 1.0f, 1.0f, 0.45f});

    const float textLeft = artRect.Right() + 14.0f * s;
    const float textRight = barsRight - 24.6f * s - 14.0f * s;
    const float slide = (1.0f - tp) * 6.0f * s;
    const float totalH = m_titleBlock.height + m_artistBlock.height + 1.0f * s;
    const float top = artRect.Center().y - totalH * 0.5f + slide;
    DrawMarquee(ctx, m_titleBlock, textLeft, top, textRight - textLeft, White(tp));
    DrawMarquee(ctx, m_artistBlock, textLeft, top + m_titleBlock.height + 1.0f * s, textRight - textLeft, White(0.60f * tp));

    // --- scrubber ------------------------------------------------------------------------------
    const float barX = b.Left() + pad;
    const float barW = b.width - pad * 2.0f;
    const float sy = b.Top() + 102.0f * s;
    const float th = Lerp(4.0f, 9.0f, Clamp01(m_scrubThick.GetValue())) * s;
    m_scrubBar = Rect{barX, sy - 13.0f * s, barW, 26.0f * s};

    const double dur = m_track.durationSeconds;
    float frac = (dur > 0.0) ? static_cast<float>(m_track.positionSeconds / dur) : 0.0f;
    if (m_dragging == Control::Scrub) frac = m_dragFraction;
    frac = Clamp01(frac);

    ctx->FillRoundedRectangle(D2D1::RoundedRect(RectF(barX, sy - th * 0.5f, barX + barW, sy + th * 0.5f), th * 0.5f, th * 0.5f),
                              Br(White(0.20f)));
    const float fillW = std::max(th, barW * frac);
    if (frac > 0.0f || m_dragging == Control::Scrub) {
        ctx->FillRoundedRectangle(D2D1::RoundedRect(RectF(barX, sy - th * 0.5f, barX + fillW, sy + th * 0.5f), th * 0.5f, th * 0.5f),
                                  Br(White(0.94f)));
    }

    const double shown = (dur > 0.0) ? frac * dur : m_track.positionSeconds;
    const std::wstring left = FormatTime(shown);
    const std::wstring right = (dur > 0.0) ? L"-" + FormatTime(dur - shown) : L"--:--";
    DrawLabel(ctx, left, m_fontTimeLeft.Get(), RectF(barX, sy + 9.0f * s, barX + 80.0f * s, sy + 25.0f * s), White(0.50f));
    DrawLabel(ctx, right, m_fontTimeRight.Get(), RectF(barX + barW - 80.0f * s, sy + 9.0f * s, barX + barW, sy + 25.0f * s), White(0.50f));

    // --- transport -------------------------------------------------------------------------------
    const float cy = b.Top() + 148.0f * s;
    const float cx = b.Center().x;
    const float spacing = 76.0f * s;
    const Vec2 pPrev{cx - spacing, cy}, pPlay{cx, cy}, pNext{cx + spacing, cy};

    m_btnPrev = Rect{pPrev.x - 26.0f * s, cy - 23.0f * s, 52.0f * s, 46.0f * s};
    m_btnPlay = Rect{pPlay.x - 30.0f * s, cy - 25.0f * s, 60.0f * s, 50.0f * s};
    m_btnNext = Rect{pNext.x - 26.0f * s, cy - 23.0f * s, 52.0f * s, 46.0f * s};

    auto drawButton = [&](const Rect& hit, const Vec2& c, int pressIndex, auto&& glyph) {
        const bool hover = hit.Contains(m_mousePos);
        if (hover) {
            ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, c.y), 24.0f * s, 24.0f * s), Br(White(0.10f)));
        }
        const float k = m_pressSpring[pressIndex].GetValue();
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(c.x, c.y)));
        glyph(hover ? 1.0f : 0.92f);
        ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    };

    drawButton(m_btnPrev, pPrev, 0, [&](float a) { DrawSkip(ctx, pPrev, 26.0f * s, false, a); });
    drawButton(m_btnPlay, pPlay, 1, [&](float a) { DrawPlayPause(ctx, pPlay, 30.0f * s, m_playMix.GetValue(), a); });
    drawButton(m_btnNext, pNext, 2, [&](float a) { DrawSkip(ctx, pNext, 26.0f * s, true, a); });
}

void IslandWindow::DrawSystem(ID2D1DeviceContext* ctx, const Rect& b) {
    const float s = m_dpiScale;
    const float colW = b.width / 3.0f;
    const float cy = b.Top() + 52.0f * s;
    const float R = 24.0f * s;
    const float stroke = 5.5f * s;

    const Color colors[3] = {kRingCpu, kRingRam, kRingGpu};
    const wchar_t* labels[3] = {L"CPU", L"RAM", L"GPU"};

    for (int i = 0; i < 3; ++i) {
        const float cx = b.Left() + colW * (static_cast<float>(i) + 0.5f);
        const D2D1_POINT_2F c = D2D1::Point2F(cx, cy);
        const float pct = std::clamp(m_ringSpring[i].GetValue(), 0.0f, 100.0f);
        const Color col = colors[i];

        // Dim track, like Apple's Activity rings
        ctx->DrawEllipse(D2D1::Ellipse(c, R, R), Br(Color{col.r, col.g, col.b, 0.22f}), stroke);

        if (pct >= 99.5f) {
            ctx->DrawEllipse(D2D1::Ellipse(c, R, R), Br(col), stroke);
        } else if (pct > 0.4f) {
            const float sweep = pct / 100.0f * 360.0f;
            const float a0 = -90.0f * kPi / 180.0f;
            const float a1 = (-90.0f + sweep) * kPi / 180.0f;
            ComPtr<ID2D1PathGeometry> arc;
            if (SUCCEEDED(m_dcomp->GetD2DFactory()->CreatePathGeometry(&arc))) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(arc->Open(&sink))) {
                    sink->BeginFigure(D2D1::Point2F(cx + R * std::cos(a0), cy + R * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + R * std::cos(a1), cy + R * std::sin(a1)),
                                                  D2D1::SizeF(R, R), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                                  sweep > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                    ctx->DrawGeometry(arc.Get(), Br(col), stroke, m_roundedStroke.Get());
                }
            }
        }

        wchar_t value[16];
        swprintf_s(value, L"%d%%", static_cast<int>(pct + 0.5f));
        DrawLabel(ctx, value, m_fontRingValue.Get(), RectF(cx - R, cy - R, cx + R, cy + R), White(0.97f));
        DrawLabel(ctx, labels[i], m_fontCaption.Get(), RectF(cx - 40.0f * s, cy + R + 10.0f * s, cx + 40.0f * s, cy + R + 26.0f * s), White(0.55f));
    }
}

void IslandWindow::DrawVolume(ID2D1DeviceContext* ctx, const Rect& b) {
    const float s = m_dpiScale;
    const float cy = b.Center().y;
    float vol = Clamp01(m_volDisplay.GetValue());
    if (m_dragging == Control::Volume) vol = m_dragFraction;

    wchar_t glyph = kIconVol3;
    if (vol <= 0.005f) glyph = kIconMute;
    else if (vol < 0.34f) glyph = kIconVol1;
    else if (vol < 0.67f) glyph = kIconVol2;
    DrawGlyph(ctx, glyph, RectF(b.Left() + 12.0f * s, cy - 14.0f * s, b.Left() + 42.0f * s, cy + 14.0f * s), White(0.95f));

    const float x0 = b.Left() + 52.0f * s;
    const float x1 = b.Right() - 20.0f * s;
    const float th = (m_dragging == Control::Volume ? 10.0f : 7.0f) * s;
    m_volBar = Rect{x0, cy - 15.0f * s, x1 - x0, 30.0f * s};

    ctx->FillRoundedRectangle(D2D1::RoundedRect(RectF(x0, cy - th * 0.5f, x1, cy + th * 0.5f), th * 0.5f, th * 0.5f), Br(White(0.22f)));
    const float fillW = std::max(th, (x1 - x0) * vol);
    ctx->FillRoundedRectangle(D2D1::RoundedRect(RectF(x0, cy - th * 0.5f, x0 + fillW, cy + th * 0.5f), th * 0.5f, th * 0.5f), Br(White(0.95f)));
}

void IslandWindow::DrawNotification(ID2D1DeviceContext* ctx, const Rect& b) {
    const float s = m_dpiScale;
    const float cy = b.Center().y;

    const float icon = 44.0f * s;
    const Rect iconRect{b.Left() + 18.0f * s, cy - icon * 0.5f, icon, icon};
    const Color base = Color::FromHex(m_activeNotification.iconColor);
    if (auto shape = m_dcomp->CreateSquircleGeometry(iconRect, 12.0f * s)) {
        auto grad = m_dcomp->CreateLinearGradientBrush(
            Color{std::min(1.0f, base.r + 0.18f), std::min(1.0f, base.g + 0.18f), std::min(1.0f, base.b + 0.18f), 1.0f},
            Color{base.r * 0.85f, base.g * 0.85f, base.b * 0.85f, 1.0f},
            D2D1::Point2F(iconRect.Left(), iconRect.Top()), D2D1::Point2F(iconRect.Left(), iconRect.Bottom()));
        if (grad) ctx->FillGeometry(shape.Get(), grad.Get());
        ctx->DrawGeometry(shape.Get(), Br(White(0.18f)), 1.0f * s);
    }
    const std::wstring& seed = !m_activeNotification.sourceApp.empty() ? m_activeNotification.sourceApp : m_activeNotification.title;
    if (!seed.empty()) {
        const std::wstring initial(1, static_cast<wchar_t>(towupper(seed[0])));
        DrawLabel(ctx, initial, m_fontInitial.Get(), RectF(iconRect.Left(), iconRect.Top(), iconRect.Right(), iconRect.Bottom()), White(0.97f));
    }

    const float textLeft = iconRect.Right() + 14.0f * s;
    const float textRight = b.Right() - 20.0f * s;
    DrawLabel(ctx, m_activeNotification.title, m_fontNotifTitle.Get(),
              RectF(textLeft, b.Top() + 13.0f * s, textRight - 34.0f * s, b.Top() + 34.0f * s), White(0.98f));
    DrawLabel(ctx, L"now", m_fontCaption.Get(),
              RectF(textRight - 34.0f * s, b.Top() + 13.0f * s, textRight, b.Top() + 34.0f * s), White(0.36f));
    DrawLabel(ctx, m_activeNotification.message, m_fontNotifBody.Get(),
              RectF(textLeft, b.Top() + 35.0f * s, textRight, b.Bottom() - 9.0f * s), White(0.62f));
}

// =============================================================================================
//  Input
// =============================================================================================

IslandWindow::Control IslandWindow::HitTestControl(const Vec2& pt) const {
    if (m_contentAlpha.GetValue() < 0.6f) return Control::None;
    if (m_currentMode == IslandMode::MediaExpanded) {
        if (m_btnPlay.Contains(pt)) return Control::Play;
        if (m_btnPrev.Contains(pt)) return Control::Prev;
        if (m_btnNext.Contains(pt)) return Control::Next;
        if (m_scrubBar.Contains(pt)) return Control::Scrub;
    } else if (m_currentMode == IslandMode::VolumeHUD) {
        if (m_volBar.Contains(pt)) return Control::Volume;
    }
    return Control::None;
}

void IslandWindow::ApplyDrag(const Vec2& pt) {
    auto& media = media::MediaService::Instance();
    if (m_dragging == Control::Scrub) {
        m_dragFraction = Clamp01((pt.x - m_scrubBar.x) / std::max(1.0f, m_scrubBar.width));
    } else if (m_dragging == Control::Volume) {
        m_dragFraction = Clamp01((pt.x - m_volBar.x) / std::max(1.0f, m_volBar.width));
        m_volDisplay.SnapTo(m_dragFraction);
        if (std::abs(m_dragFraction - media.GetVolume()) >= 0.01f) media.SetVolume(m_dragFraction);
        m_volumeHudTimer = 2.0f;
    }
}

void IslandWindow::EndDrag(bool commit) {
    auto& media = media::MediaService::Instance();
    if (commit) {
        if (m_dragging == Control::Scrub && m_track.durationSeconds > 0.0) {
            media.Seek(m_dragFraction * m_track.durationSeconds);
        } else if (m_dragging == Control::Volume) {
            media.SetVolume(m_dragFraction);
        }
    }
    m_dragging = Control::None;
}

LRESULT IslandWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            const Rect bnd = m_boundsSpring.GetValue();
            const Vec2 p{static_cast<float>(pt.x), static_cast<float>(pt.y)};
            // Everything outside the pill falls through to the windows underneath
            return bnd.Contains(p) ? HTCLIENT : HTTRANSPARENT;
        }

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                const bool hand = HitTestControl(m_mousePos) != Control::None;
                SetCursor(LoadCursor(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            const Vec2 p{static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam))};
            m_mousePos = p;
            const bool inside = m_boundsSpring.GetValue().Contains(p);
            if (inside && !m_isHovered) {
                m_isHovered = true;
                TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            } else if (!inside && m_isHovered && m_dragging == Control::None && m_pressed == Control::None) {
                m_isHovered = false;
            }
            if (m_dragging != Control::None) ApplyDrag(p);
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (m_dragging == Control::None && m_pressed == Control::None) {
                m_isHovered = false;
                m_mousePos = Vec2{-1.0f, -1.0f};
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            // Scroll over the island to change the volume
            const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
            TriggerVolumeHUD(media::MediaService::Instance().GetVolume() + notches * 0.03f);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const Vec2 p{static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam))};
            m_mousePos = p;
            const bool hasMedia = m_track.hasMedia && !m_track.title.empty();

            if (m_currentMode == IslandMode::Idle) {
                SetMode(hasMedia ? IslandMode::MediaExpanded : IslandMode::SystemExpanded);
                m_isPinned = true;
            } else if (m_currentMode == IslandMode::MediaExpanded || m_currentMode == IslandMode::VolumeHUD) {
                const Control c = HitTestControl(p);
                if (c == Control::Scrub || c == Control::Volume) {
                    m_dragging = c;
                    SetCapture(hwnd);
                    ApplyDrag(p);
                } else if (c != Control::None) {
                    m_pressed = c;
                    SetCapture(hwnd);
                } else {
                    SetMode(IslandMode::Idle);
                    m_isPinned = false;
                }
            } else {
                SetMode(IslandMode::Idle);
                m_isPinned = false;
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            const Vec2 p{static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam))};
            // ReleaseCapture() sends WM_CAPTURECHANGED synchronously, which clears the press / drag state:
            // remember it first, otherwise the buttons and the seek bar never see the release.
            const Control dragging = m_dragging, pressedBefore = m_pressed;
            if (GetCapture() == hwnd) ReleaseCapture();

            if (dragging != Control::None) {
                m_dragging = dragging;
                EndDrag(true);
            } else if (pressedBefore != Control::None) {
                // Buttons fire on release (and only if the pointer is still on them), like real UI
                const Control pressed = pressedBefore;
                m_pressed = Control::None;
                if (HitTestControl(p) == pressed) {
                    auto& media = media::MediaService::Instance();
                    if (pressed == Control::Play) media.PlayPause();
                    else if (pressed == Control::Prev) media.PreviousTrack();
                    else if (pressed == Control::Next) media.NextTrack();
                }
            }
            m_isHovered = m_boundsSpring.GetValue().Contains(p);
            if (!m_isHovered) m_mousePos = Vec2{-1.0f, -1.0f};
            return 0;
        }

        case WM_CAPTURECHANGED:
            if (m_dragging != Control::None) EndDrag(false);
            m_pressed = Control::None;
            return 0;

        case WM_RBUTTONUP: {
            POINT pt;
            GetCursorPos(&pt);
            using namespace aether::ui;
            namespace i18n = aether::i18n;
            SetForegroundWindow(hwnd);
            const int cmd = ShowContextMenu({
                MenuItem(1001, i18n::Pick(L"Open Settings Center", L"Ayar Merkezini Aç"), 0xE713),
                MenuSeparator(),
                MenuItem(1002, i18n::Pick(L"Toggle desktop widgets", L"Masaüstü widget'larını aç/kapat"), 0xE8A1),
                MenuItem(1003, i18n::Pick(L"Cycle island mode", L"Ada modunu değiştir"), 0xE72C),
                MenuItem(1004, i18n::Pick(L"Test notification", L"Bildirimi dene"), 0xEA8F),
                MenuSeparator(),
                MenuItem(1005, i18n::Pick(L"Exit Aether", L"Aether'den çık"), 0xE7E8, true)}, pt);

            if (cmd == 1001) {
                PostMessage(hwnd, WM_USER + 100, 0, 0);
            } else if (cmd == 1002) {
                static bool widgetsOn = true;
                widgetsOn = !widgetsOn;
                widgets::WidgetManager::Instance().SetVisible(widgetsOn);
            } else if (cmd == 1003) {
                if (m_currentMode == IslandMode::Idle) SetMode(IslandMode::MediaExpanded);
                else if (m_currentMode == IslandMode::MediaExpanded) SetMode(IslandMode::SystemExpanded);
                else SetMode(IslandMode::Idle);
            } else if (cmd == 1004) {
                NotificationData notif;
                notif.title = L"Aether";
                notif.sourceApp = L"Aether";
                notif.message = L"Dynamic Island is running with hardware-composited rendering.";
                notif.iconColor = 0xFF0A84FF;
                PushNotification(notif);
            } else if (cmd == 1005) {
                PostQuitMessage(0);
            }
            return 0;
        }

        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE: {
            const uint32_t oldW = m_windowWidth, oldH = m_windowHeight;
            const float oldScale = m_dpiScale;
            PositionWindow();
            if (m_dcomp && (m_windowWidth != oldW || m_windowHeight != oldH)) {
                m_dcomp->Resize(m_windowWidth, m_windowHeight);
            }
            if (m_dpiScale != oldScale) CreateFonts();
            UpdateTargetBounds();
            m_boundsSpring.SnapTo(m_boundsSpring.GetTarget());
            m_radiusSpring.SnapTo(m_radiusSpring.GetTarget());
            m_lastRegionBounds = Rect{-1.0f, -1.0f, -1.0f, -1.0f};
            return 0;
        }

        case WM_DESTROY: {
            m_hwnd = nullptr;
            return 0;
        }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace aether::island
