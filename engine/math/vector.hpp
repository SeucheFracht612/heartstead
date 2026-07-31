#pragma once

#include <cmath>
#include <compare>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace heartstead::math {

enum class Axis3 {
    x,
    y,
    z,
};

template <typename T> struct Vec2 {
    T x{};
    T y{};

    [[nodiscard]] constexpr bool is_finite() const noexcept {
        if constexpr (std::is_floating_point_v<T>) {
            return std::isfinite(x) && std::isfinite(y);
        } else {
            return true;
        }
    }

    friend constexpr auto operator<=>(const Vec2&, const Vec2&) = default;
};

template <typename T> struct Vec3 {
    T x{};
    T y{};
    T z{};

    [[nodiscard]] constexpr bool is_finite() const noexcept {
        if constexpr (std::is_floating_point_v<T>) {
            return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
        } else {
            return true;
        }
    }

    [[nodiscard]] constexpr bool all_positive() const noexcept {
        return x > T{} && y > T{} && z > T{};
    }

    [[nodiscard]] constexpr bool any_zero() const noexcept {
        return x == T{} || y == T{} || z == T{};
    }

    friend constexpr auto operator<=>(const Vec3&, const Vec3&) = default;
};

template <typename T>
[[nodiscard]] constexpr Vec3<T> component_min(Vec3<T> lhs, Vec3<T> rhs) noexcept;
template <typename T>
[[nodiscard]] constexpr Vec3<T> component_max(Vec3<T> lhs, Vec3<T> rhs) noexcept;

template <typename T> struct Transform3 {
    Vec3<T> position{};
    Vec3<T> rotation_degrees{};
    Vec3<T> scale{static_cast<T>(1), static_cast<T>(1), static_cast<T>(1)};

    [[nodiscard]] constexpr bool is_finite() const noexcept {
        return position.is_finite() && rotation_degrees.is_finite() && scale.is_finite();
    }

    [[nodiscard]] constexpr bool has_non_zero_scale() const noexcept {
        return !scale.any_zero();
    }
};

template <typename T> struct Bounds3 {
    Vec3<T> min{};
    Vec3<T> max{};

    friend constexpr bool operator==(const Bounds3&, const Bounds3&) = default;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return min.is_finite() && max.is_finite() && min.x <= max.x && min.y <= max.y &&
               min.z <= max.z;
    }

    [[nodiscard]] constexpr bool contains(Vec3<T> point) const noexcept {
        return is_valid() && point.x >= min.x && point.y >= min.y && point.z >= min.z &&
               point.x <= max.x && point.y <= max.y && point.z <= max.z;
    }

    [[nodiscard]] constexpr Bounds3 expanded(T amount) const noexcept {
        return Bounds3{
            Vec3<T>{static_cast<T>(min.x - amount), static_cast<T>(min.y - amount),
                    static_cast<T>(min.z - amount)},
            Vec3<T>{static_cast<T>(max.x + amount), static_cast<T>(max.y + amount),
                    static_cast<T>(max.z + amount)},
        };
    }

    [[nodiscard]] constexpr Bounds3 merged_with(const Bounds3& other) const noexcept {
        return Bounds3{
            component_min(min, other.min),
            component_max(max, other.max),
        };
    }
};

template <typename T> [[nodiscard]] constexpr Vec3<T> operator+(Vec3<T> lhs, Vec3<T> rhs) noexcept {
    return Vec3<T>{static_cast<T>(lhs.x + rhs.x), static_cast<T>(lhs.y + rhs.y),
                   static_cast<T>(lhs.z + rhs.z)};
}

template <typename T> [[nodiscard]] constexpr Vec3<T> operator-(Vec3<T> lhs, Vec3<T> rhs) noexcept {
    return Vec3<T>{static_cast<T>(lhs.x - rhs.x), static_cast<T>(lhs.y - rhs.y),
                   static_cast<T>(lhs.z - rhs.z)};
}

