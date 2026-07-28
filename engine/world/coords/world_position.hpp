#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/world/coords/world_coords.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

namespace heartstead::world {

// Authoritative spatial positions never collapse the integer world address into a floating-point
// value. The local offset is normalized to [0, 1) on every axis, making anchor the exact owning
// block even beyond the range where a double can distinguish adjacent cells.
struct WorldPosition {
    BlockCoord anchor{};
    math::Vec3d local_offset{};

    constexpr WorldPosition() = default;

    WorldPosition(double x, double y, double z) noexcept {
        assign_legacy_global({x, y, z});
    }

    WorldPosition(math::Vec3d global) noexcept {
        assign_legacy_global(global);
    }

    [[nodiscard]] static core::Result<WorldPosition> from_anchor(BlockCoord anchor,
                                                                 math::Vec3d local_offset) {
        if (!local_offset.is_finite()) {
            return core::Result<WorldPosition>::failure(
                "world_position.non_finite_offset", "world position local offset must be finite");
        }

        WorldPosition result;
        result.anchor = anchor;
        result.local_offset = local_offset;
        auto status = result.normalize();
        if (!status) {
            return core::Result<WorldPosition>::failure(status.error().code,
                                                        status.error().message);
        }
        return core::Result<WorldPosition>::success(result);
    }

    [[nodiscard]] static core::Result<WorldPosition> from_legacy_global(math::Vec3d global) {
        if (!global.is_finite()) {
            return core::Result<WorldPosition>::failure("world_position.non_finite_global",
                                                        "legacy global position must be finite");
        }
        constexpr double min_inclusive = -9223372036854775808.0;
        constexpr double max_exclusive = 9223372036854775808.0;
        if (global.x < min_inclusive || global.y < min_inclusive || global.z < min_inclusive ||
            global.x >= max_exclusive || global.y >= max_exclusive || global.z >= max_exclusive) {
            return core::Result<WorldPosition>::failure(
                "world_position.global_out_of_range",
                "legacy global position cannot be represented by a signed 64-bit anchor");
        }

        const math::Vec3d floors{std::floor(global.x), std::floor(global.y), std::floor(global.z)};
        WorldPosition result;
        result.anchor = {static_cast<std::int64_t>(floors.x), static_cast<std::int64_t>(floors.y),
                         static_cast<std::int64_t>(floors.z)};
        result.local_offset = global - floors;
        return core::Result<WorldPosition>::success(result);
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return local_offset.is_finite() && local_offset.x >= 0.0 && local_offset.x < 1.0 &&
               local_offset.y >= 0.0 && local_offset.y < 1.0 && local_offset.z >= 0.0 &&
               local_offset.z < 1.0;
    }

    [[nodiscard]] math::Vec3d approximate_global() const noexcept {
        return {static_cast<double>(anchor.x) + local_offset.x,
                static_cast<double>(anchor.y) + local_offset.y,
                static_cast<double>(anchor.z) + local_offset.z};
    }

    [[nodiscard]] math::Vec3d relative_to(BlockCoord origin) const noexcept {
        const auto axis = [](std::int64_t value, std::int64_t base, double local) noexcept {
            return static_cast<double>(static_cast<long double>(value) -
                                       static_cast<long double>(base) +
                                       static_cast<long double>(local));
        };
        return {axis(anchor.x, origin.x, local_offset.x), axis(anchor.y, origin.y, local_offset.y),
                axis(anchor.z, origin.z, local_offset.z)};
    }

    friend bool operator==(const WorldPosition&, const WorldPosition&) = default;

