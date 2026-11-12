#pragma once

#include <cstdint>
#include <string>
#include <algorithm>
#include <cmath>

namespace aether::core {

struct Vec2 {
    float x{0.0f};
    float y{0.0f};

    constexpr Vec2() = default;
    constexpr Vec2(float inX, float inY) : x(inX), y(inY) {}

    constexpr Vec2 operator+(const Vec2& rhs) const { return {x + rhs.x, y + rhs.y}; }
    constexpr Vec2 operator-(const Vec2& rhs) const { return {x - rhs.x, y - rhs.y}; }
    constexpr Vec2 operator*(float scalar) const { return {x * scalar, y * scalar}; }
    constexpr Vec2 operator/(float scalar) const { return {x / scalar, y / scalar}; }

    Vec2& operator+=(const Vec2& rhs) { x += rhs.x; y += rhs.y; return *this; }
    Vec2& operator-=(const Vec2& rhs) { x -= rhs.x; y -= rhs.y; return *this; }
    Vec2& operator*=(float scalar) { x *= scalar; y *= scalar; return *this; }

    [[nodiscard]] float LengthSquared() const { return x * x + y * y; }
    [[nodiscard]] float Length() const { return std::sqrt(LengthSquared()); }

    [[nodiscard]] static Vec2 Lerp(const Vec2& a, const Vec2& b, float t) {
        return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
    }
};

struct Rect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};

    constexpr Rect() = default;
    constexpr Rect(float inX, float inY, float inW, float inH)
        : x(inX), y(inY), width(inW), height(inH) {}

    [[nodiscard]] constexpr float Left() const { return x; }
    [[nodiscard]] constexpr float Top() const { return y; }
    [[nodiscard]] constexpr float Right() const { return x + width; }
    [[nodiscard]] constexpr float Bottom() const { return y + height; }

    [[nodiscard]] constexpr Vec2 TopLeft() const { return {x, y}; }
    [[nodiscard]] constexpr Vec2 Center() const { return {x + width * 0.5f, y + height * 0.5f}; }
    [[nodiscard]] constexpr Vec2 Size() const { return {width, height}; }

    [[nodiscard]] constexpr bool Contains(const Vec2& pt) const {
        return pt.x >= x && pt.x <= (x + width) && pt.y >= y && pt.y <= (y + height);
    }

    [[nodiscard]] constexpr bool Contains(float px, float py) const {
        return Contains(Vec2{px, py});
    }

    [[nodiscard]] static Rect Lerp(const Rect& a, const Rect& b, float t) {
        return {
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.width + (b.width - a.width) * t,
            a.height + (b.height - a.height) * t
        };
    }
};

struct Color {
    float r{0.0f};
    float g{0.0f};
    float b{0.0f};
    float a{1.0f};

    constexpr Color() = default;
    constexpr Color(float inR, float inG, float inB, float inA = 1.0f)
        : r(inR), g(inG), b(inB), a(inA) {}

    constexpr Color(uint8_t inR, uint8_t inG, uint8_t inB, uint8_t inA = 255)
        : r(inR / 255.0f), g(inG / 255.0f), b(inB / 255.0f), a(inA / 255.0f) {}

    [[nodiscard]] static constexpr Color FromHex(uint32_t hex) {
        // Format: 0xRRGGBB or 0xAARRGGBB
        if ((hex & 0xFF000000) != 0) {
            return {
                static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
                static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
                static_cast<float>(hex & 0xFF) / 255.0f,
                static_cast<float>((hex >> 24) & 0xFF) / 255.0f
            };
        }
        return {
            static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
            static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
            static_cast<float>(hex & 0xFF) / 255.0f,
            1.0f
        };
    }

    [[nodiscard]] uint32_t ToHex() const {
        auto clampByte = [](float v) -> uint32_t {
            return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return (clampByte(a) << 24) | (clampByte(r) << 16) | (clampByte(g) << 8) | clampByte(b);
    }

    [[nodiscard]] Color WithAlpha(float newA) const {
        return {r, g, b, newA};
    }

    [[nodiscard]] static Color Lerp(const Color& a, const Color& b, float t) {
        return {
            a.r + (b.r - a.r) * t,
            a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t
        };
    }
};

} // namespace aether::core
