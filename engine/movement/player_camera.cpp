#include "engine/movement/player_camera.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace heartstead::movement {

namespace {

struct CameraConstraint {
    double safe_fraction = 1.0;
    bool obstructed = false;
};

[[nodiscard]] std::optional<double> segment_aabb_entry(math::Vec3d start, math::Vec3d delta,
                                                       math::Bounds3d bounds) noexcept {
    double entry = 0.0;
    double exit = 1.0;
    const auto clip_axis = [&entry, &exit](double origin, double direction, double minimum,
                                           double maximum) {
        constexpr double parallel_epsilon = 1.0e-12;
        if (std::abs(direction) <= parallel_epsilon) {
            return origin >= minimum && origin <= maximum;
        }
        auto first = (minimum - origin) / direction;
        auto second = (maximum - origin) / direction;
        if (first > second) {
            std::swap(first, second);
        }
        entry = std::max(entry, first);
        exit = std::min(exit, second);
        return entry <= exit;
    };
    if (!clip_axis(start.x, delta.x, bounds.min.x, bounds.max.x) ||
        !clip_axis(start.y, delta.y, bounds.min.y, bounds.max.y) ||
        !clip_axis(start.z, delta.z, bounds.min.z, bounds.max.z) || exit < 0.0 || entry > 1.0) {
        return std::nullopt;
    }
    return std::clamp(entry, 0.0, 1.0);
}

[[nodiscard]] core::Result<CameraConstraint> constrain_third_person_camera(
    const world::WorldPosition& pivot, const world::WorldPosition& desired,
    const PlayerCameraCollisionContext& collision, double radius, double clearance) {
    const auto start = pivot.local_offset;
    const auto desired_local = desired.relative_to(pivot.anchor);
    const auto delta = desired_local - start;
    const auto distance = math::length(delta);
    if (distance <= 1.0e-12) {
        return core::Result<CameraConstraint>::success({1.0, false});
    }

    const auto minimum = math::component_min(start, desired_local) - math::splat(radius);
    const auto maximum = math::component_max(start, desired_local) + math::splat(radius);
    constexpr auto minimum_offset = static_cast<double>(std::numeric_limits<std::int32_t>::min());
    constexpr auto maximum_offset =
        static_cast<double>(std::numeric_limits<std::int32_t>::max() - 1);
    constexpr double maximum_query_span = 64.0;
    if (minimum.x < minimum_offset || minimum.y < minimum_offset || minimum.z < minimum_offset ||
        maximum.x > maximum_offset || maximum.y > maximum_offset || maximum.z > maximum_offset ||
        maximum.x - minimum.x > maximum_query_span || maximum.y - minimum.y > maximum_query_span ||
        maximum.z - minimum.z > maximum_query_span) {
        return core::Result<CameraConstraint>::failure(
            "player_camera.collision_query_too_large",
            "third-person camera collision query exceeds its safe local range");
    }
    const auto floor_offset = [](double value) {
        return static_cast<std::int32_t>(std::floor(value));
    };
    const math::Coord3i min_offset{floor_offset(minimum.x), floor_offset(minimum.y),
                                   floor_offset(minimum.z)};
    const math::Coord3i max_offset{floor_offset(maximum.x), floor_offset(maximum.y),
                                   floor_offset(maximum.z)};

    double nearest_entry = 1.0;
    bool obstructed = false;
    for (auto x = min_offset.x; x <= max_offset.x; ++x) {
        for (auto y = min_offset.y; y <= max_offset.y; ++y) {
            for (auto z = min_offset.z; z <= max_offset.z; ++z) {
                auto block = world::checked_block_coord_offset(pivot.anchor, {x, y, z});
                if (!block) {
                    return core::Result<CameraConstraint>::failure(block.error().code,
                                                                   block.error().message);
                }
                const auto address = world::block_to_chunk_local(block.value());
                const auto* chunk = collision.chunks.find(address.chunk);
                const world::VoxelDefinition* definition = nullptr;
                bool use_full_cube = chunk == nullptr;
                if (chunk != nullptr) {
                    auto cell = chunk->get(address.local);
                    if (!cell) {
                        return core::Result<CameraConstraint>::failure(cell.error().code,
                                                                       cell.error().message);
                    }
                    if (cell.value().is_air()) {
                        continue;
                    }
                    definition = collision.palette.find_by_type(cell.value().type);
                    use_full_cube = definition == nullptr;
                }

                const math::Vec3d translation{static_cast<double>(x), static_cast<double>(y),
                                              static_cast<double>(z)};
                const auto test_bounds = [&](math::Bounds3d bounds) {
                    bounds.min += translation - math::splat(radius);
                    bounds.max += translation + math::splat(radius);
                    if (const auto entry = segment_aabb_entry(start, delta, bounds);
                        entry.has_value()) {
                        nearest_entry = std::min(nearest_entry, *entry);
                        obstructed = true;
                    }
                };
                if (use_full_cube) {
                    test_bounds({{}, math::splat(1.0)});
                    continue;
                }
                for (const auto& source : definition->collision_bounds) {
                    test_bounds({
                        {static_cast<double>(source.min.x), static_cast<double>(source.min.y),
                         static_cast<double>(source.min.z)},
                        {static_cast<double>(source.max.x), static_cast<double>(source.max.y),
                         static_cast<double>(source.max.z)},
                    });
                }
            }
        }
    }
    const auto safe_fraction =
        obstructed ? std::max(0.0, nearest_entry - std::min(clearance / distance, nearest_entry))
                   : 1.0;
    return core::Result<CameraConstraint>::success({safe_fraction, obstructed});
}

} // namespace

