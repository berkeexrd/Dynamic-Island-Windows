#pragma once

#include <imgui.h>
#include <cstdint>

namespace aether::ui {

// Interface scale (system DPI / 96); the settings window multiplies every pixel size by it.
inline float Scale = 1.0f;
inline float Sc(float v) { return v * Scale; }

// Master Rules v4: Spacing tokens
namespace Spacing {
    constexpr float XS = 4.0f;
    constexpr float SM = 8.0f;
    constexpr float MD = 12.0f;
    constexpr float LG = 16.0f;
    constexpr float XL = 24.0f;
    constexpr float XXL = 32.0f;
}

// Master Rules v4: Radius tokens
namespace Radius {
    constexpr float None = 0.0f;
    constexpr float XS = 4.0f;
    constexpr float SM = 6.0f;
    constexpr float MD = 8.0f;
    constexpr float LG = 10.0f;
    constexpr float XL = 12.0f;
}

// Master Rules v4: Component sizing tokens
namespace Sizing {
    constexpr float Compact = 28.0f;
    constexpr float Standard = 34.0f;
    constexpr float Large = 40.0f;
}

struct ThemeColors {
    // Tone / Depth: 0 Background -> 1 Shell -> 2 Surface/Cards -> 3 Elevated/Controls -> 4 Popup/Modal
    ImVec4 Background{0.07f, 0.07f, 0.09f, 1.0f};       // L0
    ImVec4 Shell{0.09f, 0.09f, 0.12f, 1.0f};            // L1 Sidebar/Shell
    ImVec4 Surface{0.12f, 0.12f, 0.15f, 1.0f};          // L2 Content Surface
    ImVec4 SurfaceElevated{0.15f, 0.15f, 0.19f, 1.0f};  // L2 Cards
    ImVec4 SurfaceHover{0.18f, 0.18f, 0.23f, 1.0f};
    ImVec4 SurfaceActive{0.21f, 0.21f, 0.27f, 1.0f};
    ImVec4 SurfaceOverlay{0.13f, 0.13f, 0.17f, 0.96f};

    // Typography hierarchy
    ImVec4 TextPrimary{0.95f, 0.95f, 0.97f, 1.0f};
    ImVec4 TextSecondary{0.70f, 0.72f, 0.78f, 1.0f};
    ImVec4 TextMuted{0.45f, 0.47f, 0.54f, 1.0f};
    ImVec4 TextDisabled{0.30f, 0.32f, 0.38f, 1.0f};

    // Borders
    ImVec4 Border{0.18f, 0.18f, 0.23f, 1.0f};
    ImVec4 BorderSubtle{0.14f, 0.14f, 0.18f, 1.0f};
    ImVec4 BorderHover{0.28f, 0.28f, 0.35f, 1.0f};
    ImVec4 BorderFocus{0.0f, 0.47f, 0.83f, 1.0f}; // Accent bound

    // Accent (Runtime customizable, propagates everywhere)
    ImVec4 Accent{0.0f, 0.47f, 0.83f, 1.0f};       // Default Fluent Blue
    ImVec4 AccentHover{0.10f, 0.55f, 0.92f, 1.0f};
    ImVec4 AccentActive{0.0f, 0.40f, 0.72f, 1.0f};
    ImVec4 AccentMuted{0.0f, 0.47f, 0.83f, 0.18f};
    ImVec4 AccentGlow{0.0f, 0.47f, 0.83f, 0.35f};

    // Semantic Status
    ImVec4 Success{0.15f, 0.78f, 0.45f, 1.0f};
    ImVec4 Warning{0.95f, 0.65f, 0.12f, 1.0f};
    ImVec4 Error{0.92f, 0.26f, 0.26f, 1.0f};
    ImVec4 Info{0.20f, 0.60f, 0.95f, 1.0f};

    // Shadows
    ImVec4 ShadowSoft{0.0f, 0.0f, 0.0f, 0.25f};
    ImVec4 ShadowMedium{0.0f, 0.0f, 0.0f, 0.45f};
    ImVec4 ShadowStrong{0.0f, 0.0f, 0.0f, 0.70f};
    ImVec4 BackdropDim{0.0f, 0.0f, 0.0f, 0.55f};
};

class Theme {
public:
    static Theme& Instance() {
        static Theme instance;
        return instance;
    }

    void Apply();
    void SetAccentColor(uint32_t hexRgba);
    void UseGlassTheme(int index);   // switches the shared glass theme and re-tints the whole settings UI
    [[nodiscard]] int ThemeIndex() const { return m_themeIndex; }
    [[nodiscard]] const ThemeColors& Colors() const { return m_colors; }

private:
    Theme();
    ThemeColors m_colors;
    int m_themeIndex{0};
};

// Fonts loaded by the settings window (null = ImGui default)
inline ImFont* FontBold = nullptr;
inline ImFont* FontIcons = nullptr;   // Segoe Fluent Icons (Windows 11), nullptr when missing
inline ImFont* FontTitle = nullptr;
inline ImFont* FontSmall = nullptr;

} // namespace aether::ui
