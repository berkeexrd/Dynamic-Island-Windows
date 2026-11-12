#include "aether/island/island_window.hpp"
#include "aether/config/config_manager.hpp"
#include "aether/system/taskbar_service.hpp"
#include "aether/core/log.hpp"
#include "aether/core/events.hpp"
#include <ctime>
#include <cmath>
#include <algorithm>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <d2d1effects.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxguid.lib")

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

namespace aether::island {

static const wchar_t* ISLAND_CLASS_NAME = L"AetherDynamicIslandClass";

static void DrawD2DText(ID2D1DeviceContext* ctx, const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, ID2D1Brush* brush) {
    if (ctx && format && brush && !text.empty()) {
        ctx->DrawText(text.c_str(), static_cast<UINT32>(text.length()), format, &rect, brush);
    }
}

IslandWindow::IslandWindow()
    : m_boundsSpring(core::Rect{300, 14, 160, 34}, animation::SpringConfig{460.0f, 34.0f, 1.0f, 0.0005f})
    , m_radiusSpring(17.0f, animation::SpringConfig{460.0f, 34.0f, 1.0f, 0.0005f})
    , m_contentAlpha(1.0f, animation::SpringConfig{480.0f, 36.0f, 1.0f, 0.0005f})
    , m_trackAnimProgress(1.0f, animation::SpringConfig{380.0f, 30.0f, 1.0f, 0.001f}) {
}

IslandWindow::~IslandWindow() {
    CleanupCapture();
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

bool IslandWindow::Create() {
    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = ISLAND_CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    HMONITOR hMon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(MONITORINFO)};
    GetMonitorInfoW(hMon, &mi);

    int screenWidth = mi.rcMonitor.right - mi.rcMonitor.left;
    m_windowWidth = 840;
    m_windowHeight = 260;
    int windowX = mi.rcMonitor.left + (screenWidth - m_windowWidth) / 2;
    int windowY = mi.rcMonitor.top;

    m_hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        ISLAND_CLASS_NAME,
        L"Aether Dynamic Island",
        WS_POPUP,
        windowX, windowY, m_windowWidth, m_windowHeight,
        nullptr, nullptr, GetModuleHandle(nullptr), this
    );

    if (!m_hwnd) {
        AETHER_LOG_ERROR("Failed to create Island window");
        return false;
    }

    // DirectComposition per-pixel transparency (alpha = 0 is 100% crystal-clear desktop)
    SetWindowDisplayAffinity(m_hwnd, 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */);

    m_dpiScale = static_cast<float>(GetDpiForWindow(m_hwnd)) / 96.0f;

    m_dcomp = std::make_unique<render::DCompLayer>();
    if (!m_dcomp->Initialize(m_hwnd, m_windowWidth, m_windowHeight)) {
        AETHER_LOG_ERROR("Failed to initialize DCompLayer for Island");
        return false;
    }

    m_roundedStroke = m_dcomp->CreateRoundedStrokeStyle();

    // DirectWrite Typography Hierarchy
    auto dwrite = m_dcomp->GetDWriteFactory();
    if (dwrite) {
        // Hero numbers (large percentages)
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Display",
            nullptr,
            DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            20.0f * m_dpiScale,
            L"en-us",
            &m_fontHero
        );

        // Centered Hero numbers for activity gauges
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Display",
            nullptr,
            DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            16.0f * m_dpiScale,
            L"en-us",
            &m_fontCenterHero
        );
        if (m_fontCenterHero) {
            m_fontCenterHero->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_fontCenterHero->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        DWRITE_TRIMMING trimmingChar = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };

        // Titles (Track name, card header)
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Display",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            14.5f * m_dpiScale,
            L"en-us",
            &m_fontTitle
        );
        if (m_fontTitle) {
            m_fontTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_fontTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            ComPtr<IDWriteInlineObject> ellipsisTitle;
            dwrite->CreateEllipsisTrimmingSign(m_fontTitle.Get(), &ellipsisTitle);
            m_fontTitle->SetTrimming(&trimmingChar, ellipsisTitle.Get());
        }

        // Dedicated Idle Track Title Format with clean character ellipsis
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Display",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.5f * m_dpiScale,
            L"en-us",
            &m_fontIdleTitle
        );
        if (m_fontIdleTitle) {
            m_fontIdleTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_fontIdleTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            ComPtr<IDWriteInlineObject> ellipsisIdle;
            dwrite->CreateEllipsisTrimmingSign(m_fontIdleTitle.Get(), &ellipsisIdle);
            m_fontIdleTitle->SetTrimming(&trimmingChar, ellipsisIdle.Get());
        }

        // Centered Titles
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Display",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            14.5f * m_dpiScale,
            L"en-us",
            &m_fontCenterTitle
        );
        if (m_fontCenterTitle) {
            m_fontCenterTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_fontCenterTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Body text (Artist, subtitles)
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Text",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f * m_dpiScale,
            L"en-us",
            &m_fontBody
        );
        if (m_fontBody) {
            m_fontBody->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_fontBody->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            ComPtr<IDWriteInlineObject> ellipsisBody;
            dwrite->CreateEllipsisTrimmingSign(m_fontBody.Get(), &ellipsisBody);
            m_fontBody->SetTrimming(&trimmingChar, ellipsisBody.Get());
        }

        // Small metadata, captions, badges
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Small",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            10.0f * m_dpiScale,
            L"en-us",
            &m_fontCaption
        );
        if (m_fontCaption) {
            m_fontCaption->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Centered small captions
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Small",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            10.0f * m_dpiScale,
            L"en-us",
            &m_fontCenterCaption
        );
        if (m_fontCenterCaption) {
            m_fontCenterCaption->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_fontCenterCaption->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Idle clock text (optically centered vertically)
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Display",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            13.5f * m_dpiScale,
            L"en-us",
            &m_fontClock
        );
        if (m_fontClock) {
            m_fontClock->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    core::EventBus::Instance().Subscribe<media::MediaChangedEvent>([this](const media::MediaChangedEvent& evt) {
        UpdateTargetBounds();
        if (evt.track.hasMedia && !evt.track.title.empty() && evt.track.title != m_lastTrackTitle) {
            m_lastTrackTitle = evt.track.title;
            m_trackAnimProgress.SnapTo(0.0f);
            m_trackAnimProgress.SetTarget(1.0f);
        }
    });

    UpdateTargetBounds();
    m_boundsSpring.SnapTo(m_boundsSpring.GetTarget());
    m_radiusSpring.SnapTo(m_radiusSpring.GetTarget());
    UpdateWindowRegion(m_boundsSpring.GetValue(), m_radiusSpring.GetValue());

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    AETHER_LOG_INFO("IslandWindow created successfully with crystal-clear DirectComposition surface");
    return true;
}

void IslandWindow::Show(bool show) {
    if (m_hwnd) {
        ShowWindow(m_hwnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
}

void IslandWindow::UpdateAlbumArt(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        if (m_albumArtBitmap) {
            m_albumArtBitmap.Reset();
            m_lastArtHash = 0;
        }
        return;
    }

    size_t hash = bytes.size();
    if (bytes.size() >= 8) {
        hash ^= *reinterpret_cast<const size_t*>(bytes.data());
    }
    if (hash == m_lastArtHash && m_albumArtBitmap) {
        return;
    }
    m_lastArtHash = hash;

    int w = 0, h = 0, comp = 0;
    unsigned char* decoded = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (!decoded) {
        m_albumArtBitmap.Reset();
        return;
    }

    auto ctx = m_dcomp ? m_dcomp->GetContext() : nullptr;
    if (ctx && w > 0 && h > 0) {
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)
        );
        m_albumArtBitmap.Reset();
        HRESULT hr = ctx->CreateBitmap(D2D1::SizeU(w, h), decoded, w * 4, props, &m_albumArtBitmap);
        if (SUCCEEDED(hr)) {
            AETHER_LOG_INFO("Decoded GSMTC Album Art: " << w << "x" << h << " (" << bytes.size() << " bytes)");
        }
    }

    stbi_image_free(decoded);
}

void IslandWindow::CleanupCapture() {
    if (m_memBmp) {
        DeleteObject(m_memBmp);
        m_memBmp = nullptr;
    }
    if (m_memDC) {
        DeleteDC(m_memDC);
        m_memDC = nullptr;
    }
    m_bmpBits = nullptr;
    m_desktopBitmap.Reset();
    m_blurEffect.Reset();
    m_captureWidth = 0;
    m_captureHeight = 0;
}

void IslandWindow::CaptureDesktopBackground() {
    if (!m_hwnd) return;
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    if (static_cast<uint32_t>(w) != m_captureWidth || static_cast<uint32_t>(h) != m_captureHeight || !m_memDC) {
        if (m_memBmp) DeleteObject(m_memBmp);
        if (m_memDC) DeleteDC(m_memDC);

        HDC screenDC = GetDC(nullptr);
        m_memDC = CreateCompatibleDC(screenDC);
        BITMAPINFO bmi = {sizeof(BITMAPINFOHEADER)};
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        m_memBmp = CreateDIBSection(m_memDC, &bmi, DIB_RGB_COLORS, &m_bmpBits, nullptr, 0);
        SelectObject(m_memDC, m_memBmp);
        ReleaseDC(nullptr, screenDC);

        m_captureWidth = static_cast<uint32_t>(w);
        m_captureHeight = static_cast<uint32_t>(h);
        m_desktopBitmap.Reset();
        m_blurEffect.Reset();
    }

    HDC screenDC = GetDC(nullptr);
    BitBlt(m_memDC, 0, 0, w, h, screenDC, rc.left, rc.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screenDC);

    // Ensure alpha is 0xFF so Direct2D treats desktop snapshot as opaque
    if (m_bmpBits) {
        uint32_t* p = reinterpret_cast<uint32_t*>(m_bmpBits);
        size_t total = static_cast<size_t>(w * h);
        for (size_t i = 0; i < total; ++i) {
            p[i] |= 0xFF000000;
        }
    }

    auto ctx = m_dcomp ? m_dcomp->GetContext() : nullptr;
    if (!ctx) return;

    if (!m_desktopBitmap) {
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );
        ctx->CreateBitmap(D2D1::SizeU(w, h), m_bmpBits, w * 4, props, &m_desktopBitmap);
    } else {
        D2D1_RECT_U destRect = D2D1::RectU(0, 0, w, h);
        m_desktopBitmap->CopyFromMemory(&destRect, m_bmpBits, w * 4);
    }

    if (!m_blurEffect && ctx && m_desktopBitmap) {
        ctx->CreateEffect(CLSID_D2D1GaussianBlur, &m_blurEffect);
        if (m_blurEffect) {
            m_blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 20.0f);
            m_blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            m_blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
            m_blurEffect->SetInput(0, m_desktopBitmap.Get());
        }
    } else if (m_blurEffect && m_desktopBitmap) {
        m_blurEffect->SetInput(0, m_desktopBitmap.Get());
    }
}

