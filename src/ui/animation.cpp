#include "aether/ui/animation.hpp"
#include <cmath>
#include <algorithm>

namespace aether::ui {

void AnimationEngine::Update(float dt) {
    m_dt = std::clamp(dt, 0.001f, 0.1f);
}

void AnimationEngine::Reset() {
    m_floats.clear();
    m_colors.clear();
    m_vec2s.clear();
}

float AnimationEngine::AnimateFloat(ImGuiID id, float target, float speed) {
    auto it = m_floats.find(id);
    if (it == m_floats.end()) {
        m_floats[id] = target;
        return target;
    }

    float current = it->second;
    float blend = 1.0f - std::exp(-speed * m_dt);
    current += (target - current) * blend;

    if (std::abs(current - target) < 0.001f) {
        current = target;
    }

    it->second = current;
    return current;
}

ImVec4 AnimationEngine::AnimateColor(ImGuiID id, const ImVec4& target, float speed) {
    auto it = m_colors.find(id);
    if (it == m_colors.end()) {
        m_colors[id] = target;
        return target;
    }

    ImVec4 current = it->second;
    float blend = 1.0f - std::exp(-speed * m_dt);

    current.x += (target.x - current.x) * blend;
    current.y += (target.y - current.y) * blend;
    current.z += (target.z - current.z) * blend;
    current.w += (target.w - current.w) * blend;

    it->second = current;
    return current;
}

ImVec2 AnimationEngine::AnimateVec2(ImGuiID id, const ImVec2& target, float speed) {
    auto it = m_vec2s.find(id);
    if (it == m_vec2s.end()) {
        m_vec2s[id] = target;
        return target;
    }

    ImVec2 current = it->second;
    float blend = 1.0f - std::exp(-speed * m_dt);

    current.x += (target.x - current.x) * blend;
    current.y += (target.y - current.y) * blend;

    it->second = current;
    return current;
}

float AnimationEngine::AnimateAlpha(ImGuiID id, float target, float speed) {
    return AnimateFloat(id, target, speed);
}

} // namespace aether::ui