core::Status PlayerCameraConfig::validate() const {
    if (!std::isfinite(standing_eye_height) || !std::isfinite(crouch_eye_height) ||
        !std::isfinite(roll_eye_height) || !std::isfinite(third_person_distance) ||
        !std::isfinite(third_person_height) || !std::isfinite(third_person_shoulder) ||
        !std::isfinite(collision_radius) || !std::isfinite(collision_clearance) ||
        !std::isfinite(third_person_restore_speed) || standing_eye_height <= 0.0 ||
        crouch_eye_height <= 0.0 || roll_eye_height <= 0.0 || third_person_distance <= 0.0 ||
        crouch_eye_height > standing_eye_height || roll_eye_height > crouch_eye_height ||
        collision_radius <= 0.0 || collision_clearance < 0.0 || third_person_restore_speed <= 0.0) {
        return core::Status::failure("player_camera.invalid_config",
                                     "player camera dimensions are invalid");
    }
    if (!std::isfinite(vertical_fov_degrees) || !std::isfinite(near_plane) ||
        !std::isfinite(far_plane) || vertical_fov_degrees <= 0.0F ||
        vertical_fov_degrees >= 180.0F || near_plane <= 0.0F || far_plane <= near_plane) {
        return core::Status::failure("player_camera.invalid_projection",
                                     "player camera projection values are invalid");
    }
    return core::Status::ok();
}

PlayerCameraRig::PlayerCameraRig(PlayerCameraConfig config) : config_(config) {}