bool IslandWindow::IsResting() const {
    if (m_isGamingSuspended) {
        return m_boundsSpring.IsResting() && m_contentAlpha.IsResting();
    }
    if (!m_boundsSpring.IsResting() || !m_radiusSpring.IsResting() || !m_contentAlpha.IsResting() || !m_trackAnimProgress.IsResting()) {
        return false;
    }
    if (m_isHovered) {
        return false;
    }
    // Keep 240Hz rendering active whenever audio/media is playing in ANY mode
    auto track = media::MediaService::Instance().GetCurrentTrack();
    if (track.hasMedia && track.status == media::PlaybackStatus::Playing) {
        return false;
    }
    if (m_currentMode == IslandMode::VolumeHUD || m_currentMode == IslandMode::Notification) {
        return false;
    }
    return true;
}

void IslandWindow::SetGamingSuspended(bool suspended) {
    if (m_isGamingSuspended == suspended) return;
    m_isGamingSuspended = suspended;
    if (m_isGamingSuspended) {
        UpdateTargetBounds();
    } else {
        // Slide down gracefully from monitor bezel with spring physics
        float w = 160.0f * m_dpiScale;
        float h = 34.0f * m_dpiScale;
        float centerX = m_windowWidth * 0.5f;
        m_boundsSpring.SnapTo(core::Rect{centerX - w * 0.5f, -40.0f * m_dpiScale, w, h});
        m_contentAlpha.SnapTo(0.15f);
        SetMode(IslandMode::Idle);
        UpdateTargetBounds();
        m_contentAlpha.SetTarget(1.0f);
    }
}

void IslandWindow::SetMode(IslandMode mode) {
    if (m_currentMode == mode) return;
    m_currentMode = mode;
    UpdateTargetBounds();
    m_contentAlpha.SnapTo(0.20f);
    m_contentAlpha.SetTarget(1.0f);
}

void IslandWindow::UpdateTargetBounds() {
    const auto& cfg = config::ConfigManager::Instance().Get().island;
    float topY = cfg.topOffset * m_dpiScale;
    float centerX = m_windowWidth * 0.5f;

    core::Rect target;
    float targetRadius = 17.0f * m_dpiScale;

    if (m_isGamingSuspended) {
        float w = 60.0f * m_dpiScale;
        float h = 10.0f * m_dpiScale;
        target = core::Rect{centerX - w * 0.5f, -60.0f * m_dpiScale, w, h};
        targetRadius = h * 0.5f;
        m_boundsSpring.SetTarget(target);
        m_radiusSpring.SetTarget(targetRadius);
        m_contentAlpha.SetTarget(0.0f);
        return;
    }

    switch (m_currentMode) {
        case IslandMode::Idle: {
            auto track = media::MediaService::Instance().GetCurrentTrack();
            bool hasMedia = track.hasMedia && !track.title.empty();
            float baseW = hasMedia ? 260.0f : 160.0f;
            float w = baseW * m_dpiScale;
            float h = 34.0f * m_dpiScale;
            target = core::Rect{centerX - w * 0.5f, topY, w, h};
            targetRadius = h * 0.5f;
            break;
        }
        case IslandMode::MediaExpanded: {
            float w = 425.0f * m_dpiScale;
            float h = 172.0f * m_dpiScale;
            target = core::Rect{centerX - w * 0.5f, topY, w, h};
            targetRadius = 26.0f * m_dpiScale;
            break;
        }
        case IslandMode::SystemExpanded: {
            float w = 390.0f * m_dpiScale;
            float h = 140.0f * m_dpiScale;
            target = core::Rect{centerX - w * 0.5f, topY, w, h};
            targetRadius = 24.0f * m_dpiScale;
            break;
        }
        case IslandMode::VolumeHUD: {
            float w = 240.0f * m_dpiScale;
            float h = 36.0f * m_dpiScale;
            target = core::Rect{centerX - w * 0.5f, topY, w, h};
            targetRadius = h * 0.5f;
            break;
        }
        case IslandMode::Notification: {
            float w = 370.0f * m_dpiScale;
            float h = 76.0f * m_dpiScale;
            target = core::Rect{centerX - w * 0.5f, topY, w, h};
            targetRadius = 22.0f * m_dpiScale;
            break;
        }
    }

    m_boundsSpring.SetTarget(target);
    m_radiusSpring.SetTarget(targetRadius);
}

void IslandWindow::UpdateWindowRegion(const core::Rect& bounds, float radius) {
    if (!m_hwnd) return;
    if (std::abs(bounds.x - m_lastRegionBounds.x) < 0.5f &&
        std::abs(bounds.y - m_lastRegionBounds.y) < 0.5f &&
        std::abs(bounds.width - m_lastRegionBounds.width) < 0.5f &&
        std::abs(bounds.height - m_lastRegionBounds.height) < 0.5f) {
        return; // Region unchanged, skip OS call
    }
    m_lastRegionBounds = bounds;

    // Generous padding for shadow & rim lighting so nothing is clipped
    int padX = static_cast<int>(12.0f * m_dpiScale);
    int padY = static_cast<int>(12.0f * m_dpiScale);
    int x1 = static_cast<int>(bounds.Left()) - padX;
    int y1 = static_cast<int>(bounds.Top()) - padY;
    int x2 = static_cast<int>(bounds.Right()) + padX;
    int y2 = static_cast<int>(bounds.Bottom()) + padY;
    int r = static_cast<int>((radius + 6.0f) * 2.0f * m_dpiScale);

    HRGN rgn = nullptr;
    if (y2 <= 0 || x2 <= x1 || y2 <= y1) {
        rgn = CreateRectRgn(0, 0, 0, 0); // 100% empty region when slid off-screen, zero dead zones
    } else {
        rgn = CreateRoundRectRgn(x1, y1, x2 + 1, y2 + 1, r, r);
    }
    if (rgn) {
        // SetWindowRgn physically clips the window's OS hit-test boundaries.
        // Outside this region, Windows bypasses Aether and delivers 100% of mouse clicks to applications underneath!
        SetWindowRgn(m_hwnd, rgn, FALSE);
    }
}

void IslandWindow::PushNotification(const NotificationData& notif) {
    m_activeNotification = notif;
    SetMode(IslandMode::Notification);
}

void IslandWindow::TriggerVolumeHUD(float volume) {
    media::MediaService::Instance().SetVolume(volume);
    m_volumeHudTimer = 2.0f;
    SetMode(IslandMode::VolumeHUD);
}

void IslandWindow::Update(float dt) {
    m_boundsSpring.Update(dt);
    m_radiusSpring.Update(dt);
    m_contentAlpha.Update(dt);
    m_trackAnimProgress.Update(dt);

    // Keep OS window region tightly bound to active island squircle (+shadow)
    // Outside this region, Windows delivers 100% of mouse clicks to applications underneath!
    UpdateWindowRegion(m_boundsSpring.GetValue(), m_radiusSpring.GetValue());

    // Keep album art in sync with active media track & trigger transition on track switch
    auto curTrack = media::MediaService::Instance().GetCurrentTrack();
    if (curTrack.hasMedia) {
        if (!curTrack.title.empty() && curTrack.title != m_lastTrackTitle) {
            m_lastTrackTitle = curTrack.title;
            m_trackAnimProgress.SnapTo(0.0f);
            m_trackAnimProgress.SetTarget(1.0f);
        }
        if (!curTrack.albumArtBytes.empty()) {
            UpdateAlbumArt(curTrack.albumArtBytes);
        }
    } else if (m_albumArtBitmap) {
        m_albumArtBitmap.Reset();
        m_lastArtHash = 0;
        m_lastTrackTitle.clear();
    }

    // Periodic capture of desktop background behind island (~12 fps)
    if (!m_isGamingSuspended) {
        m_captureTimer += dt;
        if (m_captureTimer >= 0.08f || !m_desktopBitmap) {
            m_captureTimer = 0.0f;
            CaptureDesktopBackground();
        }
    }

    // Update 10-band harmonic equalizer animation
    for (int i = 0; i < 10; ++i) {
        m_equalizerPhases[i] += dt * (5.5f + i * 0.45f);
    }

    // Dynamic width adaptation in Idle mode (widens for music, shrinks when stopped)
    if (m_currentMode == IslandMode::Idle) {
        auto track = media::MediaService::Instance().GetCurrentTrack();
        bool hasMedia = track.hasMedia && !track.title.empty();
        float expectedW = (hasMedia ? 260.0f : 160.0f) * m_dpiScale;
        if (std::abs(m_boundsSpring.GetTarget().width - expectedW) > 1.0f) {
            UpdateTargetBounds();
        }
    }

    // Auto-dismiss timers
    if (m_currentMode == IslandMode::VolumeHUD) {
        m_volumeHudTimer -= dt;
        if (m_volumeHudTimer <= 0.0f) {
            SetMode(IslandMode::Idle);
        }
    } else if (m_currentMode == IslandMode::Notification) {
        m_activeNotification.timeRemaining -= dt;
        if (m_activeNotification.timeRemaining <= 0.0f) {
            SetMode(IslandMode::Idle);
        }
    }

    // Hover detection logic with smooth hysteresis grace period (prevents flutter/jitter)
    const auto& cfg = config::ConfigManager::Instance().Get().island;
    if (cfg.expandOnHover && !m_isPinned) {
        if (m_isHovered) {
            m_unhoverTimer = 0.0f;
            if (m_currentMode == IslandMode::Idle) {
                m_hoverTimer += dt;
                if (m_hoverTimer >= (cfg.hoverDelayMs / 1000.0f)) {
                    auto track = media::MediaService::Instance().GetCurrentTrack();
                    if (track.hasMedia && track.status == media::PlaybackStatus::Playing) {
                        SetMode(IslandMode::MediaExpanded);
                    } else {
                        SetMode(IslandMode::SystemExpanded);
                    }
                }
            }
        } else {
            m_hoverTimer = 0.0f;
            if (m_currentMode != IslandMode::Idle &&
                m_currentMode != IslandMode::VolumeHUD && m_currentMode != IslandMode::Notification) {
                m_unhoverTimer += dt;
                if (m_unhoverTimer >= 0.28f) { // 280ms grace period so mouse transitions never snap closed
                    SetMode(IslandMode::Idle);
                    m_unhoverTimer = 0.0f;
                }
            }
        }
    }
}

