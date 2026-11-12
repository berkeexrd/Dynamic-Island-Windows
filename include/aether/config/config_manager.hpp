#pragma once

#include <string>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include "aether/core/types.hpp"

namespace aether::config {

enum class IslandExpandTrigger {
    Hover,
    Click,
    AlwaysManual
};

enum class TaskbarStyle {
    Default,
    Transparent,
    Blur,
    Acrylic,
    CustomTint
};

enum class GamingModeAction {
    HideCompletely,
    CompactOnly,
    SuppressAnimations
};

enum class ThemePreset {
    AetherDark,
    ObsidianOLED,
    NordicFrost,
    CyberpunkNeon,
    WindowsMica
};

struct DynamicIslandConfig {
    bool enabled{true};
    float idleWidth{156.0f};
    float idleHeight{36.0f};
    float expandedMediaWidth{380.0f};
    float expandedMediaHeight{130.0f};
    float expandedSystemWidth{340.0f};
    float expandedSystemHeight{120.0f};
    float expandedVolumeWidth{260.0f};
    float expandedVolumeHeight{44.0f};
    float topOffset{14.0f};
    bool expandOnHover{true};
    float hoverDelayMs{120.0f};
    bool pinOnClick{true};
    bool showClockIdle{true};
    bool showAudioVisualizer{true};
    bool mediaAutoExpand{false};
    float notificationDismissSeconds{4.0f};
};

struct DesktopWidgetConfig {
    bool enabled{true};
    core::Vec2 position{40.0f, 60.0f};
    core::Vec2 size{220.0f, 110.0f};
    float opacity{0.92f};
    bool locked{false};
};

struct WidgetsConfig {
    bool enabled{true};
    bool snapToGrid{true};
    float gridSize{16.0f};
    DesktopWidgetConfig sysMonWidget{true, {40.0f, 60.0f}, {230.0f, 130.0f}, 0.90f, false};
    DesktopWidgetConfig clockWidget{true, {40.0f, 210.0f}, {230.0f, 90.0f}, 0.90f, false};
    DesktopWidgetConfig netWidget{true, {40.0f, 320.0f}, {230.0f, 90.0f}, 0.90f, false};
    DesktopWidgetConfig mediaWidget{false, {40.0f, 430.0f}, {280.0f, 110.0f}, 0.90f, false};
};

struct TaskbarConfig {
    bool enabled{true};
    TaskbarStyle style{TaskbarStyle::Acrylic};
    uint32_t tintColor{0xCC18181B}; // RGBA
    bool roundElements{true};
    bool autoHide{false};
};

struct AppearanceConfig {
    ThemePreset preset{ThemePreset::AetherDark};
    uint32_t accentColor{0xFF0078D4}; // Fluent Blue
    uint32_t secondaryAccent{0xFF7000FF};
    float cornerRadius{18.0f};
    float backgroundOpacity{0.92f};
    float glowIntensity{0.35f};
    float shadowIntensity{0.60f};
    bool enableGlow{true};
    bool enableShadow{true};
    bool enableBlur{true};
};

struct AnimationConfig {
    float springStiffness{280.0f};
    float springDamping{24.0f};
    float springMass{1.0f};
    float timeScale{1.0f};
    bool reducedMotion{false};
};

struct GamingModeConfig {
    bool enabled{true};
    bool autoDetectFullscreen{true};
    GamingModeAction action{GamingModeAction::HideCompletely};
    bool pauseWidgets{true};
    bool suppressNotifications{true};
};

struct AppConfig {
    bool launchOnStartup{false};
    bool minimizeToTray{true};
    int monitorIndex{0};
    float uiScale{1.0f};
    DynamicIslandConfig island;
    WidgetsConfig widgets;
    TaskbarConfig taskbar;
    AppearanceConfig appearance;
    AnimationConfig animation;
    GamingModeConfig gaming;
};

class ConfigManager {
public:
    static ConfigManager& Instance() {
        static ConfigManager instance;
        return instance;
    }

    void Initialize(const std::filesystem::path& configPath = "");
    bool Save();
    bool Load();
    void ResetToDefaults();

    [[nodiscard]] const AppConfig& Get() const { return m_config; }
    AppConfig& GetMutable() { return m_config; }

    void SetConfig(const AppConfig& newConfig);

private:
    ConfigManager() = default;

    std::filesystem::path GetDefaultConfigPath() const;

    AppConfig m_config;
    std::filesystem::path m_configPath;
    mutable std::recursive_mutex m_mutex;
};

} // namespace aether::config
