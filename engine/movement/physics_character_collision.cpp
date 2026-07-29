#include "engine/movement/physics_character_collision.hpp"

#include <cmath>
#include <utility>

namespace heartstead::movement {

namespace {

constexpr double collision_epsilon = 1.0e-4;

[[nodiscard]] physics::PhysicsCharacterShapeDesc to_physics_shape(CharacterShape shape) noexcept {
    return {static_cast<float>(shape.width), static_cast<float>(shape.height)};
}

[[nodiscard]] physics::Vec3 to_physics_delta(math::Vec3d value) noexcept {
    return {static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)};
}

[[nodiscard]] math::Vec3d to_world_delta(physics::Vec3 value) noexcept {
    return {static_cast<double>(value.x), static_cast<double>(value.y),
            static_cast<double>(value.z)};
}

} // namespace

core::Status PhysicsCharacterCollisionConfig::validate() const {
    if (!std::isfinite(fixed_delta_seconds) || fixed_delta_seconds <= 0.0 ||
        fixed_delta_seconds > 1.0 || !std::isfinite(maximum_shape_penetration) ||
        maximum_shape_penetration < 0.0 || maximum_shape_penetration > 2.0 ||
        !std::isfinite(maximum_slope_angle_degrees) || maximum_slope_angle_degrees <= 0.0 ||
        maximum_slope_angle_degrees >= 89.9 || !std::isfinite(mass) || mass <= 0.0 ||
        !std::isfinite(maximum_strength) || maximum_strength <= 0.0 || !std::isfinite(padding) ||
        padding < 0.0 || !std::isfinite(physics_island.max_local_extent) ||
        physics_island.max_local_extent <= 0.0F) {
        return core::Status::failure(
            "character_collision.invalid_physics_config",
            "physics character collision tuning and island extent are invalid");
    }
    return core::Status::ok();
}

core::Result<std::unique_ptr<PhysicsCharacterCollisionWorld>>
PhysicsCharacterCollisionWorld::create(physics::IPhysicsWorld& physics_world,
                                       const world::ChunkDatabase& chunks,
                                       const world::VoxelPalette& palette,
                                       const world::WorldPosition& initial_position,
                                       CharacterShape initial_shape,
                                       PhysicsCharacterCollisionConfig config) {
    auto config_status = config.validate();
    auto shape_status = initial_shape.validate();
    auto local_position = world::to_physics_local(initial_position, config.physics_island);
    if (!config_status || !shape_status || !local_position) {
        const auto* error = !config_status  ? &config_status.error()
                            : !shape_status ? &shape_status.error()
                                            : &local_position.error();
        return core::Result<std::unique_ptr<PhysicsCharacterCollisionWorld>>::failure(
            error->code, error->message);
    }
    physics::PhysicsCharacterDesc desc;
    desc.shape = to_physics_shape(initial_shape);
    desc.position = local_position.value();
    desc.max_slope_angle_degrees = static_cast<float>(config.maximum_slope_angle_degrees);
    desc.mass = static_cast<float>(config.mass);
    desc.max_strength = static_cast<float>(config.maximum_strength);
    desc.padding = static_cast<float>(config.padding);
    auto character = physics_world.create_character(desc);
    if (!character) {
        return core::Result<std::unique_ptr<PhysicsCharacterCollisionWorld>>::failure(
            character.error().code, character.error().message);
    }
    return core::Result<std::unique_ptr<PhysicsCharacterCollisionWorld>>::success(
        std::unique_ptr<PhysicsCharacterCollisionWorld>(new PhysicsCharacterCollisionWorld(
            chunks, palette, config, std::move(character).value())));
}

PhysicsCharacterCollisionWorld::PhysicsCharacterCollisionWorld(
    const world::ChunkDatabase& chunks, const world::VoxelPalette& palette,
    PhysicsCharacterCollisionConfig config, std::unique_ptr<physics::IPhysicsCharacter> character)
    : config_(config), character_(std::move(character)), voxel_queries_(chunks, palette) {}