void IslandWindow::Render() {
    auto ctx = m_dcomp->BeginDraw();
    if (!ctx) return;

    core::Rect curBounds = m_boundsSpring.GetValue();
    float curRadius = m_radiusSpring.GetValue();

    RenderBackground(ctx, curBounds, curRadius);

    switch (m_currentMode) {
        case IslandMode::Idle:
            RenderIdleContent(ctx, curBounds);
            break;
        case IslandMode::MediaExpanded:
            RenderMediaContent(ctx, curBounds);
            break;
        case IslandMode::SystemExpanded:
            RenderSystemContent(ctx, curBounds);
            break;
        case IslandMode::VolumeHUD:
            RenderVolumeContent(ctx, curBounds);
            break;
        case IslandMode::Notification:
            RenderNotificationContent(ctx, curBounds);
            break;
    }

    m_dcomp->EndDraw();
}

void IslandWindow::RenderBackground(ID2D1DeviceContext* ctx, const core::Rect& bounds, float radius) {
    const auto& appCfg = config::ConfigManager::Instance().Get().appearance;

    // 1. True Apple Squircle Path Geometry (C2 continuous curvature)
    auto squircle = m_dcomp->CreateSquircleGeometry(bounds, radius);
    if (!squircle) return;

    // 2. Soft Ambient Drop Shadow (hugs the squircle contour, never smudging screen)
    if (appCfg.enableShadow) {
        core::Rect s1 = bounds;
        s1.y += 4.0f * m_dpiScale;
        auto sq1 = m_dcomp->CreateSquircleGeometry(s1, radius + 2.5f * m_dpiScale);
        if (sq1) {
            auto b1 = m_dcomp->CreateSolidBrush(core::Color{0.0f, 0.0f, 0.0f, 0.18f * appCfg.shadowIntensity});
            ctx->FillGeometry(sq1.Get(), b1.Get());
        }
        core::Rect s2 = bounds;
        s2.y += 1.5f * m_dpiScale;
        auto sq2 = m_dcomp->CreateSquircleGeometry(s2, radius + 1.0f * m_dpiScale);
        if (sq2) {
            auto b2 = m_dcomp->CreateSolidBrush(core::Color{0.0f, 0.0f, 0.0f, 0.32f * appCfg.shadowIntensity});
            ctx->FillGeometry(sq2.Get(), b2.Get());
        }
    }

    bool isExpanded = (m_currentMode != IslandMode::Idle && m_currentMode != IslandMode::VolumeHUD);

    // 3. Real GPU Frosted Glass Blur (Clipped exactly to squircle geometry)
    if (m_blurEffect && appCfg.enableBlur) {
        ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), squircle.Get()), nullptr);
        ctx->DrawImage(m_blurEffect.Get());

        // Luminous crystal clear glass tint ("clear tonlar" & frosted luminescence)
        auto glassTint = isExpanded ?
            m_dcomp->CreateLinearGradientBrush(
                core::Color{0.14f, 0.18f, 0.26f, 0.36f},
                core::Color{0.06f, 0.08f, 0.14f, 0.56f},
                D2D1::Point2F(bounds.Center().x, bounds.Top()),
                D2D1::Point2F(bounds.Center().x, bounds.Bottom())
            ) :
            m_dcomp->CreateLinearGradientBrush(
                core::Color{0.12f, 0.16f, 0.24f, 0.40f},
                core::Color{0.05f, 0.07f, 0.12f, 0.62f},
                D2D1::Point2F(bounds.Center().x, bounds.Top()),
                D2D1::Point2F(bounds.Center().x, bounds.Bottom())
            );

        if (glassTint) {
            ctx->FillGeometry(squircle.Get(), glassTint.Get());
        }

        // Ambient colored music aura in expanded mode
        auto track = media::MediaService::Instance().GetCurrentTrack();
        if (isExpanded && track.hasMedia) {
            D2D1_POINT_2F auraCenter = D2D1::Point2F(bounds.Left() + 50.0f * m_dpiScale, bounds.Top() + 50.0f * m_dpiScale);
            auto aura = m_dcomp->CreateRadialGradientBrush(
                core::Color{0.0f, 0.85f, 1.0f, 0.22f * m_contentAlpha.GetValue()},
                core::Color{0.0f, 0.0f, 0.0f, 0.0f},
                auraCenter, 140.0f * m_dpiScale, 110.0f * m_dpiScale
            );
            if (aura) {
                ctx->FillGeometry(squircle.Get(), aura.Get());
            }
        }

        ctx->PopLayer();
    } else {
        // High-end fallback if blur effect is pending or disabled
        auto glassGradient = isExpanded ?
            m_dcomp->CreateLinearGradientBrush(
                core::Color{0.08f, 0.10f, 0.15f, 0.94f},
                core::Color{0.03f, 0.04f, 0.07f, 0.98f},
                D2D1::Point2F(bounds.Center().x, bounds.Top()),
                D2D1::Point2F(bounds.Center().x, bounds.Bottom())
            ) :
            m_dcomp->CreateLinearGradientBrush(
                core::Color{0.05f, 0.06f, 0.09f, 0.95f},
                core::Color{0.02f, 0.02f, 0.04f, 0.98f},
                D2D1::Point2F(bounds.Center().x, bounds.Top()),
                D2D1::Point2F(bounds.Center().x, bounds.Bottom())
            );

        if (glassGradient) {
            ctx->FillGeometry(squircle.Get(), glassGradient.Get());
        }

        auto track = media::MediaService::Instance().GetCurrentTrack();
        if (isExpanded && track.hasMedia) {
            D2D1_POINT_2F auraCenter = D2D1::Point2F(bounds.Left() + 50.0f * m_dpiScale, bounds.Top() + 50.0f * m_dpiScale);
            auto aura = m_dcomp->CreateRadialGradientBrush(
                core::Color{0.0f, 0.85f, 1.0f, 0.16f * m_contentAlpha.GetValue()},
                core::Color{0.0f, 0.0f, 0.0f, 0.0f},
                auraCenter, 140.0f * m_dpiScale, 110.0f * m_dpiScale
            );
            if (aura) {
                ctx->FillGeometry(squircle.Get(), aura.Get());
            }
        }
    }

    // 5. Specular Liquid Glass Top Rim Reflection Line
    auto rimGradient = m_dcomp->CreateLinearGradientBrush(
        core::Color{1.0f, 1.0f, 1.0f, isExpanded ? 0.32f : 0.22f},
        core::Color{1.0f, 1.0f, 1.0f, 0.04f},
        D2D1::Point2F(bounds.Center().x, bounds.Top()),
        D2D1::Point2F(bounds.Center().x, bounds.Bottom())
    );
    if (rimGradient) {
        ctx->DrawGeometry(squircle.Get(), rimGradient.Get(), 1.1f * m_dpiScale);
    }

    // 6. Dynamic Acoustic Pulse Glow on Track Change
    float tProg = m_trackAnimProgress.GetValue();
    if (tProg < 0.99f) {
        float pulseA = (1.0f - tProg) * 0.40f;
        auto pulseBrush = m_dcomp->CreateSolidBrush(core::Color{0.0f, 0.90f, 1.0f, pulseA});
        if (pulseBrush) {
            ctx->DrawGeometry(squircle.Get(), pulseBrush.Get(), 1.8f * m_dpiScale);
        }
    }
}

