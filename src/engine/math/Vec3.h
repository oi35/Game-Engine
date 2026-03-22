// File: Vec3.h
// Purpose: Defines the minimal 3D vector math primitives shared by physics and rendering code.

#pragma once

#include <cmath>

namespace engine::math {

// Minimal 3D vector type for engine math and component data.
struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;

    constexpr Vec3() = default;
    constexpr Vec3(float xValue, float yValue, float zValue) : x(xValue), y(yValue), z(zValue) {}

    constexpr Vec3 operator+(const Vec3& rhs) const {
        return {x + rhs.x, y + rhs.y, z + rhs.z};
    }

    constexpr Vec3 operator-(const Vec3& rhs) const {
        return {x - rhs.x, y - rhs.y, z - rhs.z};
    }

    constexpr Vec3 operator*(float scalar) const {
        return {x * scalar, y * scalar, z * scalar};
    }

    constexpr Vec3& operator+=(const Vec3& rhs) {
        x += rhs.x;
        y += rhs.y;
        z += rhs.z;
        return *this;
    }

    constexpr Vec3& operator-=(const Vec3& rhs) {
        x -= rhs.x;
        y -= rhs.y;
        z -= rhs.z;
        return *this;
    }

    constexpr Vec3& operator*=(float scalar) {
        x *= scalar;
        y *= scalar;
        z *= scalar;
        return *this;
    }
};

constexpr inline Vec3 operator*(float scalar, const Vec3& value) {
    return value * scalar;
}

// Dot product.
constexpr inline float dot(const Vec3& a, const Vec3& b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

// Cross product.
constexpr inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {
        (a.y * b.z) - (a.z * b.y),
        (a.z * b.x) - (a.x * b.z),
        (a.x * b.y) - (a.y * b.x),
    };
}

// Euclidean vector length.
inline float length(const Vec3& value) {
    return std::sqrt(dot(value, value));
}

// Safe normalization with fallback up-vector for near-zero input.
inline Vec3 normalize(const Vec3& value) {
    const float len = length(value);
    if (len <= 1e-6F) {
        return {0.0F, 1.0F, 0.0F};
    }
    return value * (1.0F / len);
}

}  // namespace engine::math

