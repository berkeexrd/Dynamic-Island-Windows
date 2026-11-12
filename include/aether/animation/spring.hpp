#pragma once

#include "aether/core/types.hpp"
#include <cmath>

namespace aether::animation {

struct SpringConfig {
    float stiffness{280.0f};  // k
    float damping{24.0f};     // c
    float mass{1.0f};         // m
    float restThreshold{0.001f};
};

template<typename T>
class Spring {
public:
    Spring() : m_current{}, m_target{}, m_velocity{} {}
    explicit Spring(const T& initialVal, const SpringConfig& config = SpringConfig{})
        : m_current(initialVal), m_target(initialVal), m_velocity{}, m_config(config) {}

    void SetConfig(const SpringConfig& config) { m_config = config; }
    [[nodiscard]] const SpringConfig& GetConfig() const { return m_config; }

    void SetTarget(const T& target) {
        m_target = target;
        m_isResting = false;
    }

    void SnapTo(const T& value) {
        m_current = value;
        m_target = value;
        m_velocity = T{};
        m_isResting = true;
    }

    [[nodiscard]] const T& GetValue() const { return m_current; }
    [[nodiscard]] const T& GetTarget() const { return m_target; }
    [[nodiscard]] const T& GetVelocity() const { return m_velocity; }
    [[nodiscard]] bool IsResting() const { return m_isResting; }

    void Update(float dt);

private:
    T m_current{};
    T m_target{};
    T m_velocity{};
    SpringConfig m_config{};
    bool m_isResting{true};
};

// Float specialization
template<>
inline void Spring<float>::Update(float dt) {
    if (m_isResting) return;

    // Substep integration for high stability with large dt
    const float maxSubstep = 0.008f; // ~120Hz minimum substep
    float remainingDt = std::min(dt, 0.1f); // clamp lag spikes

    while (remainingDt > 0.0f) {
        float step = std::min(remainingDt, maxSubstep);
        float displacement = m_current - m_target;
        float springForce = -m_config.stiffness * displacement;
        float dampingForce = -m_config.damping * m_velocity;
        float acceleration = (springForce + dampingForce) / m_config.mass;

        m_velocity += acceleration * step;
        m_current += m_velocity * step;

        remainingDt -= step;
    }

    if (std::abs(m_current - m_target) < m_config.restThreshold &&
        std::abs(m_velocity) < m_config.restThreshold * 2.0f) {
        m_current = m_target;
        m_velocity = 0.0f;
        m_isResting = true;
    }
}

// Vec2 specialization
template<>
inline void Spring<core::Vec2>::Update(float dt) {
    if (m_isResting) return;

    const float maxSubstep = 0.008f;
    float remainingDt = std::min(dt, 0.1f);

    while (remainingDt > 0.0f) {
        float step = std::min(remainingDt, maxSubstep);
        core::Vec2 displacement = m_current - m_target;
        core::Vec2 springForce = displacement * (-m_config.stiffness);
        core::Vec2 dampingForce = m_velocity * (-m_config.damping);
        core::Vec2 acceleration = (springForce + dampingForce) / m_config.mass;

        m_velocity += acceleration * step;
        m_current += m_velocity * step;

        remainingDt -= step;
    }

    float distSq = (m_current - m_target).LengthSquared();
    float velSq = m_velocity.LengthSquared();
    float threshSq = m_config.restThreshold * m_config.restThreshold;

    if (distSq < threshSq && velSq < threshSq * 4.0f) {
        m_current = m_target;
        m_velocity = {0.0f, 0.0f};
        m_isResting = true;
    }
}

// Rect specialization
template<>
inline void Spring<core::Rect>::Update(float dt) {
    if (m_isResting) return;

    const float maxSubstep = 0.008f;
    float remainingDt = std::min(dt, 0.1f);

    while (remainingDt > 0.0f) {
        float step = std::min(remainingDt, maxSubstep);

        auto stepComponent = [this, step](float& cur, float tgt, float& vel) {
            float disp = cur - tgt;
            float force = -m_config.stiffness * disp - m_config.damping * vel;
            float acc = force / m_config.mass;
            vel += acc * step;
            cur += vel * step;
        };

        stepComponent(m_current.x, m_target.x, m_velocity.x);
        stepComponent(m_current.y, m_target.y, m_velocity.y);
        stepComponent(m_current.width, m_target.width, m_velocity.width);
        stepComponent(m_current.height, m_target.height, m_velocity.height);

        remainingDt -= step;
    }

    float dx = std::abs(m_current.x - m_target.x);
    float dy = std::abs(m_current.y - m_target.y);
    float dw = std::abs(m_current.width - m_target.width);
    float dh = std::abs(m_current.height - m_target.height);

    float vx = std::abs(m_velocity.x);
    float vy = std::abs(m_velocity.y);
    float vw = std::abs(m_velocity.width);
    float vh = std::abs(m_velocity.height);

    float t = m_config.restThreshold;
    if (dx < t && dy < t && dw < t && dh < t &&
        vx < t * 2 && vy < t * 2 && vw < t * 2 && vh < t * 2) {
        m_current = m_target;
        m_velocity = core::Rect{0, 0, 0, 0};
        m_isResting = true;
    }
}

} // namespace aether::animation
