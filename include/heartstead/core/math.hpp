#pragma once

#include <array>
#include <cmath>

namespace heartstead {

struct Float3 {
    float x{};
    float y{};
    float z{};

    friend constexpr Float3 operator+(Float3 lhs, Float3 rhs) noexcept {
        return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
    }
    friend constexpr Float3 operator-(Float3 lhs, Float3 rhs) noexcept {
        return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
    }
    friend constexpr Float3 operator*(Float3 value, float scale) noexcept {
        return {value.x * scale, value.y * scale, value.z * scale};
    }
    Float3& operator+=(Float3 rhs) noexcept {
        x += rhs.x;
        y += rhs.y;
        z += rhs.z;
        return *this;
    }
};

[[nodiscard]] inline float dot(Float3 lhs, Float3 rhs) noexcept {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

[[nodiscard]] inline Float3 cross(Float3 lhs, Float3 rhs) noexcept {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

[[nodiscard]] inline Float3 normalize(Float3 value) noexcept {
    const auto length = std::sqrt(dot(value, value));
    return length > 0.00001F ? value * (1.0F / length) : Float3{};
}

struct Matrix4 {
    // Column-major, matching OpenGL's default uniform layout.
    std::array<float, 16> values{};

    [[nodiscard]] static Matrix4 perspective(float vertical_fov, float aspect, float near_plane, float far_plane) noexcept {
        Matrix4 result;
        const auto scale = 1.0F / std::tan(vertical_fov * 0.5F);
        result.values[0] = scale / aspect;
        result.values[5] = scale;
        result.values[10] = (far_plane + near_plane) / (near_plane - far_plane);
        result.values[11] = -1.0F;
        result.values[14] = (2.0F * far_plane * near_plane) / (near_plane - far_plane);
        return result;
    }

    [[nodiscard]] static Matrix4 orthographic(
        float left, float right, float bottom, float top, float near_plane, float far_plane) noexcept {
        Matrix4 result;
        result.values[0] = 2.0F / (right - left);
        result.values[5] = 2.0F / (top - bottom);
        result.values[10] = -2.0F / (far_plane - near_plane);
        result.values[12] = -(right + left) / (right - left);
        result.values[13] = -(top + bottom) / (top - bottom);
        result.values[14] = -(far_plane + near_plane) / (far_plane - near_plane);
        result.values[15] = 1.0F;
        return result;
    }

    [[nodiscard]] static Matrix4 look_at(Float3 eye, Float3 target, Float3 up) noexcept {
        const auto forward = normalize(target - eye);
        const auto side = normalize(cross(forward, up));
        const auto camera_up = cross(side, forward);
        Matrix4 result;
        result.values = {
            side.x, camera_up.x, -forward.x, 0.0F,
            side.y, camera_up.y, -forward.y, 0.0F,
            side.z, camera_up.z, -forward.z, 0.0F,
            -dot(side, eye), -dot(camera_up, eye), dot(forward, eye), 1.0F,
        };
        return result;
    }
};

[[nodiscard]] inline Matrix4 operator*(const Matrix4& lhs, const Matrix4& rhs) noexcept {
    Matrix4 result;
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            float value = 0.0F;
            for (std::size_t index = 0; index < 4; ++index) {
                value += lhs.values[index * 4 + row] * rhs.values[column * 4 + index];
            }
            result.values[column * 4 + row] = value;
        }
    }
    return result;
}

} // namespace heartstead