physics::PhysicsBackend PhysicsCharacterCollisionWorld::backend() const noexcept {
    return character_->backend();
}

core::Status PhysicsCharacterCollisionWorld::set_fixed_delta_seconds(double fixed_delta_seconds) {
    if (!std::isfinite(fixed_delta_seconds) || fixed_delta_seconds <= 0.0 ||
        fixed_delta_seconds > 1.0) {
        return core::Status::failure("character_collision.invalid_fixed_delta",
                                     "character fixed delta must be in (0, 1]");
    }
    config_.fixed_delta_seconds = fixed_delta_seconds;
    return core::Status::ok();
}

core::Result<CharacterMoveResult>
PhysicsCharacterCollisionWorld::move(const world::WorldPosition& position,
                                     const CharacterShape& shape, math::Vec3d desired_delta,
                                     double step_height, bool prevent_edge_drop) {
    auto shape_status = shape.validate();
    if (!shape_status || !position.is_valid() || !desired_delta.is_finite() ||
        !std::isfinite(step_height) || step_height < 0.0 || step_height > shape.height) {
        return core::Result<CharacterMoveResult>::failure(
            "character_collision.invalid_move", "character collision move input is invalid");
    }

    auto unloaded_guard =
        voxel_queries_.move(position, shape, desired_delta, step_height, prevent_edge_drop);
    if (!unloaded_guard) {
        return unloaded_guard;
    }
    if (unloaded_guard.value().blocked_by_unloaded_chunk) {
        return unloaded_guard;
    }

    auto status = synchronize_position(position);
    if (!status) {
        return core::Result<CharacterMoveResult>::failure(status.error().code,
                                                          status.error().message);
    }
    auto shape_switch = switch_shape(shape);
    if (!shape_switch) {
        return core::Result<CharacterMoveResult>::failure(shape_switch.error().code,
                                                          shape_switch.error().message);
    }
    if (!shape_switch.value()) {
        return core::Result<CharacterMoveResult>::failure(
            "character_collision.shape_switch_blocked",
            "physics character could not switch to the requested stance");
    }

    physics::PhysicsCharacterMoveDesc move_desc;
    move_desc.desired_delta = to_physics_delta(desired_delta);
    move_desc.delta_seconds = static_cast<float>(config_.fixed_delta_seconds);
    move_desc.step_height = static_cast<float>(step_height);
    move_desc.stick_to_floor_distance = desired_delta.y <= 0.0 ? 0.05F : 0.0F;
    auto moved = character_->move(move_desc);
    if (!moved) {
        return core::Result<CharacterMoveResult>::failure(moved.error().code,
                                                          moved.error().message);
    }

    auto next = world_position(moved.value().position);
    if (!next) {
        return core::Result<CharacterMoveResult>::failure(next.error().code, next.error().message);
    }
    auto applied = to_world_delta(moved.value().applied_delta);
    bool stepped = moved.value().stepped;
    bool grounded =
        moved.value().ground_state == physics::PhysicsCharacterGroundState::on_ground;

    const auto& voxel_move = unloaded_guard.value();
    const auto physics_blocked_horizontally =
        std::abs(applied.x - desired_delta.x) > collision_epsilon ||
        std::abs(applied.z - desired_delta.z) > collision_epsilon;
    // CharacterVirtual can stall at the convex seam between a floor and a riser when both are
    // children of the same cooked chunk compound. The voxel solver is authoritative for static
    // terrain, so use its exact step candidate only after Jolt revalidates the complete character
    // shape at that position (including dynamic-body overlap).
    if (voxel_move.stepped && !stepped && physics_blocked_horizontally) {
        auto candidate_local =
            world::to_physics_local(voxel_move.position, config_.physics_island);
        if (!candidate_local) {
            return core::Result<CharacterMoveResult>::failure(candidate_local.error().code,
                                                              candidate_local.error().message);
        }
        const auto physics_position = moved.value().position;
        auto candidate_status = character_->set_position(candidate_local.value());
        if (!candidate_status) {
            return core::Result<CharacterMoveResult>::failure(candidate_status.error().code,
                                                              candidate_status.error().message);
        }
        auto accepted =
            character_->set_shape(to_physics_shape(shape), 0.0F);
        if (!accepted) {
            (void)character_->set_position(physics_position);
            return core::Result<CharacterMoveResult>::failure(accepted.error().code,
                                                              accepted.error().message);
        }
        if (accepted.value()) {
            auto supported = character_->has_support(0.08F);
            if (!supported) {
                (void)character_->set_position(physics_position);
                return core::Result<CharacterMoveResult>::failure(supported.error().code,
                                                                  supported.error().message);
            }
            next = world_position(character_->position());
            if (!next) {
                (void)character_->set_position(physics_position);
                return core::Result<CharacterMoveResult>::failure(next.error().code,
                                                                  next.error().message);
            }
            applied = next.value().relative_to(position.anchor) - position.local_offset;
            stepped = true;
            grounded = supported.value() || voxel_move.grounded;
        } else {
            auto restore_status = character_->set_position(physics_position);
            if (!restore_status) {
                return core::Result<CharacterMoveResult>::failure(
                    restore_status.error().code, restore_status.error().message);
            }
        }
    }

    // Preserve exact voxel support when Jolt reports a transient unsupported state on a compound
    // subshape seam. The tight probe cannot turn an actual fall into a hovering grounded state.
    if (!grounded && desired_delta.y <= 0.0) {
        auto terrain_supported = voxel_queries_.has_support(next.value(), shape, 0.025);
        if (!terrain_supported) {
            return core::Result<CharacterMoveResult>::failure(terrain_supported.error().code,
                                                              terrain_supported.error().message);
        }
        grounded = terrain_supported.value();
    }

    bool edge_drop_prevented = false;
    if (prevent_edge_drop && !grounded &&
        (std::abs(desired_delta.x) > collision_epsilon ||
         std::abs(desired_delta.z) > collision_epsilon)) {
        auto supported_before = has_support(position, shape, 0.08);
        if (!supported_before) {
            return core::Result<CharacterMoveResult>::failure(supported_before.error().code,
                                                              supported_before.error().message);
        }
        if (supported_before.value()) {
            applied.x = 0.0;
            applied.z = 0.0;
            next =
                world::WorldPosition::from_anchor(position.anchor, position.local_offset + applied);
            if (!next) {
                return core::Result<CharacterMoveResult>::failure(next.error().code,
                                                                  next.error().message);
            }
            status = synchronize_position(next.value());
            if (!status) {
                return core::Result<CharacterMoveResult>::failure(status.error().code,
                                                                  status.error().message);
            }
            edge_drop_prevented = true;
        }
    }

    CharacterMoveResult result;
    result.position = next.value();
    result.requested_delta = desired_delta;
    result.applied_delta = applied;
    result.hit_x = std::abs(applied.x - desired_delta.x) > collision_epsilon;
    result.hit_y = std::abs(applied.y - desired_delta.y) > collision_epsilon;
    result.hit_z = std::abs(applied.z - desired_delta.z) > collision_epsilon;
    result.grounded = grounded || edge_drop_prevented;
    result.hit_ceiling = desired_delta.y > 0.0 && result.hit_y;
    result.stepped = stepped;
    return core::Result<CharacterMoveResult>::success(result);
}