core::Result<PlayerCameraFrame>
PlayerCameraRig::evaluate(const PlayerControllerState& player, PlayerCameraPerspective perspective,
                          std::uint32_t viewport_width, std::uint32_t viewport_height,
                          const PlayerCameraCollisionContext* collision, double delta_seconds) {
    auto config_status = config_.validate();
    if (!config_status || !player.position.is_valid() || viewport_width == 0 ||
        viewport_height == 0) {
        return core::Result<PlayerCameraFrame>::failure(
            !config_status ? config_status.error().code : "player_camera.invalid_viewport",
            !config_status ? config_status.error().message
                           : "player camera viewport and player position must be valid");
    }
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0) {
        return core::Result<PlayerCameraFrame>::failure(
            "player_camera.invalid_delta", "player camera delta must be finite and non-negative");
    }
    constexpr double degrees_to_radians = 3.14159265358979323846 / 180.0;
    const auto yaw = static_cast<double>(player.yaw_centidegrees) * 0.01 * degrees_to_radians;
    const auto pitch = static_cast<double>(player.pitch_centidegrees) * 0.01 * degrees_to_radians;
    const math::Vec3d forward{std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                              std::cos(yaw) * std::cos(pitch)};
    const math::Vec3d right{std::cos(yaw), 0.0, -std::sin(yaw)};
    const auto eye_height = player.mode == PlayerControllerMode::rolling ? config_.roll_eye_height
                            : player.crouched                            ? config_.crouch_eye_height
                                              : config_.standing_eye_height;
    const auto pivot_offset = math::Vec3d{0.0, eye_height, 0.0};
    auto camera_offset = pivot_offset;
    if (perspective == PlayerCameraPerspective::third_person) {
        camera_offset += forward * -config_.third_person_distance;
        camera_offset += right * config_.third_person_shoulder;
        camera_offset.y += config_.third_person_height;
    }
    auto camera_position = world::WorldPosition::from_anchor(
        player.position.anchor, player.position.local_offset + camera_offset);
    if (!camera_position) {
        return core::Result<PlayerCameraFrame>::failure(camera_position.error().code,
                                                        camera_position.error().message);
    }
    auto pivot = world::WorldPosition::from_anchor(player.position.anchor,
                                                   player.position.local_offset + pivot_offset);
    if (!pivot) {
        return core::Result<PlayerCameraFrame>::failure(pivot.error().code, pivot.error().message);
    }
    double allowed_boom_fraction = 1.0;
    bool boom_obstructed = false;
    if (perspective == PlayerCameraPerspective::third_person && collision != nullptr) {
        auto constrained =
            constrain_third_person_camera(pivot.value(), camera_position.value(), *collision,
                                          config_.collision_radius, config_.collision_clearance);
        if (!constrained) {
            return core::Result<PlayerCameraFrame>::failure(constrained.error().code,
                                                            constrained.error().message);
        }
        allowed_boom_fraction = constrained.value().safe_fraction;
        boom_obstructed = constrained.value().obstructed;
    }
    const auto desired_boom_delta =
        camera_position.value().relative_to(pivot.value().anchor) - pivot.value().local_offset;
    const auto desired_boom_distance = math::length(desired_boom_delta);
    bool boom_restoring = false;
    if (perspective == PlayerCameraPerspective::third_person) {
        if (!boom_initialized_) {
            boom_fraction_ = allowed_boom_fraction;
            boom_initialized_ = true;
        } else if (allowed_boom_fraction <= boom_fraction_) {
            boom_fraction_ = allowed_boom_fraction;
        } else {
            const auto restore_fraction =
                desired_boom_distance <= 1.0e-12
                    ? 1.0
                    : config_.third_person_restore_speed * delta_seconds / desired_boom_distance;
            boom_fraction_ = std::min(allowed_boom_fraction, boom_fraction_ + restore_fraction);
            boom_restoring = boom_fraction_ < allowed_boom_fraction;
        }
        camera_position = world::WorldPosition::from_anchor(
            pivot.value().anchor, pivot.value().local_offset + desired_boom_delta * boom_fraction_);
        if (!camera_position) {
            return core::Result<PlayerCameraFrame>::failure(camera_position.error().code,
                                                            camera_position.error().message);
        }
    } else {
        boom_fraction_ = 1.0;
        boom_initialized_ = false;
    }

    PlayerCameraFrame frame;
    frame.perspective = perspective;
    frame.position = camera_position.value();
    frame.forward = forward;
    frame.floating_origin.block = camera_position.value().anchor;
    const math::Vec3f local_eye{static_cast<float>(camera_position.value().local_offset.x),
                                static_cast<float>(camera_position.value().local_offset.y),
                                static_cast<float>(camera_position.value().local_offset.z)};
    constexpr float pi = 3.14159265358979323846F;
    frame.view =
        math::view_matrix(local_eye, pi - static_cast<float>(yaw), static_cast<float>(pitch));
    frame.projection = math::perspective_projection(config_.vertical_fov_degrees * pi / 180.0F,
                                                    static_cast<float>(viewport_width) /
                                                        static_cast<float>(viewport_height),
                                                    config_.near_plane, config_.far_plane);
    if (!frame.view.is_finite() || !frame.projection.is_finite()) {
        return core::Result<PlayerCameraFrame>::failure(
            "player_camera.invalid_matrix", "player camera produced a non-finite matrix");
    }
    frame.view_projection = frame.projection * frame.view;
    frame.body.root_position = player.position;
    frame.body.yaw_degrees = static_cast<double>(player.yaw_centidegrees) * 0.01;
    frame.body.planar_velocity = {player.velocity.x, 0.0, player.velocity.z};
    frame.body.crouched = player.crouched;
    frame.body.rolling = player.mode == PlayerControllerMode::rolling;
    frame.body.local_body_visible = perspective == PlayerCameraPerspective::third_person;
    if (perspective == PlayerCameraPerspective::third_person) {
        frame.desired_boom_distance = desired_boom_distance;
        frame.actual_boom_distance = desired_boom_distance * boom_fraction_;
        frame.boom_obstructed = boom_obstructed;
        frame.boom_restoring = boom_restoring;
    }
    return core::Result<PlayerCameraFrame>::success(frame);
}

} // namespace heartstead::movement
