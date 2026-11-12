#pragma once

#include <windows.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <vector>
#include "aether/core/types.hpp"
#include "aether/island/island_state.hpp"
#include "aether/animation/spring.hpp"
#include "aether/render/dcomp_layer.hpp"
#include "aether/media/media_service.hpp"
#include "aether/system/metrics_collector.hpp"

namespace aether::island {

using Microsoft::WRL::ComPtr;

class IslandWindow {
public:
    IslandWindow();
    ~IslandWindow();

    bool Create();
    void Show(bool show = true);
    void Update(float dt);
    void Render();

    void SetMode(IslandMode mode);
    [[nodiscard]] IslandMode GetMode() const { return m_currentMode; }

    void PushNotification(const NotificationData& notif);
    void TriggerVolumeHUD(float volume);
    void SetGamingSuspended(bool suspended);

    [[nodiscard]] bool IsResting() const;
    [[nodiscard]] HWND GetHwnd() const { return m_hwnd; }
    [[nodiscard]] bool IsHovered() const { return m_isHovered; }
    [[nodiscard]] bool IsPinned() const { return m_isPinned; }

    void SetPinned(bool pinned) { m_isPinned = pinned; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void UpdateTargetBounds();
    void UpdateWindowRegion(const core::Rect& bounds, float radius);
    void CaptureDesktopBackground();
    void CleanupCapture();
    void RenderBackground(ID2D1DeviceContext* ctx, const core::Rect& bounds, float radius);
    void RenderIdleContent(ID2D1DeviceContext* ctx, const core::Rect& bounds);
    void RenderMediaContent(ID2D1DeviceContext* ctx, const core::Rect& bounds);
    void RenderSystemContent(ID2D1DeviceContext* ctx, const core::Rect& bounds);
    void RenderVolumeContent(ID2D1DeviceContext* ctx, const core::Rect& bounds);
    void RenderNotificationContent(ID2D1DeviceContext* ctx, const core::Rect& bounds);
    void RenderBrandBadge(ID2D1DeviceContext* ctx, const std::wstring& sourceApp, const core::Rect& rect, float alpha);
    void UpdateAlbumArt(const std::vector<uint8_t>& bytes);

    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dcomp;

    IslandMode m_currentMode{IslandMode::Idle};
    IslandMode m_targetMode{IslandMode::Idle};

    // Animated geometry
    animation::Spring<core::Rect> m_boundsSpring;
    animation::Spring<float> m_radiusSpring;
    animation::Spring<float> m_contentAlpha;
    animation::Spring<float> m_trackAnimProgress;
    core::Rect m_lastRegionBounds{-1.0f, -1.0f, -1.0f, -1.0f};
    std::wstring m_lastTrackTitle{L""};

    bool m_isHovered{false};
    bool m_isPinned{false};
    bool m_isGamingSuspended{false};
    core::Vec2 m_mousePos{-1.0f, -1.0f};
    float m_hoverTimer{0.0f};
    float m_unhoverTimer{0.0f};

    float m_volumeHudTimer{0.0f};
    NotificationData m_activeNotification;

    // DirectWrite Upgraded Typography Hierarchy
    ComPtr<IDWriteTextFormat> m_fontHero;
    ComPtr<IDWriteTextFormat> m_fontTitle;
    ComPtr<IDWriteTextFormat> m_fontBody;
    ComPtr<IDWriteTextFormat> m_fontCaption;
    ComPtr<IDWriteTextFormat> m_fontClock;
    ComPtr<IDWriteTextFormat> m_fontIdleTitle;
    ComPtr<IDWriteTextFormat> m_fontCenterHero;
    ComPtr<IDWriteTextFormat> m_fontCenterTitle;
    ComPtr<IDWriteTextFormat> m_fontCenterCaption;

    // Album art decoded GPU bitmap
    ComPtr<ID2D1Bitmap1> m_albumArtBitmap;
    size_t m_lastArtHash{0};

    // Interactive media control bounds
    core::Rect m_btnPrev;
    core::Rect m_btnPlay;
    core::Rect m_btnNext;
    core::Rect m_scrubBar;

    // Rounded stroke style for radial gauges and curves
    ComPtr<ID2D1StrokeStyle> m_roundedStroke;

    // Equalizer animation state (10 bands)
    float m_equalizerPhases[10]{0.0f, 0.3f, 0.6f, 0.9f, 1.2f, 1.5f, 1.8f, 2.1f, 2.4f, 2.7f};

    // Real GPU Frosted Glass Blur & Desktop Snapshot
    HDC m_memDC{nullptr};
    HBITMAP m_memBmp{nullptr};
    void* m_bmpBits{nullptr};
    uint32_t m_captureWidth{0};
    uint32_t m_captureHeight{0};
    float m_captureTimer{0.0f};
    ComPtr<ID2D1Bitmap1> m_desktopBitmap;
    ComPtr<ID2D1Effect> m_blurEffect;

    uint32_t m_windowWidth{800};
    uint32_t m_windowHeight{260};
    float m_dpiScale{1.0f};
};

} // namespace aether::island