void IslandWindow::RenderIdleContent(ID2D1DeviceContext* ctx, const core::Rect& bounds) {
    float alpha = m_contentAlpha.GetValue();
    if (alpha <= 0.01f) return;

    auto track = media::MediaService::Instance().GetCurrentTrack();
    bool hasMedia = track.hasMedia && !track.title.empty();
    float centerY = bounds.Center().y;

    if (hasMedia) {
        float tProg = m_trackAnimProgress.GetValue();
        tProg = std::clamp(tProg, 0.0f, 1.0f);

        // --- MEDIA IDLE STATE ---
        // Left: 22x22 Mini Album Art Squircle / Vinyl Record (Scale-Pop animation)
        float iconSize = 22.0f * m_dpiScale;
        float iconX = bounds.Left() + 10.0f * m_dpiScale;
        float iconY = centerY - iconSize * 0.5f;
        core::Rect artRect{iconX, iconY, iconSize, iconSize};

        auto artSquircle = m_dcomp->CreateSquircleGeometry(artRect, 6.0f * m_dpiScale);
        if (m_albumArtBitmap && artSquircle) {
            ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), artSquircle.Get()), nullptr);
            float artScale = 0.82f + 0.18f * tProg;
            float scaledSize = iconSize * artScale;
            float off = (iconSize - scaledSize) * 0.5f;
            D2D1_RECT_F destRect = D2D1::RectF(iconX + off, iconY + off, iconX + off + scaledSize, iconY + off + scaledSize);
            float artAlpha = alpha * (0.35f + 0.65f * tProg);
            ctx->DrawBitmap(m_albumArtBitmap.Get(), destRect, artAlpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            ctx->PopLayer();
            auto rim = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 0.30f * alpha});
            ctx->DrawGeometry(artSquircle.Get(), rim.Get(), 0.8f);
        } else {
            auto artBg = m_dcomp->CreateLinearGradientBrush(
                core::Color{0.30f, 0.20f, 0.50f, alpha},
                core::Color{0.12f, 0.10f, 0.25f, alpha},
                D2D1::Point2F(iconX, iconY),
                D2D1::Point2F(iconX + iconSize, iconY + iconSize)
            );
            if (artSquircle && artBg) {
                ctx->FillGeometry(artSquircle.Get(), artBg.Get());
                auto rim = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 0.25f * alpha});
                ctx->DrawGeometry(artSquircle.Get(), rim.Get(), 0.8f);
            }

            // Center vinyl core dot (electric cyan)
            auto coreBrush = m_dcomp->CreateSolidBrush(core::Color{0.0f, 0.95f, 1.0f, 0.95f * alpha});
            ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(iconX + iconSize * 0.5f, iconY + iconSize * 0.5f), 3.0f * m_dpiScale, 3.0f * m_dpiScale), coreBrush.Get());
        }

        // Middle: Truncated Track Title with subtle drop shadow & slide-up fade transition
        float textLeft = iconX + iconSize + 8.0f * m_dpiScale;
        float textRight = bounds.Right() - 32.0f * m_dpiScale;
        float slideY = (1.0f - tProg) * 5.0f * m_dpiScale;
        float textAlpha = alpha * tProg;
        D2D1_RECT_F textRectShadow = D2D1::RectF(textLeft + 1.0f, bounds.Top() + slideY + 1.0f, textRight + 1.0f, bounds.Bottom() + slideY + 1.0f);
        D2D1_RECT_F textRect = D2D1::RectF(textLeft, bounds.Top() + slideY, textRight, bounds.Bottom() + slideY);
        auto shadowBrush = m_dcomp->CreateSolidBrush(core::Color{0.0f, 0.0f, 0.0f, 0.65f * textAlpha});
        auto titleBrush = m_dcomp->CreateSolidBrush(core::Color{0.98f, 0.98f, 1.0f, textAlpha});
        auto fontTitleToUse = m_fontIdleTitle ? m_fontIdleTitle.Get() : m_fontClock.Get();
        DrawD2DText(ctx, track.title, fontTitleToUse, textRectShadow, shadowBrush.Get());
        DrawD2DText(ctx, track.title, fontTitleToUse, textRect, titleBrush.Get());

        // Right: 4-Band Animated Fluid Audio Waveform (Playing) or 2 Clean Pause Bars (Paused)
        float rightBaseX = bounds.Right() - 26.0f * m_dpiScale;
        auto barBrush = m_dcomp->CreateLinearGradientBrush(
            core::Color{0.0f, 0.95f, 1.0f, alpha},     // Cyan
            core::Color{0.10f, 0.95f, 0.55f, alpha},   // Emerald
            D2D1::Point2F(rightBaseX, centerY + 8.0f),
            D2D1::Point2F(rightBaseX, centerY - 8.0f)
        );

        if (track.status == media::PlaybackStatus::Playing) {
            for (int i = 0; i < 4; ++i) {
                float barH = (std::sin(m_equalizerPhases[i]) * 0.5f + 0.5f) * 12.0f * m_dpiScale + 4.0f;
                float bx = rightBaseX + i * 5.0f * m_dpiScale;
                D2D1_ROUNDED_RECT pillR = D2D1::RoundedRect(
                    D2D1::RectF(bx, centerY - barH * 0.5f, bx + 2.5f * m_dpiScale, centerY + barH * 0.5f),
                    1.25f, 1.25f
                );
                ctx->FillRoundedRectangle(pillR, barBrush.Get());
            }
        } else {
            // Paused: two clean mini vertical bars
            float barH = 10.0f * m_dpiScale;
            float bx1 = bounds.Right() - 22.0f * m_dpiScale;
            float bx2 = bounds.Right() - 16.0f * m_dpiScale;
            ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx1, centerY - barH * 0.5f, bx1 + 2.5f * m_dpiScale, centerY + barH * 0.5f), 1.25f, 1.25f), barBrush.Get());
            ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx2, centerY - barH * 0.5f, bx2 + 2.5f * m_dpiScale, centerY + barH * 0.5f), 1.25f, 1.25f), barBrush.Get());
        }
    } else {
        // --- STANDARD IDLE STATE (CLOCK + SENSOR / STATUS CHIP) ---
        std::time_t t = std::time(nullptr);
        std::tm now{};
        localtime_s(&now, &t);
        wchar_t timeBuf[16];
        swprintf_s(timeBuf, L"%02d:%02d", now.tm_hour, now.tm_min);

        D2D1_RECT_F textRectShadow = D2D1::RectF(
            bounds.Left() + 19.0f * m_dpiScale,
            bounds.Top() + 1.0f,
            bounds.Left() + 76.0f * m_dpiScale,
            bounds.Bottom() + 1.0f
        );
        D2D1_RECT_F textRect = D2D1::RectF(
            bounds.Left() + 18.0f * m_dpiScale,
            bounds.Top(),
            bounds.Left() + 75.0f * m_dpiScale,
            bounds.Bottom()
        );
        auto shadowBrush = m_dcomp->CreateSolidBrush(core::Color{0.0f, 0.0f, 0.0f, 0.60f * alpha});
        auto textBrush = m_dcomp->CreateSolidBrush(core::Color{0.98f, 0.98f, 1.0f, alpha});
        DrawD2DText(ctx, timeBuf, m_fontClock.Get(), textRectShadow, shadowBrush.Get());
        DrawD2DText(ctx, timeBuf, m_fontClock.Get(), textRect, textBrush.Get());

        // Micro separator dot
        auto sepBrush = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 0.25f * alpha});
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bounds.Left() + 82.0f * m_dpiScale, centerY), 1.5f * m_dpiScale, 1.5f * m_dpiScale), sepBrush.Get());

        // Right side: Glowing cyan accent status indicator
        float rightX = bounds.Right() - 24.0f * m_dpiScale;
        auto dotGlow = m_dcomp->CreateSolidBrush(core::Color{0.0f, 0.85f, 1.0f, 0.35f * alpha});
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rightX, centerY), 7.0f * m_dpiScale, 7.0f * m_dpiScale), dotGlow.Get());

        auto dotBrush = m_dcomp->CreateSolidBrush(core::Color{0.15f, 0.95f, 1.0f, 0.98f * alpha});
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rightX, centerY), 3.0f * m_dpiScale, 3.0f * m_dpiScale), dotBrush.Get());
    }
}

