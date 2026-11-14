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
    void PlayIntro();   // start-up: the island drops in from above the screen edge

    // True when nothing at all needs a new frame (the main loop can sleep).
    [[nodiscard]] bool IsResting() const;
    // True while springs / hover / drags are in flight and full refresh rate is wanted. When
    // IsResting() is false but this is false too, only ambient motion (visualizer, marquee) is
    // running and ~60 fps is plenty.
    [[nodiscard]] bool IsSpringDriven() const;

    [[nodiscard]] HWND GetHwnd() const { return m_hwnd; }
    [[nodiscard]] bool IsHovered() const { return m_isHovered; }
    [[nodiscard]] bool IsPinned() const { return m_isPinned; }

    void SetPinned(bool pinned) { m_isPinned = pinned; }

private:
    enum class Control { None, Prev, Play, Next, Scrub, Volume };

    struct TextBlock {
        ComPtr<IDWriteTextLayout> layout;
        float width{0.0f};
        float height{0.0f};
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // ---- layout / lifecycle ------------------------------------------------------------------
    void CreateFonts();
    void PositionWindow();
    void UpdateTargetBounds();
    void UpdateWindowRegion(const core::Rect& bounds);
    int  LayoutSignature() const;

    // ---- media / data ------------------------------------------------------------------------
    void UpdateAlbumArt(const std::vector<uint8_t>& bytes);
    void RebuildTrackText();
    bool BuildTextBlock(const std::wstring& text, IDWriteTextFormat* fmt, TextBlock& out);

    // ---- rendering ---------------------------------------------------------------------------
    ComPtr<ID2D1CommandList> RecordShadow(ID2D1DeviceContext* ctx, ID2D1Geometry* geo);
    ComPtr<ID2D1CommandList> RecordContent(ID2D1DeviceContext* ctx, const core::Rect& bounds);
    void DrawContent(ID2D1DeviceContext* ctx, const core::Rect& bounds);

    void DrawIdle(ID2D1DeviceContext* ctx, const core::Rect& bounds);
    void DrawPeek(ID2D1DeviceContext* ctx, const core::Rect& bounds);
    void DrawMedia(ID2D1DeviceContext* ctx, const core::Rect& bounds);
    void DrawSystem(ID2D1DeviceContext* ctx, const core::Rect& bounds);
    void DrawVolume(ID2D1DeviceContext* ctx, const core::Rect& bounds);
    void DrawNotification(ID2D1DeviceContext* ctx, const core::Rect& bounds);

    // Small building blocks
    ID2D1SolidColorBrush* Br(const core::Color& c);
    void DrawLabel(ID2D1DeviceContext* ctx, const std::wstring& text, IDWriteTextFormat* fmt,
                   const D2D1_RECT_F& rect, const core::Color& color);
    void DrawGlyph(ID2D1DeviceContext* ctx, wchar_t glyph, const D2D1_RECT_F& rect, const core::Color& color);
    void DrawMarquee(ID2D1DeviceContext* ctx, const TextBlock& block, float left, float top,
                     float availWidth, const core::Color& color);
    void DrawArt(ID2D1DeviceContext* ctx, const core::Rect& rect, float cornerRadius, float scale, float alpha);
    void DrawVisualizer(ID2D1DeviceContext* ctx, float rightX, float centerY, float maxHeight,
                        int bars, const core::Color& color);
    void DrawPrivacyDots(ID2D1DeviceContext* ctx, float rightX, float centerY);
    void DrawPlayPause(ID2D1DeviceContext* ctx, const core::Vec2& c, float size, float mix, float alpha);
    void DrawSkip(ID2D1DeviceContext* ctx, const core::Vec2& c, float size, bool forward, float alpha);
    void FillTriangle(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& a, const D2D1_POINT_2F& b,
                      const D2D1_POINT_2F& c, const core::Color& color, float rounding);

    // ---- interaction -------------------------------------------------------------------------
    Control HitTestControl(const core::Vec2& pt) const;
    void ApplyDrag(const core::Vec2& pt);
    void EndDrag(bool commit);

    // ---- state -------------------------------------------------------------------------------
    HWND m_hwnd{nullptr};
    std::unique_ptr<render::DCompLayer> m_dcomp;

    IslandMode m_currentMode{IslandMode::Idle};
    IslandMode m_targetMode{IslandMode::Idle};

    // Animated geometry & content
    animation::Spring<core::Rect> m_boundsSpring;
    animation::Spring<float> m_radiusSpring;
    animation::Spring<float> m_contentAlpha;
    animation::Spring<float> m_trackAnimProgress;
    animation::Spring<float> m_playMix;        // 0 = "play" glyph, 1 = "pause" glyph
    animation::Spring<float> m_scrubThick;     // 0..1 scrubber thickness (hover / drag)
    animation::Spring<float> m_volDisplay;
    animation::Spring<float> m_ringSpring[3];  // CPU / RAM / GPU
    animation::Spring<float> m_pressSpring[3]; // prev / play / next press scale

    core::Rect m_lastRegionBounds{-1.0f, -1.0f, -1.0f, -1.0f};

    bool m_isHovered{false};
    bool m_isPinned{false};
    bool m_isGamingSuspended{false};
    core::Vec2 m_mousePos{-1.0f, -1.0f};
    float m_hoverTimer{0.0f};
    float m_unhoverTimer{0.0f};

    float m_volumeHudTimer{0.0f};
    float m_peekTimer{0.0f};
    float m_marqueeTime{0.0f};
    bool  m_marqueeActive{false};
    NotificationData m_activeNotification;

    // Latest UI-thread snapshot of the media session (no album art bytes)
    media::MediaTrackInfo m_track;
    bool m_micActive{false};
    bool m_camActive{false};
    int  m_layoutSig{0};
    std::wstring m_lastTrackTitle;
    std::wstring m_lastTrackArtist;

    // Typography
    ComPtr<IDWriteTextFormat> m_fontTitle;       // track title (layout based, top aligned)
    ComPtr<IDWriteTextFormat> m_fontArtist;      // artist (layout based, top aligned)
    ComPtr<IDWriteTextFormat> m_fontClock;
    ComPtr<IDWriteTextFormat> m_fontRingValue;
    ComPtr<IDWriteTextFormat> m_fontCaption;
    ComPtr<IDWriteTextFormat> m_fontTimeLeft;
    ComPtr<IDWriteTextFormat> m_fontTimeRight;
    ComPtr<IDWriteTextFormat> m_fontNotifTitle;
    ComPtr<IDWriteTextFormat> m_fontNotifBody;
    ComPtr<IDWriteTextFormat> m_fontInitial;
    ComPtr<IDWriteTextFormat> m_fontIcon;
    bool m_hasIconFont{false};

    TextBlock m_titleBlock;
    TextBlock m_artistBlock;

    // Album art + colour extracted from it
    ComPtr<ID2D1Bitmap1> m_albumArt;
    float m_artW{0.0f};
    float m_artH{0.0f};
    uint64_t m_artVersion{0};
    core::Color m_accent{0.93f, 0.93f, 0.96f, 1.0f};
    core::Color m_accentTarget{0.93f, 0.93f, 0.96f, 1.0f};

    // Interactive control bounds (updated every frame the media view is drawn)
    core::Rect m_btnPrev;
    core::Rect m_btnPlay;
    core::Rect m_btnNext;
    core::Rect m_scrubBar;
    core::Rect m_volBar;
    Control m_pressed{Control::None};
    Control m_dragging{Control::None};
    float m_dragFraction{0.0f};

    // Direct2D resources
    ComPtr<ID2D1SolidColorBrush> m_solid;
    ComPtr<ID2D1StrokeStyle> m_roundedStroke;
    ComPtr<ID2D1Effect> m_shadowEffect;
    ComPtr<ID2D1Effect> m_blurEffect;

    // Visualizer state
    float m_barLevels[6]{};
    float m_barPhase[6]{0.0f, 1.3f, 2.6f, 3.9f, 5.2f, 0.7f};

    int m_affinityApplied{-1};     // last display-affinity value applied (capture visibility)
    int m_monitorApplied{-1};      // last monitor index applied
    uint32_t m_windowWidth{880};
    uint32_t m_windowHeight{300};
    float m_dpiScale{1.0f};
};

} // namespace aether::island