core::Result<bool> PhysicsCharacterCollisionWorld::overlaps(const world::WorldPosition& position,
                                                            const CharacterShape& shape) {
    auto status = synchronize_position(position);
    if (!status) {
        return core::Result<bool>::failure(status.error().code, status.error().message);
    }
    auto switched = switch_shape(shape, true);
    if (!switched) {
        return core::Result<bool>::failure(switched.error().code, switched.error().message);
    }
    return core::Result<bool>::success(!switched.value());
}

core::Result<world::WorldPosition>
PhysicsCharacterCollisionWorld::depenetrate(const world::WorldPosition& position,
                                            const CharacterShape& shape,
                                            std::uint32_t maximum_iterations) {
    auto status = synchronize_position(position);
    if (!status) {
        return core::Result<world::WorldPosition>::failure(status.error().code,
                                                           status.error().message);
    }
    auto switched = switch_shape(shape);
    if (!switched) {
        return core::Result<world::WorldPosition>::failure(switched.error().code,
                                                           switched.error().message);
    }
    if (!switched.value()) {
        return core::Result<world::WorldPosition>::failure(
            "character_collision.shape_switch_blocked",
            "physics character shape is obstructed during depenetration");
    }
    auto recovered = character_->recover_from_penetration(maximum_iterations);
    if (!recovered) {
        return core::Result<world::WorldPosition>::failure(recovered.error().code,
                                                           recovered.error().message);
    }
    return world_position(recovered.value());
}

