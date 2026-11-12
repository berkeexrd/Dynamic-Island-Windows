#include "aether/config/config_manager.hpp"
#include "aether/core/log.hpp"
#include "aether/core/events.hpp"
#include <fstream>
#include <shlobj.h>

namespace aether::config {

struct ConfigUpdatedEvent {
    const AppConfig& config;
};

void ConfigManager::Initialize(const std::filesystem::path& configPath) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!configPath.empty()) {
        m_configPath = configPath;
    } else {
        m_configPath = GetDefaultConfigPath();
    }

    if (std::filesystem::exists(m_configPath)) {
        Load();
    } else {
        ResetToDefaults();
        Save();
    }
}

std::filesystem::path ConfigManager::GetDefaultConfigPath() const {
    PWSTR appDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath))) {
        std::filesystem::path path(appDataPath);
        CoTaskMemFree(appDataPath);
        path /= "Aether";
        std::filesystem::create_directories(path);
        path /= "aether_config.json";
        return path;
    }
    return std::filesystem::current_path() / "aether_config.json";
}

void ConfigManager::ResetToDefaults() {
    m_config = AppConfig{};
}

bool ConfigManager::Save() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    try {
        nlohmann::json j;
        j["launchOnStartup"] = m_config.launchOnStartup;
        j["minimizeToTray"] = m_config.minimizeToTray;
        j["monitorIndex"] = m_config.monitorIndex;
        j["uiScale"] = m_config.uiScale;

        // Dynamic Island
        j["island"]["enabled"] = m_config.island.enabled;
        j["island"]["idleWidth"] = m_config.island.idleWidth;
        j["island"]["idleHeight"] = m_config.island.idleHeight;
        j["island"]["expandedMediaWidth"] = m_config.island.expandedMediaWidth;
        j["island"]["expandedMediaHeight"] = m_config.island.expandedMediaHeight;
        j["island"]["expandedSystemWidth"] = m_config.island.expandedSystemWidth;
        j["island"]["expandedSystemHeight"] = m_config.island.expandedSystemHeight;
        j["island"]["expandedVolumeWidth"] = m_config.island.expandedVolumeWidth;
        j["island"]["expandedVolumeHeight"] = m_config.island.expandedVolumeHeight;
        j["island"]["topOffset"] = m_config.island.topOffset;
        j["island"]["expandOnHover"] = m_config.island.expandOnHover;
        j["island"]["hoverDelayMs"] = m_config.island.hoverDelayMs;
        j["island"]["pinOnClick"] = m_config.island.pinOnClick;
        j["island"]["showClockIdle"] = m_config.island.showClockIdle;
        j["island"]["showAudioVisualizer"] = m_config.island.showAudioVisualizer;
        j["island"]["mediaAutoExpand"] = m_config.island.mediaAutoExpand;
        j["island"]["notificationDismissSeconds"] = m_config.island.notificationDismissSeconds;

        // Widgets
        j["widgets"]["enabled"] = m_config.widgets.enabled;
        j["widgets"]["snapToGrid"] = m_config.widgets.snapToGrid;
        j["widgets"]["gridSize"] = m_config.widgets.gridSize;

        auto serializeWidget = [](const DesktopWidgetConfig& w) {
            return nlohmann::json{
                {"enabled", w.enabled},
                {"posX", w.position.x},
                {"posY", w.position.y},
                {"sizeW", w.size.x},
                {"sizeH", w.size.y},
                {"opacity", w.opacity},
                {"locked", w.locked}
            };
        };

        j["widgets"]["sysMon"] = serializeWidget(m_config.widgets.sysMonWidget);
        j["widgets"]["clock"] = serializeWidget(m_config.widgets.clockWidget);
        j["widgets"]["network"] = serializeWidget(m_config.widgets.netWidget);
        j["widgets"]["media"] = serializeWidget(m_config.widgets.mediaWidget);

        // Taskbar
        j["taskbar"]["enabled"] = m_config.taskbar.enabled;
        j["taskbar"]["style"] = static_cast<int>(m_config.taskbar.style);
        j["taskbar"]["tintColor"] = m_config.taskbar.tintColor;
        j["taskbar"]["roundElements"] = m_config.taskbar.roundElements;
        j["taskbar"]["autoHide"] = m_config.taskbar.autoHide;

        // Appearance
        j["appearance"]["preset"] = static_cast<int>(m_config.appearance.preset);
        j["appearance"]["accentColor"] = m_config.appearance.accentColor;
        j["appearance"]["secondaryAccent"] = m_config.appearance.secondaryAccent;
        j["appearance"]["cornerRadius"] = m_config.appearance.cornerRadius;
        j["appearance"]["backgroundOpacity"] = m_config.appearance.backgroundOpacity;
        j["appearance"]["glowIntensity"] = m_config.appearance.glowIntensity;
        j["appearance"]["shadowIntensity"] = m_config.appearance.shadowIntensity;
        j["appearance"]["enableGlow"] = m_config.appearance.enableGlow;
        j["appearance"]["enableShadow"] = m_config.appearance.enableShadow;
        j["appearance"]["enableBlur"] = m_config.appearance.enableBlur;

        // Animation
        j["animation"]["springStiffness"] = m_config.animation.springStiffness;
        j["animation"]["springDamping"] = m_config.animation.springDamping;
        j["animation"]["springMass"] = m_config.animation.springMass;
        j["animation"]["timeScale"] = m_config.animation.timeScale;
        j["animation"]["reducedMotion"] = m_config.animation.reducedMotion;

        // Gaming
        j["gaming"]["enabled"] = m_config.gaming.enabled;
        j["gaming"]["autoDetectFullscreen"] = m_config.gaming.autoDetectFullscreen;
        j["gaming"]["action"] = static_cast<int>(m_config.gaming.action);
        j["gaming"]["pauseWidgets"] = m_config.gaming.pauseWidgets;
        j["gaming"]["suppressNotifications"] = m_config.gaming.suppressNotifications;

        std::ofstream file(m_configPath);
        if (!file.is_open()) return false;
        file << j.dump(4);
        AETHER_LOG_INFO("Config saved successfully to " << m_configPath.string());
        return true;
    } catch (const std::exception& ex) {
        AETHER_LOG_ERROR("Failed to save config: " << ex.what());
        return false;
    }
}