void IslandWindow::RenderBrandBadge(ID2D1DeviceContext* ctx, const std::wstring& sourceApp, const core::Rect& rect, float alpha) {
    if (!ctx) return;

    enum class Brand { Spotify, OperaGX, Chrome, Edge, YouTube, Generic };
    Brand brand = Brand::Generic;
    std::wstring brandName = L"Media";

    if (sourceApp.find(L"Spotify") != std::wstring::npos) {
        brand = Brand::Spotify;
        brandName = L"Spotify";
    } else if (sourceApp.find(L"Opera") != std::wstring::npos) {
        brand = Brand::OperaGX;
        brandName = L"Opera GX";
    } else if (sourceApp.find(L"Chrome") != std::wstring::npos) {
        brand = Brand::Chrome;
        brandName = L"Chrome";
    } else if (sourceApp.find(L"Edge") != std::wstring::npos) {
        brand = Brand::Edge;
        brandName = L"Edge";
    } else if (sourceApp.find(L"YouTube") != std::wstring::npos) {
        brand = Brand::YouTube;
        brandName = L"YouTube";
    }

    // Outer pill container (translucent glass badge with subtle border)
    auto badgeGeo = m_dcomp->CreateSquircleGeometry(rect, rect.height * 0.5f);
    if (badgeGeo) {
        auto bgBrush = m_dcomp->CreateSolidBrush(core::Color{0.12f, 0.15f, 0.22f, 0.65f * alpha});
        auto borderBrush = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 0.16f * alpha});
        ctx->FillGeometry(badgeGeo.Get(), bgBrush.Get());
        ctx->DrawGeometry(badgeGeo.Get(), borderBrush.Get(), 0.8f);
    }

    // Brand icon center point
    float iconRadius = 7.5f * m_dpiScale;
    D2D1_POINT_2F ic = D2D1::Point2F(rect.Left() + 13.0f * m_dpiScale, rect.Center().y);

    switch (brand) {
        case Brand::Spotify: {
            // Authentic Spotify Emerald circle
            auto spBg = m_dcomp->CreateSolidBrush(core::Color{0.11f, 0.84f, 0.38f, 0.95f * alpha});
            ctx->FillEllipse(D2D1::Ellipse(ic, iconRadius, iconRadius), spBg.Get());

            // 3 curved sound wave arcs in crisp white
            auto arcBrush = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 0.95f * alpha});
            for (int i = 0; i < 3; ++i) {
                float r = (5.0f - i * 1.4f) * m_dpiScale;
                float offY = (i * 1.5f - 1.2f) * m_dpiScale;
                D2D1_POINT_2F pCenter = D2D1::Point2F(ic.x, ic.y + offY + 0.8f * m_dpiScale);

                ComPtr<ID2D1PathGeometry> arcPath;
                m_dcomp->GetD2DFactory()->CreatePathGeometry(&arcPath);
                if (arcPath) {
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(arcPath->Open(&sink))) {
                        float startAngle = -140.0f * 3.14159f / 180.0f;
                        float endAngle = -40.0f * 3.14159f / 180.0f;
                        sink->BeginFigure(D2D1::Point2F(pCenter.x + r * std::cos(startAngle), pCenter.y + r * std::sin(startAngle)), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddArc(D2D1::ArcSegment(
                            D2D1::Point2F(pCenter.x + r * std::cos(endAngle), pCenter.y + r * std::sin(endAngle)),
                            D2D1::SizeF(r, r * 0.82f),
                            0.0f,
                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                            D2D1_ARC_SIZE_SMALL
                        ));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                    }
                    ctx->DrawGeometry(arcPath.Get(), arcBrush.Get(), 1.15f * m_dpiScale, m_roundedStroke.Get());
                }
            }
            break;
        }
        case Brand::OperaGX: {
            // Cyberpunk Dark background with Neon Crimson ring (#FA1E4E)
            auto opBg = m_dcomp->CreateSolidBrush(core::Color{0.06f, 0.06f, 0.09f, 0.95f * alpha});
            ctx->FillEllipse(D2D1::Ellipse(ic, iconRadius, iconRadius), opBg.Get());
            auto opRing = m_dcomp->CreateSolidBrush(core::Color{0.98f, 0.12f, 0.31f, 0.95f * alpha});
            ctx->DrawEllipse(D2D1::Ellipse(ic, iconRadius - 1.0f * m_dpiScale, iconRadius - 1.0f * m_dpiScale), opRing.Get(), 1.6f * m_dpiScale);
            ctx->FillEllipse(D2D1::Ellipse(ic, 2.8f * m_dpiScale, 2.8f * m_dpiScale), opRing.Get());
            break;
        }
        case Brand::Chrome: {
            // Segmented circular Chrome badge
            auto redBrush = m_dcomp->CreateSolidBrush(core::Color{0.92f, 0.26f, 0.21f, 0.95f * alpha});
            auto blueBrush = m_dcomp->CreateSolidBrush(core::Color{0.26f, 0.52f, 0.96f, 0.95f * alpha});
            auto whiteBrush = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 0.95f * alpha});

            ctx->FillEllipse(D2D1::Ellipse(ic, iconRadius, iconRadius), redBrush.Get());
            ctx->FillEllipse(D2D1::Ellipse(ic, 4.2f * m_dpiScale, 4.2f * m_dpiScale), whiteBrush.Get());
            ctx->FillEllipse(D2D1::Ellipse(ic, 3.2f * m_dpiScale, 3.2f * m_dpiScale), blueBrush.Get());
            break;
        }
        case Brand::Edge: {
            auto edgeBrush = m_dcomp->CreateLinearGradientBrush(
                core::Color{0.0f, 0.47f, 0.84f, 0.95f * alpha},
                core::Color{0.0f, 0.82f, 0.65f, 0.95f * alpha},
                D2D1::Point2F(ic.x - iconRadius, ic.y - iconRadius),
                D2D1::Point2F(ic.x + iconRadius, ic.y + iconRadius)
            );
            if (edgeBrush) {
                ctx->FillEllipse(D2D1::Ellipse(ic, iconRadius, iconRadius), edgeBrush.Get());
            }
            break;
        }
        default: {
            auto roseBrush = m_dcomp->CreateLinearGradientBrush(
                core::Color{0.99f, 0.24f, 0.27f, 0.95f * alpha},
                core::Color{0.94f, 0.18f, 0.40f, 0.95f * alpha},
                D2D1::Point2F(ic.x - iconRadius, ic.y - iconRadius),
                D2D1::Point2F(ic.x + iconRadius, ic.y + iconRadius)
            );
            if (roseBrush) {
                ctx->FillEllipse(D2D1::Ellipse(ic, iconRadius, iconRadius), roseBrush.Get());
            }
            break;
        }
    }

    // Brand title text beside icon
    D2D1_RECT_F labelRect = D2D1::RectF(
        rect.Left() + 24.0f * m_dpiScale,
        rect.Top(),
        rect.Right() - 6.0f * m_dpiScale,
        rect.Bottom()
    );
    auto textBrush = m_dcomp->CreateSolidBrush(core::Color{0.88f, 0.92f, 0.98f, 0.95f * alpha});
    DrawD2DText(ctx, brandName, m_fontCaption.Get(), labelRect, textBrush.Get());
}