core::Result<bool> PhysicsCharacterCollisionWorld::has_support(const world::WorldPosition& position,
                                                               const CharacterShape& shape,
                                                               double probe_distance) {
    if (!std::isfinite(probe_distance) || probe_distance <= 0.0 || probe_distance > 2.0) {
        return core::Result<bool>::failure("character_collision.invalid_support_probe",
                                           "support probe distance must be in (0, 2]");
    }
    auto status = synchronize_position(position);
    if (!status) {
        return core::Result<bool>::failure(status.error().code, status.error().message);
    }
    auto switched = switch_shape(shape);
    if (!switched) {
        return core::Result<bool>::failure(switched.error().code, switched.error().message);
    }
    if (!switched.value()) {
        return core::Result<bool>::success(false);
    }
    return character_->has_support(static_cast<float>(probe_distance));
}

core::Result<std::optional<LedgeProbeResult>>
PhysicsCharacterCollisionWorld::probe_ledge(const world::WorldPosition& position,
                                            const CharacterShape& shape, math::Vec3d forward,
                                            double maximum_height, double reach) {
    return voxel_queries_.probe_ledge(position, shape, forward, maximum_height, reach);
}

core::Result<bool>
PhysicsCharacterCollisionWorld::touches_occupancy(const world::WorldPosition& position,
                                                  const CharacterShape& shape,
                                                  world::BlockLogicalOccupancy occupancy) {
    return voxel_queries_.touches_occupancy(position, shape, occupancy);
}

core::Result<double>
PhysicsCharacterCollisionWorld::fluid_submersion(const world::WorldPosition& position,
                                                 const CharacterShape& shape) {
    return voxel_queries_.fluid_submersion(position, shape);
}

core::Result<bool> PhysicsCharacterCollisionWorld::touches_tag(const world::WorldPosition& position,
                                                               const CharacterShape& shape,
                                                               std::string_view tag) {
    return voxel_queries_.touches_tag(position, shape, tag);
}

core::Status
PhysicsCharacterCollisionWorld::synchronize_position(const world::WorldPosition& position) {
    auto local = world::to_physics_local(position, config_.physics_island);
    if (!local) {
        return core::Status::failure(local.error().code, local.error().message);
    }
    if (math::length_squared(character_->position() - local.value()) <= 1.0e-8F) {
        return core::Status::ok();
    }
    return character_->set_position(local.value());
}

core::Result<bool> PhysicsCharacterCollisionWorld::switch_shape(CharacterShape shape,
                                                                bool force_test) {
    auto status = shape.validate();
    if (!status) {
        return core::Result<bool>::failure(status.error().code, status.error().message);
    }
    const auto requested = to_physics_shape(shape);
    const auto current = character_->shape();
    if (!force_test && requested.width == current.width && requested.height == current.height) {
        return core::Result<bool>::success(true);
    }
    return character_->set_shape(requested, static_cast<float>(config_.maximum_shape_penetration));
}

core::Result<world::WorldPosition>
PhysicsCharacterCollisionWorld::world_position(physics::Vec3 local_position) const {
    return world::from_physics_local(local_position, config_.physics_island);
}

} // namespace heartstead::movement