template <typename T> [[nodiscard]] constexpr Vec3<T> operator*(Vec3<T> value, T scalar) noexcept {
    return Vec3<T>{static_cast<T>(value.x * scalar), static_cast<T>(value.y * scalar),
                   static_cast<T>(value.z * scalar)};
}

template <typename T> [[nodiscard]] constexpr Vec3<T> operator*(T scalar, Vec3<T> value) noexcept {
    return value * scalar;
}

template <typename T> [[nodiscard]] constexpr Vec3<T> operator/(Vec3<T> value, T scalar) noexcept {
    return Vec3<T>{static_cast<T>(value.x / scalar), static_cast<T>(value.y / scalar),
                   static_cast<T>(value.z / scalar)};
}

template <typename T> constexpr Vec3<T>& operator+=(Vec3<T>& lhs, Vec3<T> rhs) noexcept {
    lhs = lhs + rhs;
    return lhs;
}

template <typename T> constexpr Vec3<T>& operator-=(Vec3<T>& lhs, Vec3<T> rhs) noexcept {
    lhs = lhs - rhs;
    return lhs;
}

template <typename T> constexpr Vec3<T>& operator*=(Vec3<T>& value, T scalar) noexcept {
    value = value * scalar;
    return value;
}

template <typename T> constexpr Vec3<T>& operator/=(Vec3<T>& value, T scalar) noexcept {
    value = value / scalar;
    return value;
}

template <typename T> [[nodiscard]] constexpr T dot(Vec3<T> lhs, Vec3<T> rhs) noexcept {
    return static_cast<T>((lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z));
}

template <typename T> [[nodiscard]] constexpr Vec3<T> cross(Vec3<T> lhs, Vec3<T> rhs) noexcept {
    return Vec3<T>{static_cast<T>((lhs.y * rhs.z) - (lhs.z * rhs.y)),
                   static_cast<T>((lhs.z * rhs.x) - (lhs.x * rhs.z)),
                   static_cast<T>((lhs.x * rhs.y) - (lhs.y * rhs.x))};
}

template <typename T> [[nodiscard]] constexpr T length_squared(Vec3<T> value) noexcept {
    return dot(value, value);
}

template <typename T> [[nodiscard]] double length(Vec3<T> value) noexcept {
    return std::sqrt(static_cast<double>(length_squared(value)));
}

template <typename T>
[[nodiscard]] constexpr Vec3<T> component_min(Vec3<T> lhs, Vec3<T> rhs) noexcept {
    return Vec3<T>{lhs.x < rhs.x ? lhs.x : rhs.x, lhs.y < rhs.y ? lhs.y : rhs.y,
                   lhs.z < rhs.z ? lhs.z : rhs.z};
}

template <typename T>
[[nodiscard]] constexpr Vec3<T> component_max(Vec3<T> lhs, Vec3<T> rhs) noexcept {
    return Vec3<T>{lhs.x > rhs.x ? lhs.x : rhs.x, lhs.y > rhs.y ? lhs.y : rhs.y,
                   lhs.z > rhs.z ? lhs.z : rhs.z};
}

template <typename T> [[nodiscard]] constexpr Vec3<T> splat(T value) noexcept {
    return Vec3<T>{value, value, value};
}

using Vec3f = Vec3<float>;
using Vec3d = Vec3<double>;
using Vec2f = Vec2<float>;
using Vec2d = Vec2<double>;
using Coord3i = Vec3<std::int32_t>;
using Coord3i64 = Vec3<std::int64_t>;
using Coord3u16 = Vec3<std::uint16_t>;

using Transform3f = Transform3<float>;
using Transform3d = Transform3<double>;

using Bounds3i = Bounds3<std::int32_t>;
using Bounds3i64 = Bounds3<std::int64_t>;
using Bounds3f = Bounds3<float>;
using Bounds3d = Bounds3<double>;

[[nodiscard]] std::string_view axis3_name(Axis3 axis) noexcept;

} // namespace heartstead::math