bool ConfigManager::Load() {
    try {
        std::ifstream file(m_configPath);
        if (!file.is_open()) return false;

        nlohmann::json j;
        file >> j;

        if (j.contains("launchOnStartup")) m_config.launchOnStartup = j["launchOnStartup"];
        if (j.contains("minimizeToTray")) m_config.minimizeToTray = j["minimizeToTray"];
        if (j.contains("monitorIndex")) m_config.monitorIndex = j["monitorIndex"];
        if (j.contains("uiScale")) m_config.uiScale = j["uiScale"];

        if (j.contains("island")) {
            const auto& ji = j["island"];
            if (ji.contains("enabled")) m_config.island.enabled = ji["enabled"];
            if (ji.contains("idleWidth")) m_config.island.idleWidth = ji["idleWidth"];
            if (ji.contains("idleHeight")) m_config.island.idleHeight = ji["idleHeight"];
            if (ji.contains("expandedMediaWidth")) m_config.island.expandedMediaWidth = ji["expandedMediaWidth"];
            if (ji.contains("expandedMediaHeight")) m_config.island.expandedMediaHeight = ji["expandedMediaHeight"];
            if (ji.contains("expandedSystemWidth")) m_config.island.expandedSystemWidth = ji["expandedSystemWidth"];
            if (ji.contains("expandedSystemHeight")) m_config.island.expandedSystemHeight = ji["expandedSystemHeight"];
            if (ji.contains("expandedVolumeWidth")) m_config.island.expandedVolumeWidth = ji["expandedVolumeWidth"];
            if (ji.contains("expandedVolumeHeight")) m_config.island.expandedVolumeHeight = ji["expandedVolumeHeight"];
            if (ji.contains("topOffset")) m_config.island.topOffset = ji["topOffset"];
            if (ji.contains("expandOnHover")) m_config.island.expandOnHover = ji["expandOnHover"];
            if (ji.contains("hoverDelayMs")) m_config.island.hoverDelayMs = ji["hoverDelayMs"];
            if (ji.contains("pinOnClick")) m_config.island.pinOnClick = ji["pinOnClick"];
            if (ji.contains("showClockIdle")) m_config.island.showClockIdle = ji["showClockIdle"];
            if (ji.contains("showAudioVisualizer")) m_config.island.showAudioVisualizer = ji["showAudioVisualizer"];
            if (ji.contains("mediaAutoExpand")) m_config.island.mediaAutoExpand = ji["mediaAutoExpand"];
            if (ji.contains("notificationDismissSeconds")) m_config.island.notificationDismissSeconds = ji["notificationDismissSeconds"];
        }

        auto deserializeWidget = [](const nlohmann::json& jw, DesktopWidgetConfig& w) {
            if (jw.contains("enabled")) w.enabled = jw["enabled"];
            if (jw.contains("posX")) w.position.x = jw["posX"];
            if (jw.contains("posY")) w.position.y = jw["posY"];
            if (jw.contains("sizeW")) w.size.x = jw["sizeW"];
            if (jw.contains("sizeH")) w.size.y = jw["sizeH"];
            if (jw.contains("opacity")) w.opacity = jw["opacity"];
            if (jw.contains("locked")) w.locked = jw["locked"];
        };

        if (j.contains("widgets")) {
            const auto& jw = j["widgets"];
            if (jw.contains("enabled")) m_config.widgets.enabled = jw["enabled"];
            if (jw.contains("snapToGrid")) m_config.widgets.snapToGrid = jw["snapToGrid"];
            if (jw.contains("gridSize")) m_config.widgets.gridSize = jw["gridSize"];
            if (jw.contains("sysMon")) deserializeWidget(jw["sysMon"], m_config.widgets.sysMonWidget);
            if (jw.contains("clock")) deserializeWidget(jw["clock"], m_config.widgets.clockWidget);
            if (jw.contains("network")) deserializeWidget(jw["network"], m_config.widgets.netWidget);
            if (jw.contains("media")) deserializeWidget(jw["media"], m_config.widgets.mediaWidget);
        }

        if (j.contains("taskbar")) {
            const auto& jt = j["taskbar"];
            if (jt.contains("enabled")) m_config.taskbar.enabled = jt["enabled"];
            if (jt.contains("style")) m_config.taskbar.style = static_cast<TaskbarStyle>(jt["style"].get<int>());
            if (jt.contains("tintColor")) m_config.taskbar.tintColor = jt["tintColor"];
            if (jt.contains("roundElements")) m_config.taskbar.roundElements = jt["roundElements"];
            if (jt.contains("autoHide")) m_config.taskbar.autoHide = jt["autoHide"];
        }

        if (j.contains("appearance")) {
            const auto& ja = j["appearance"];
            if (ja.contains("preset")) m_config.appearance.preset = static_cast<ThemePreset>(ja["preset"].get<int>());
            if (ja.contains("accentColor")) m_config.appearance.accentColor = ja["accentColor"];
            if (ja.contains("secondaryAccent")) m_config.appearance.secondaryAccent = ja["secondaryAccent"];
            if (ja.contains("cornerRadius")) m_config.appearance.cornerRadius = ja["cornerRadius"];
            if (ja.contains("backgroundOpacity")) m_config.appearance.backgroundOpacity = ja["backgroundOpacity"];
            if (ja.contains("glowIntensity")) m_config.appearance.glowIntensity = ja["glowIntensity"];
            if (ja.contains("shadowIntensity")) m_config.appearance.shadowIntensity = ja["shadowIntensity"];
            if (ja.contains("enableGlow")) m_config.appearance.enableGlow = ja["enableGlow"];
            if (ja.contains("enableShadow")) m_config.appearance.enableShadow = ja["enableShadow"];
            if (ja.contains("enableBlur")) m_config.appearance.enableBlur = ja["enableBlur"];
        }

        if (j.contains("animation")) {
            const auto& janim = j["animation"];
            if (janim.contains("springStiffness")) m_config.animation.springStiffness = janim["springStiffness"];
            if (janim.contains("springDamping")) m_config.animation.springDamping = janim["springDamping"];
            if (janim.contains("springMass")) m_config.animation.springMass = janim["springMass"];
            if (janim.contains("timeScale")) m_config.animation.timeScale = janim["timeScale"];
            if (janim.contains("reducedMotion")) m_config.animation.reducedMotion = janim["reducedMotion"];
        }

        if (j.contains("gaming")) {
            const auto& jg = j["gaming"];
            if (jg.contains("enabled")) m_config.gaming.enabled = jg["enabled"];
            if (jg.contains("autoDetectFullscreen")) m_config.gaming.autoDetectFullscreen = jg["autoDetectFullscreen"];
            if (jg.contains("action")) m_config.gaming.action = static_cast<GamingModeAction>(jg["action"].get<int>());
            if (jg.contains("pauseWidgets")) m_config.gaming.pauseWidgets = jg["pauseWidgets"];
            if (jg.contains("suppressNotifications")) m_config.gaming.suppressNotifications = jg["suppressNotifications"];
        }

        AETHER_LOG_INFO("Config loaded successfully from " << m_configPath.string());
        return true;
    } catch (const std::exception& ex) {
        AETHER_LOG_WARN("Could not parse config, using defaults: " << ex.what());
        return false;
    }
}

void ConfigManager::SetConfig(const AppConfig& newConfig) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_config = newConfig;
    }
    Save();
    core::EventBus::Instance().Publish(ConfigUpdatedEvent{m_config});
}

} // namespace aether::config