void IslandWindow::RenderMediaContent(ID2D1DeviceContext* ctx, const core::Rect& bounds) {
    float alpha = m_contentAlpha.GetValue();
    if (alpha <= 0.01f) return;

    auto track = media::MediaService::Instance().GetCurrentTrack();
    float tProg = m_trackAnimProgress.GetValue();
    tProg = std::clamp(tProg, 0.0f, 1.0f);

    // 1. Album Artwork Squircle (64x64, with scale-pop animation)
    float artSize = 64.0f * m_dpiScale;
    float artX = bounds.Left() + 18.0f * m_dpiScale;
    float artY = bounds.Top() + 18.0f * m_dpiScale;
    core::Rect artBounds{artX, artY, artSize, artSize};

    auto artSquircle = m_dcomp->CreateSquircleGeometry(artBounds, 14.0f * m_dpiScale);

    if (m_albumArtBitmap && artSquircle) {
        ctx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), artSquircle.Get()), nullptr);
        float artScale = 0.90f + 0.10f * tProg;
        float scaledSize = artSize * artScale;
        float off = (artSize - scaledSize) * 0.5f;
        D2D1_RECT_F destRect = D2D1::RectF(artX + off, artY + off, artX + off + scaledSize, artY + off + scaledSize);
        float artAlpha = alpha * (0.30f + 0.70f * tProg);
        ctx->DrawBitmap(m_albumArtBitmap.Get(), destRect, artAlpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        ctx->PopLayer();

        auto rim = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 0.28f * alpha});
        ctx->DrawGeometry(artSquircle.Get(), rim.Get(), 1.0f);
    } else {
        auto artBgBrush = m_dcomp->CreateLinearGradientBrush(
            core::Color{0.25f, 0.16f, 0.42f, alpha},
            core::Color{0.08f, 0.07f, 0.16f, alpha},
            D2D1::Point2F(artX, artY),
            D2D1::Point2F(artX + artSize, artY + artSize)
        );
        if (artSquircle && artBgBrush) {
            ctx->FillGeometry(artSquircle.Get(), artBgBrush.Get());
            auto rim = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 0.20f * alpha});
            ctx->DrawGeometry(artSquircle.Get(), rim.Get(), 1.0f);
        }

        // Vinyl concentric grooves
        auto vinylGroove = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 0.10f * alpha});
        D2D1_POINT_2F artCenter = D2D1::Point2F(artX + artSize * 0.5f, artY + artSize * 0.5f);
        ctx->DrawEllipse(D2D1::Ellipse(artCenter, 20.0f * m_dpiScale, 20.0f * m_dpiScale), vinylGroove.Get(), 1.0f);
        ctx->DrawEllipse(D2D1::Ellipse(artCenter, 13.0f * m_dpiScale, 13.0f * m_dpiScale), vinylGroove.Get(), 1.0f);

        auto vinylCenter = m_dcomp->CreateSolidBrush(core::Color{0.0f, 0.90f, 1.0f, 0.95f * alpha});
        ctx->FillEllipse(D2D1::Ellipse(artCenter, 4.0f * m_dpiScale, 4.0f * m_dpiScale), vinylCenter.Get());
    }

    // 2. Vector Brand Badge (Spotify, Opera GX, Chrome, Edge, etc.)
    float badgeW = 86.0f * m_dpiScale;
    float badgeH = 22.0f * m_dpiScale;
    float badgeX = bounds.Right() - 18.0f * m_dpiScale - badgeW;
    float badgeY = artY + 2.0f * m_dpiScale;
    core::Rect badgeRect{badgeX, badgeY, badgeW, badgeH};
    RenderBrandBadge(ctx, track.sourceApp, badgeRect, alpha);

    // 3. Fluid 6-Band Audio Spectrum Visualizer (Below Badge)
    float waveRightX = bounds.Right() - 18.0f * m_dpiScale;
    auto specBrush = m_dcomp->CreateLinearGradientBrush(
        core::Color{0.0f, 0.95f, 1.0f, alpha},      // Cyan
        core::Color{0.70f, 0.25f, 1.0f, alpha},    // Neon Purple
        D2D1::Point2F(waveRightX, artY + 54.0f),
        D2D1::Point2F(waveRightX, artY + 28.0f)
    );

    for (int i = 0; i < 6; ++i) {
        float h = (std::sin(m_equalizerPhases[i]) * 0.45f + std::sin(m_equalizerPhases[i] * 1.6f) * 0.35f + 0.35f) * 16.0f * m_dpiScale + 4.0f;
        float bx = waveRightX - (5 - i) * 6.0f * m_dpiScale;
        float by = artY + 42.0f * m_dpiScale;
        D2D1_ROUNDED_RECT barR = D2D1::RoundedRect(
            D2D1::RectF(bx, by - h * 0.5f, bx + 3.0f * m_dpiScale, by + h * 0.5f),
            1.5f, 1.5f
        );
        ctx->FillRoundedRectangle(barR, specBrush.Get());
    }

    // 4. Track Title (Segoe UI Variable Display Semi-Bold, bounded with ellipsis & vertical slide transition!)
    float textX = artX + artSize + 14.0f * m_dpiScale;
    float textRight = badgeX - 10.0f * m_dpiScale;
    float slideY = (1.0f - tProg) * 6.0f * m_dpiScale;
    float titleAlpha = alpha * tProg;
    float titleTop = artY + 4.0f * m_dpiScale + slideY;
    float titleBottom = artY + 27.0f * m_dpiScale + slideY;
    D2D1_RECT_F titleRectShadow = D2D1::RectF(textX + 1.0f, titleTop + 1.0f, textRight + 1.0f, titleBottom + 1.0f);
    D2D1_RECT_F titleRect = D2D1::RectF(textX, titleTop, textRight, titleBottom);
    auto shadowBrush = m_dcomp->CreateSolidBrush(core::Color{0.0f, 0.0f, 0.0f, 0.75f * titleAlpha});
    auto titleBrush = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, titleAlpha});
    DrawD2DText(ctx, track.title, m_fontTitle.Get(), titleRectShadow, shadowBrush.Get());
    DrawD2DText(ctx, track.title, m_fontTitle.Get(), titleRect, titleBrush.Get());

    // 5. Artist Name (Segoe UI Variable Text Regular, bounded with ellipsis & vertical slide transition!)
    float artistTop = artY + 30.0f * m_dpiScale + slideY;
    float artistBottom = artY + 48.0f * m_dpiScale + slideY;
    auto artistBrush = m_dcomp->CreateSolidBrush(core::Color{0.70f, 0.75f, 0.85f, titleAlpha});
    D2D1_RECT_F artistRect = D2D1::RectF(textX, artistTop, textRight, artistBottom);
    DrawD2DText(ctx, track.artist, m_fontBody.Get(), artistRect, artistBrush.Get());

    // 6. Scrubber Timeline & Dynamic 3-Stop Liquid Progress Bar
    float barX = artX;
    float barY = artY + artSize + 14.0f * m_dpiScale;
    float barW = bounds.width - 36.0f * m_dpiScale;
    float barH = 5.0f * m_dpiScale;
    m_scrubBar = core::Rect{barX, barY - 4.0f * m_dpiScale, barW, barH + 8.0f * m_dpiScale};

    bool isHoverScrub = m_scrubBar.Contains(m_mousePos);

    // Translucent background track
    auto trackBgBrush = m_dcomp->CreateSolidBrush(core::Color{0.18f, 0.20f, 0.28f, 0.70f * alpha});
    ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(barX, barY, barX + barW, barY + barH), barH * 0.5f, barH * 0.5f), trackBgBrush.Get());

    float progress = (track.durationSeconds > 0.0) ? static_cast<float>(track.positionSeconds / track.durationSeconds) : 0.0f;
    progress = std::clamp(progress, 0.0f, 1.0f);

    float activeW = barW * progress;
    if (activeW > 1.0f) {
        // 3-Stop Liquid Gradient: Electric Cyan (#00F2FE) -> Violet (#7C3AED) -> Magenta Pink (#EC4899)
        auto fillBrush = m_dcomp->CreateLinearGradientBrush(
            core::Color{0.0f, 0.95f, 1.0f, alpha},
            core::Color{0.92f, 0.28f, 0.60f, alpha},
            D2D1::Point2F(barX, barY),
            D2D1::Point2F(barX + barW, barY)
        );
        if (fillBrush) {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(barX, barY, barX + activeW, barY + barH), barH * 0.5f, barH * 0.5f), fillBrush.Get());
        }

        // Dynamic traveling specular shimmer wave while playing!
        if (track.status == media::PlaybackStatus::Playing) {
            float cycle = std::fmod(m_equalizerPhases[0] * 0.35f, 1.0f);
            float shimmerX = barX + activeW * cycle;
            float shimmerW = 28.0f * m_dpiScale;
            auto shimmerBrush = m_dcomp->CreateLinearGradientBrush(
                core::Color{1.0f, 1.0f, 1.0f, 0.0f},
                core::Color{1.0f, 1.0f, 1.0f, 0.40f * alpha},
                D2D1::Point2F(shimmerX - shimmerW * 0.5f, barY),
                D2D1::Point2F(shimmerX + shimmerW * 0.5f, barY)
            );
            if (shimmerBrush) {
                D2D1_RECT_F sRect = D2D1::RectF(
                    std::max(barX, shimmerX - shimmerW * 0.5f),
                    barY,
                    std::min(barX + activeW, shimmerX + shimmerW * 0.5f),
                    barY + barH
                );
                if (sRect.right > sRect.left) {
                    ctx->FillRoundedRectangle(D2D1::RoundedRect(sRect, barH * 0.5f, barH * 0.5f), shimmerBrush.Get());
                }
            }
        }
    }

    // Dynamic Multi-Layer Pulsing Neon Thumb
    float thumbX = barX + activeW;
    float thumbY = barY + barH * 0.5f;
    float pulse = std::sin(m_equalizerPhases[0] * 1.5f) * 0.5f + 0.5f;
    float haloRadius = (isHoverScrub ? 8.5f : (6.5f + pulse * 2.0f)) * m_dpiScale;

    // Neon breathing halo
    auto haloBrush = m_dcomp->CreateRadialGradientBrush(
        core::Color{0.0f, 0.90f, 1.0f, (0.28f + pulse * 0.20f) * alpha},
        core::Color{0.0f, 0.90f, 1.0f, 0.0f},
        D2D1::Point2F(thumbX, thumbY), haloRadius, haloRadius
    );
    if (haloBrush) {
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), haloRadius, haloRadius), haloBrush.Get());
    }

    // Pearl white solid thumb disc
    float coreRadius = (isHoverScrub ? 5.5f : 4.5f) * m_dpiScale;
    auto thumbCoreBrush = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 0.98f * alpha});
    ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), coreRadius, coreRadius), thumbCoreBrush.Get());

    // Time indicators (Real live ticking time!)
    wchar_t timeElapsed[16];
    int curSec = static_cast<int>(track.positionSeconds);
    swprintf_s(timeElapsed, L"%02d:%02d", curSec / 60, curSec % 60);
    D2D1_RECT_F elapsedRect = D2D1::RectF(barX, barY + 6.0f * m_dpiScale, barX + 60.0f * m_dpiScale, barY + 22.0f * m_dpiScale);
    DrawD2DText(ctx, timeElapsed, m_fontCaption.Get(), elapsedRect, artistBrush.Get());

    wchar_t timeTotal[16];
    int totSec = static_cast<int>(track.durationSeconds);
    if (totSec > 0) {
        swprintf_s(timeTotal, L"%02d:%02d", totSec / 60, totSec % 60);
    } else {
        swprintf_s(timeTotal, L"--:--");
    }
    D2D1_RECT_F totalRect = D2D1::RectF(barX + barW - 60.0f * m_dpiScale, barY + 6.0f * m_dpiScale, barX + barW, barY + 22.0f * m_dpiScale);
    DrawD2DText(ctx, timeTotal, m_fontCaption.Get(), totalRect, artistBrush.Get());

    // 7. Redesigned Premium Transport Controls Row
    float ctrlCenterY = bounds.Bottom() - 26.0f * m_dpiScale;
    float centerX = bounds.Center().x;

    bool isHoverPrev = m_btnPrev.Contains(m_mousePos);
    bool isHoverPlay = m_btnPlay.Contains(m_mousePos);
    bool isHoverNext = m_btnNext.Contains(m_mousePos);

    // --- PREVIOUS TRACK BUTTON (Solid filled, pointing LEFT |◀◀) ---
    float prevSize = 34.0f * m_dpiScale;
    m_btnPrev = core::Rect{centerX - 62.0f * m_dpiScale - prevSize * 0.5f, ctrlCenterY - prevSize * 0.5f, prevSize, prevSize};

    if (isHoverPrev) {
        auto hovBg = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 0.12f * alpha});
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(m_btnPrev.Center().x, m_btnPrev.Center().y), prevSize * 0.48f, prevSize * 0.48f), hovBg.Get());
    }

    auto ctrlBrush = m_dcomp->CreateSolidBrush(core::Color{0.92f, 0.95f, 1.0f, (isHoverPrev ? 1.0f : 0.85f) * alpha});
    float pcx = m_btnPrev.Center().x;
    float pcy = m_btnPrev.Center().y;

    // Solid Vertical Bar on left
    D2D1_ROUNDED_RECT barR = D2D1::RoundedRect(
        D2D1::RectF(pcx - 7.5f * m_dpiScale, pcy - 5.5f * m_dpiScale, pcx - 5.0f * m_dpiScale, pcy + 5.5f * m_dpiScale),
        1.2f * m_dpiScale, 1.2f * m_dpiScale
    );
    ctx->FillRoundedRectangle(barR, ctrlBrush.Get());

    // Solid Triangle pointing LEFT towards the vertical bar
    ComPtr<ID2D1PathGeometry> prevTri;
    m_dcomp->GetD2DFactory()->CreatePathGeometry(&prevTri);
    if (prevTri) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(prevTri->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(pcx - 4.0f * m_dpiScale, pcy), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(pcx + 5.5f * m_dpiScale, pcy - 5.5f * m_dpiScale));
            sink->AddLine(D2D1::Point2F(pcx + 5.5f * m_dpiScale, pcy + 5.5f * m_dpiScale));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
        }
        ctx->FillGeometry(prevTri.Get(), ctrlBrush.Get());
    }

    // --- PLAY / PAUSE BUTTON (Centerpiece: 42x42 Luminous Pearl White Disc with cyan ambient glow) ---
    float playSize = 42.0f * m_dpiScale;
    m_btnPlay = core::Rect{centerX - playSize * 0.5f, ctrlCenterY - playSize * 0.5f, playSize, playSize};
    D2D1_POINT_2F playCenter = D2D1::Point2F(centerX, ctrlCenterY);

    // Ambient soft glow under centerpiece button
    auto playHalo = m_dcomp->CreateRadialGradientBrush(
        core::Color{0.0f, 0.85f, 1.0f, (isHoverPlay ? 0.38f : 0.22f) * alpha},
        core::Color{0.0f, 0.85f, 1.0f, 0.0f},
        playCenter, playSize * 0.85f, playSize * 0.85f
    );
    if (playHalo) {
        ctx->FillEllipse(D2D1::Ellipse(playCenter, playSize * 0.85f, playSize * 0.85f), playHalo.Get());
    }

    // Pearl White Button Disc with crisp specular rim
    auto playDiscBrush = m_dcomp->CreateLinearGradientBrush(
        core::Color{1.0f, 1.0f, 1.0f, 0.98f * alpha},
        core::Color{0.92f, 0.94f, 0.97f, 0.98f * alpha},
        D2D1::Point2F(centerX, ctrlCenterY - playSize * 0.5f),
        D2D1::Point2F(centerX, ctrlCenterY + playSize * 0.5f)
    );
    if (playDiscBrush) {
        ctx->FillEllipse(D2D1::Ellipse(playCenter, playSize * 0.48f, playSize * 0.48f), playDiscBrush.Get());
        auto playRimBrush = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 0.50f * alpha});
        ctx->DrawEllipse(D2D1::Ellipse(playCenter, playSize * 0.48f, playSize * 0.48f), playRimBrush.Get(), 1.0f);
    }

    // Solid obsidian glyph inside pearl disc (#0A0D14)
    auto playIconBrush = m_dcomp->CreateSolidBrush(core::Color{0.04f, 0.05f, 0.08f, 0.96f * alpha});
    if (track.status == media::PlaybackStatus::Playing) {
        // Pause: two solid rounded capsules (❚❚)
        float pBarW = 3.6f * m_dpiScale;
        float pBarH = 13.5f * m_dpiScale;
        float gap = 4.0f * m_dpiScale;
        D2D1_ROUNDED_RECT bar1 = D2D1::RoundedRect(
            D2D1::RectF(centerX - gap * 0.5f - pBarW, ctrlCenterY - pBarH * 0.5f, centerX - gap * 0.5f, ctrlCenterY + pBarH * 0.5f),
            1.6f * m_dpiScale, 1.6f * m_dpiScale
        );
        D2D1_ROUNDED_RECT bar2 = D2D1::RoundedRect(
            D2D1::RectF(centerX + gap * 0.5f, ctrlCenterY - pBarH * 0.5f, centerX + gap * 0.5f + pBarW, ctrlCenterY + pBarH * 0.5f),
            1.6f * m_dpiScale, 1.6f * m_dpiScale
        );
        ctx->FillRoundedRectangle(bar1, playIconBrush.Get());
        ctx->FillRoundedRectangle(bar2, playIconBrush.Get());
    } else {
        // Play: rounded solid equilateral triangle pointing RIGHT (▶)
        float triSize = 13.0f * m_dpiScale;
        float triOffX = 1.5f * m_dpiScale; // Optical center correction
        D2D1_POINT_2F t1 = D2D1::Point2F(centerX - triSize * 0.35f + triOffX, ctrlCenterY - triSize * 0.50f);
        D2D1_POINT_2F t2 = D2D1::Point2F(centerX + triSize * 0.55f + triOffX, ctrlCenterY);
        D2D1_POINT_2F t3 = D2D1::Point2F(centerX - triSize * 0.35f + triOffX, ctrlCenterY + triSize * 0.50f);

        ComPtr<ID2D1PathGeometry> playTri;
        m_dcomp->GetD2DFactory()->CreatePathGeometry(&playTri);
        if (playTri) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(playTri->Open(&sink))) {
                sink->BeginFigure(t1, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(t2);
                sink->AddLine(t3);
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
            }
            ctx->FillGeometry(playTri.Get(), playIconBrush.Get());
        }
    }

    // --- NEXT TRACK BUTTON (Solid filled, pointing RIGHT ⏭) ---
    float nextSize = 34.0f * m_dpiScale;
    m_btnNext = core::Rect{centerX + 62.0f * m_dpiScale - nextSize * 0.5f, ctrlCenterY - nextSize * 0.5f, nextSize, nextSize};
    if (isHoverNext) {
        auto hovBg = m_dcomp->CreateSolidBrush(core::Color{1.0f, 1.0f, 1.0f, 0.12f * alpha});
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(m_btnNext.Center().x, m_btnNext.Center().y), nextSize * 0.48f, nextSize * 0.48f), hovBg.Get());
    }

    auto nextBrush = m_dcomp->CreateSolidBrush(core::Color{0.92f, 0.95f, 1.0f, (isHoverNext ? 1.0f : 0.85f) * alpha});
    float ncx = m_btnNext.Center().x;
    float ncy = m_btnNext.Center().y;

    // Solid Triangle pointing RIGHT towards the vertical bar
    ComPtr<ID2D1PathGeometry> nextTri;
    m_dcomp->GetD2DFactory()->CreatePathGeometry(&nextTri);
    if (nextTri) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(nextTri->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(ncx - 5.5f * m_dpiScale, ncy - 5.5f * m_dpiScale), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(ncx + 4.0f * m_dpiScale, ncy));
            sink->AddLine(D2D1::Point2F(ncx - 5.5f * m_dpiScale, ncy + 5.5f * m_dpiScale));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
        }
        ctx->FillGeometry(nextTri.Get(), nextBrush.Get());
    }

    // Solid Vertical Bar on right
    D2D1_ROUNDED_RECT nextBarR = D2D1::RoundedRect(
        D2D1::RectF(ncx + 5.0f * m_dpiScale, ncy - 5.5f * m_dpiScale, ncx + 7.5f * m_dpiScale, ncy + 5.5f * m_dpiScale),
        1.2f * m_dpiScale, 1.2f * m_dpiScale
    );
    ctx->FillRoundedRectangle(nextBarR, nextBrush.Get());
}

