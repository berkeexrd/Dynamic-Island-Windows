#pragma once

#include <imgui.h>
#include <unordered_map>
#include <cstdint>

namespace aether::ui {

namespace AnimationSpeed {
    constexpr float Fast = 14.0f;   // ~0.10s
    constexpr float Normal = 9.0f;  // ~0.16s
    constexpr float Slow = 6.0f;    // ~0.24s
}

class AnimationEngine {
public:
    static AnimationEngine& Instance() {
        static AnimationEngine instance;
        return instance;
    }

    void Update(float dt);

    float AnimateFloat(ImGuiID id, float target, float speed = AnimationSpeed::Normal);
    ImVec4 AnimateColor(ImGuiID id, const ImVec4& target, float speed = AnimationSpeed::Normal);
    ImVec2 AnimateVec2(ImGuiID id, const ImVec2& target, float speed = AnimationSpeed::Normal);
    float AnimateAlpha(ImGuiID id, float target, float speed = AnimationSpeed::Normal);

    void Reset();

private:
    AnimationEngine() = default;

    float m_dt{0.016f};
    std::unordered_map<ImGuiID, float> m_floats;
    std::unordered_map<ImGuiID, ImVec4> m_colors;
    std::unordered_map<ImGuiID, ImVec2> m_vec2s;
};

} // namespace aether::ui