  private:
    [[nodiscard]] core::Status normalize() {
        const math::Vec3d floors{std::floor(local_offset.x), std::floor(local_offset.y),
                                 std::floor(local_offset.z)};
        constexpr auto min = std::numeric_limits<std::int64_t>::min();
        constexpr auto max = std::numeric_limits<std::int64_t>::max();
        const auto add_axis = [](std::int64_t base, double delta) -> core::Result<std::int64_t> {
            constexpr double min_inclusive = -9223372036854775808.0;
            constexpr double max_exclusive = 9223372036854775808.0;
            if (delta < min_inclusive || delta >= max_exclusive) {
                return core::Result<std::int64_t>::failure(
                    "world_position.offset_out_of_range",
                    "world position local offset cannot be normalized into int64");
            }
            const auto integer_delta = static_cast<std::int64_t>(delta);
            if ((integer_delta > 0 && base > max - integer_delta) ||
                (integer_delta < 0 && base < min - integer_delta)) {
                return core::Result<std::int64_t>::failure(
                    "world_position.anchor_overflow",
                    "world position normalization overflows its signed 64-bit anchor");
            }
            return core::Result<std::int64_t>::success(base + integer_delta);
        };

        auto x = add_axis(anchor.x, floors.x);
        auto y = add_axis(anchor.y, floors.y);
        auto z = add_axis(anchor.z, floors.z);
        if (!x || !y || !z) {
            const auto& error = !x ? x.error() : (!y ? y.error() : z.error());
            return core::Status::failure(error.code, error.message);
        }
        anchor = {x.value(), y.value(), z.value()};
        local_offset -= floors;
        return core::Status::ok();
    }

    void assign_legacy_global(math::Vec3d global) noexcept {
        auto converted = from_legacy_global(global);
        if (converted) {
            *this = converted.value();
            return;
        }
        anchor = {};
        local_offset = global;
    }
};

struct WorldTransform {
    WorldPosition position{};
    math::Vec3d rotation_degrees{};
    math::Vec3d scale{1.0, 1.0, 1.0};

    [[nodiscard]] bool is_finite() const noexcept {
        return position.is_valid() && rotation_degrees.is_finite() && scale.is_finite();
    }

    [[nodiscard]] bool has_non_zero_scale() const noexcept {
        return !scale.any_zero();
    }

    friend bool operator==(const WorldTransform&, const WorldTransform&) = default;
};

struct FloatingOrigin {
    BlockCoord block{};
};

struct PhysicsIslandFrame {
    BlockCoord block{};
    float max_local_extent = 4096.0F;
};

[[nodiscard]] inline core::Result<math::Vec3f>
to_camera_relative(const WorldPosition& position, FloatingOrigin origin,
                   float max_local_extent = 1'048'576.0F) {
    if (!position.is_valid() || !std::isfinite(max_local_extent) || max_local_extent <= 0.0F) {
        return core::Result<math::Vec3f>::failure("floating_origin.invalid_input",
                                                  "camera-relative conversion input is invalid");
    }
    const auto relative = position.relative_to(origin.block);
    if (std::abs(relative.x) > max_local_extent || std::abs(relative.y) > max_local_extent ||
        std::abs(relative.z) > max_local_extent) {
        return core::Result<math::Vec3f>::failure(
            "floating_origin.outside_local_extent",
            "world position is outside the camera-relative rendering extent");
    }
    return core::Result<math::Vec3f>::success({static_cast<float>(relative.x),
                                               static_cast<float>(relative.y),
                                               static_cast<float>(relative.z)});
}

[[nodiscard]] inline core::Result<math::Vec3f> to_physics_local(const WorldPosition& position,
                                                                const PhysicsIslandFrame& frame) {
    return to_camera_relative(position, FloatingOrigin{frame.block}, frame.max_local_extent);
}

[[nodiscard]] inline core::Result<WorldPosition>
from_physics_local(math::Vec3f local_position, const PhysicsIslandFrame& frame) {
    if (!local_position.is_finite() || !std::isfinite(frame.max_local_extent) ||
        frame.max_local_extent <= 0.0F || std::abs(local_position.x) > frame.max_local_extent ||
        std::abs(local_position.y) > frame.max_local_extent ||
        std::abs(local_position.z) > frame.max_local_extent) {
        return core::Result<WorldPosition>::failure(
            "physics_island.invalid_local_position",
            "physics-local position is non-finite or outside the island extent");
    }
    return WorldPosition::from_anchor(frame.block, {static_cast<double>(local_position.x),
                                                    static_cast<double>(local_position.y),
                                                    static_cast<double>(local_position.z)});
}

} // namespace heartstead::world