void IslandWindow::RenderSystemContent(ID2D1DeviceContext* ctx, const core::Rect& bounds) {
    float alpha = m_contentAlpha.GetValue();
    if (alpha <= 0.01f) return;

    auto metrics = system::MetricsCollector::Instance().GetMetrics();
    float colW = bounds.width / 3.0f;
    float centerY = bounds.Top() + 56.0f * m_dpiScale;
    float ringRadius = 26.0f * m_dpiScale;

    auto drawActivityRing = [&](float cx, float pct, const core::Color& colStart, const core::Color& colEnd, const wchar_t* label, const wchar_t* valueStr) {
        D2D1_POINT_2F center = D2D1::Point2F(cx, centerY);

        // 1. Inactive Track (dim background circle)
        auto trackBrush = m_dcomp->CreateSolidBrush(core::Color{0.18f, 0.20f, 0.25f, 0.35f * alpha});
        ctx->DrawEllipse(D2D1::Ellipse(center, ringRadius, ringRadius), trackBrush.Get(), 5.0f * m_dpiScale, m_roundedStroke.Get());

        // 2. Active Progress Arc (Apple Activity Ring style)
        if (pct > 0.5f) {
            float sweepAngle = std::clamp(pct / 100.0f, 0.01f, 1.0f) * 360.0f;
            float radStart = -90.0f * 3.14159265f / 180.0f;
            float radEnd = (-90.0f + sweepAngle) * 3.14159265f / 180.0f;

            D2D1_POINT_2F ptStart = D2D1::Point2F(
                center.x + ringRadius * std::cos(radStart),
                center.y + ringRadius * std::sin(radStart)
            );
            D2D1_POINT_2F ptEnd = D2D1::Point2F(
                center.x + ringRadius * std::cos(radEnd),
                center.y + ringRadius * std::sin(radEnd)
            );

            ComPtr<ID2D1PathGeometry> arcGeo;
            m_dcomp->GetD2DFactory()->CreatePathGeometry(&arcGeo);
            if (arcGeo) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(arcGeo->Open(&sink))) {
                    sink->BeginFigure(ptStart, D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddArc(D2D1::ArcSegment(
                        ptEnd,
                        D2D1::SizeF(ringRadius, ringRadius),
                        0.0f,
                        sweepAngle > 180.0f ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_CLOCKWISE,
                        sweepAngle > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL
                    ));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                }

                auto arcBrush = m_dcomp->CreateLinearGradientBrush(
                    colStart.WithAlpha(alpha), colEnd.WithAlpha(alpha),
                    D2D1::Point2F(center.x, center.y - ringRadius),
                    D2D1::Point2F(center.x, center.y + ringRadius)
                );
                if (arcBrush) {
                    ctx->DrawGeometry(arcGeo.Get(), arcBrush.Get(), 5.0f * m_dpiScale, m_roundedStroke.Get());
                }
            }
        }

        // 3. Central Percentage Text (Perfect horizontal & vertical centering)
        auto textBrush = m_dcomp->CreateSolidBrush(core::Color{0.96f, 0.96f, 0.98f, alpha});
        D2D1_RECT_F valRect = D2D1::RectF(cx - ringRadius, centerY - ringRadius, cx + ringRadius, centerY + ringRadius);
        DrawD2DText(ctx, valueStr, m_fontCenterHero.Get(), valRect, textBrush.Get());

        // 4. Subtitle Label (Centered under ring)
        auto labelBrush = m_dcomp->CreateSolidBrush(core::Color{0.55f, 0.60f, 0.70f, alpha});
        D2D1_RECT_F lblRect = D2D1::RectF(cx - ringRadius - 15.0f, centerY + ringRadius + 6.0f * m_dpiScale, cx + ringRadius + 15.0f, centerY + ringRadius + 22.0f * m_dpiScale);
        DrawD2DText(ctx, label, m_fontCenterCaption.Get(), lblRect, labelBrush.Get());
    };

    // 1. CPU Ring (Cyan to Fluent Blue)
    wchar_t cpuVal[16];
    swprintf_s(cpuVal, L"%d%%", static_cast<int>(metrics.cpuUsagePercent + 0.5f));
    drawActivityRing(
        bounds.Left() + colW * 0.5f,
        metrics.cpuUsagePercent,
        core::Color{0.0f, 0.90f, 1.0f},
        core::Color{0.0f, 0.45f, 0.90f},
        L"CPU CORE",
        cpuVal
    );

    // 2. RAM Ring (Emerald to Mint)
    wchar_t ramVal[16];
    swprintf_s(ramVal, L"%d%%", static_cast<int>(metrics.ramUsagePercent + 0.5f));
    drawActivityRing(
        bounds.Left() + colW * 1.5f,
        metrics.ramUsagePercent,
        core::Color{0.10f, 0.92f, 0.50f},
        core::Color{0.05f, 0.75f, 0.35f},
        L"MEM LOAD",
        ramVal
    );

    // 3. GPU Ring (Neon Violet to Magenta)
    wchar_t gpuVal[16];
    swprintf_s(gpuVal, L"%d%%", static_cast<int>(metrics.gpuUsagePercent + 0.5f));
    drawActivityRing(
        bounds.Left() + colW * 2.5f,
        metrics.gpuUsagePercent,
        core::Color{0.80f, 0.30f, 1.0f},
        core::Color{0.95f, 0.15f, 0.65f},
        L"GPU 4060",
        gpuVal
    );
}

