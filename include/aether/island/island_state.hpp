#pragma once

#include <string>
#include <cstdint>

namespace aether::island {

enum class IslandMode {
    Idle,
    MediaExpanded,
    SystemExpanded,
    VolumeHUD,
    Notification
};

struct NotificationData {
    std::wstring title;
    std::wstring message;
    std::wstring sourceApp;
    uint32_t iconColor{0xFF0078D4};
    double timeRemaining{4.0};
};

} // namespace aether::island
