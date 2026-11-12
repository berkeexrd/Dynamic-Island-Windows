#pragma once

#include "aether/config/config_manager.hpp"

namespace aether::ui {

enum class SettingsTab {
    General,
    DynamicIsland,
    Widgets,
    Taskbar,
    Desktop,
    Appearance,
    Animations,
    Media,
    SystemMonitoring,
    GamingMode,
    Startup,
    Performance,
    About
};

class SettingsPages {
public:
    static void Render(SettingsTab activeTab);

private:
    static void RenderGeneral();
    static void RenderDynamicIsland();
    static void RenderWidgets();
    static void RenderTaskbar();
    static void RenderDesktop();
    static void RenderAppearance();
    static void RenderAnimations();
    static void RenderMedia();
    static void RenderSystemMonitoring();
    static void RenderGamingMode();
    static void RenderStartup();
    static void RenderPerformance();
    static void RenderAbout();
};

} // namespace aether::ui