void IslandWindow::RenderVolumeContent(ID2D1DeviceContext* ctx, const core::Rect& bounds) {
    float alpha = m_contentAlpha.GetValue();
    if (alpha <= 0.01f) return;

    float vol = media::MediaService::Instance().GetVolume();
    float centerY = bounds.Center().y;

    auto textBrush = m_dcomp->CreateSolidBrush(core::Color{0.96f, 0.96f, 0.98f, alpha});
    D2D1_RECT_F iconRect = D2D1::RectF(bounds.Left() + 16.0f * m_dpiScale, centerY - 10.0f * m_dpiScale, bounds.Left() + 52.0f * m_dpiScale, centerY + 10.0f * m_dpiScale);
    DrawD2DText(ctx, L"VOL", m_fontTitle.Get(), iconRect, textBrush.Get());

    // Volume Capsule Track
    float barX = bounds.Left() + 56.0f * m_dpiScale;
    float barW = bounds.width - 116.0f * m_dpiScale;
    float barH = 6.0f * m_dpiScale;

    auto bgBrush = m_dcomp->CreateSolidBrush(core::Color{0.18f, 0.20f, 0.26f, alpha});
    ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(barX, centerY - barH * 0.5f, barX + barW, centerY + barH * 0.5f), 3.0f, 3.0f), bgBrush.Get());

    auto accentBrush = m_dcomp->CreateLinearGradientBrush(
        core::Color{0.0f, 0.85f, 1.0f, alpha},
        core::Color{0.15f, 0.50f, 1.0f, alpha},
        D2D1::Point2F(barX, centerY),
        D2D1::Point2F(barX + barW, centerY)
    );
    if (accentBrush) {
        ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(barX, centerY - barH * 0.5f, barX + barW * vol, centerY + barH * 0.5f), 3.0f, 3.0f), accentBrush.Get());
    }

    wchar_t volBuf[16];
    swprintf_s(volBuf, L"%d%%", static_cast<int>(vol * 100.0f + 0.5f));
    D2D1_RECT_F valRect = D2D1::RectF(bounds.Right() - 50.0f * m_dpiScale, centerY - 10.0f * m_dpiScale, bounds.Right() - 10.0f * m_dpiScale, centerY + 10.0f * m_dpiScale);
    DrawD2DText(ctx, volBuf, m_fontCenterCaption.Get(), valRect, textBrush.Get());
}

void IslandWindow::RenderNotificationContent(ID2D1DeviceContext* ctx, const core::Rect& bounds) {
    float alpha = m_contentAlpha.GetValue();
    if (alpha <= 0.01f) return;

    float badgeSize = 38.0f * m_dpiScale;
    float badgeX = bounds.Left() + 16.0f * m_dpiScale;
    float badgeY = bounds.Center().y - badgeSize * 0.5f;

    auto badgeBrush = m_dcomp->CreateSolidBrush(core::Color::FromHex(m_activeNotification.iconColor).WithAlpha(alpha));
    ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(badgeX, badgeY, badgeX + badgeSize, badgeY + badgeSize), 10.0f * m_dpiScale, 10.0f * m_dpiScale), badgeBrush.Get());

    float textX = badgeX + badgeSize + 14.0f * m_dpiScale;
    auto titleBrush = m_dcomp->CreateSolidBrush(core::Color{0.98f, 0.98f, 0.99f, alpha});
    D2D1_RECT_F titleRect = D2D1::RectF(textX, bounds.Top() + 14.0f * m_dpiScale, bounds.Right() - 16.0f * m_dpiScale, bounds.Top() + 34.0f * m_dpiScale);
    DrawD2DText(ctx, m_activeNotification.title, m_fontTitle.Get(), titleRect, titleBrush.Get());

    auto bodyBrush = m_dcomp->CreateSolidBrush(core::Color{0.65f, 0.68f, 0.75f, alpha});
    D2D1_RECT_F bodyRect = D2D1::RectF(textX, bounds.Top() + 36.0f * m_dpiScale, bounds.Right() - 16.0f * m_dpiScale, bounds.Bottom() - 12.0f * m_dpiScale);
    DrawD2DText(ctx, m_activeNotification.message, m_fontBody.Get(), bodyRect, bodyBrush.Get());
}

LRESULT IslandWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST: {
            int x = static_cast<short>(LOWORD(lParam));
            int y = static_cast<short>(HIWORD(lParam));
            POINT pt = {x, y};
            ScreenToClient(hwnd, &pt);
            core::Vec2 clickPt{static_cast<float>(pt.x), static_cast<float>(pt.y)};

            if (m_boundsSpring.GetValue().Contains(clickPt)) {
                return HTCLIENT;
            }
            return HTTRANSPARENT;
        }

        case WM_MOUSEMOVE: {
            int x = static_cast<short>(LOWORD(lParam));
            int y = static_cast<short>(HIWORD(lParam));
            core::Vec2 movePt{static_cast<float>(x), static_cast<float>(y)};
            m_mousePos = movePt;

            bool inside = m_boundsSpring.GetValue().Contains(movePt);
            if (inside) {
                if (!m_isHovered) {
                    m_isHovered = true;
                    TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
                    TrackMouseEvent(&tme);
                }
            } else {
                if (m_isHovered) {
                    m_isHovered = false;
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            m_isHovered = false;
            m_mousePos = core::Vec2{-1.0f, -1.0f};
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            core::Vec2 clickPt{static_cast<float>(pt.x), static_cast<float>(pt.y)};

            if (m_currentMode == IslandMode::Idle) {
                auto track = media::MediaService::Instance().GetCurrentTrack();
                if (track.hasMedia && track.status == media::PlaybackStatus::Playing) {
                    SetMode(IslandMode::MediaExpanded);
                } else {
                    SetMode(IslandMode::SystemExpanded);
                }
                m_isPinned = true;
            } else if (m_currentMode == IslandMode::MediaExpanded) {
                // Interactive buttons and scrubber in MediaExpanded mode
                if (m_scrubBar.Contains(clickPt)) {
                    float frac = (clickPt.x - m_scrubBar.x) / m_scrubBar.width;
                    frac = std::clamp(frac, 0.0f, 1.0f);
                    auto track = media::MediaService::Instance().GetCurrentTrack();
                    if (track.durationSeconds > 0.0) {
                        media::MediaService::Instance().Seek(frac * track.durationSeconds);
                    }
                } else if (m_btnPlay.Contains(clickPt)) {
                    media::MediaService::Instance().PlayPause();
                } else if (m_btnPrev.Contains(clickPt)) {
                    media::MediaService::Instance().PreviousTrack();
                } else if (m_btnNext.Contains(clickPt)) {
                    media::MediaService::Instance().NextTrack();
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

        case WM_RBUTTONUP: {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 1001, L"Open AETHER Settings Center");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, 1002, L"Toggle Desktop Widgets");
            AppendMenuW(hMenu, MF_STRING, 1003, L"Cycle Island Mode");
            AppendMenuW(hMenu, MF_STRING, 1004, L"Test Notification");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, 1005, L"Exit Aether");

            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);

            if (cmd == 1001) {
                PostMessage(hwnd, WM_USER + 100, 0, 0);
            } else if (cmd == 1003) {
                if (m_currentMode == IslandMode::Idle) SetMode(IslandMode::MediaExpanded);
                else if (m_currentMode == IslandMode::MediaExpanded) SetMode(IslandMode::SystemExpanded);
                else SetMode(IslandMode::Idle);
            } else if (cmd == 1004) {
                NotificationData notif;
                notif.title = L"System Status";
                notif.message = L"AETHER running at 240Hz with hardware DirectComposition.";
                notif.iconColor = 0xFF00D2FF;
                PushNotification(notif);
            } else if (cmd == 1005) {
                PostQuitMessage(0);
            }
            return 0;
        }

        case WM_DPICHANGED: {
            m_dpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;
            UpdateTargetBounds();
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
