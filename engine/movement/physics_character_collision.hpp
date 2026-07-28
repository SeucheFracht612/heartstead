#pragma once

#include "engine/movement/character_collision.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/world/coords/world_position.hpp"

#include <memory>

namespace heartstead::movement {

struct PhysicsCharacterCollisionConfig {
    world::PhysicsIslandFrame physics_island{};
    double fixed_delta_seconds = 1.0 / 60.0;
    double maximum_shape_penetration = 0.05;
    double maximum_slope_angle_degrees = 50.0;
    double mass = 70.0;
    double maximum_strength = 100.0;
    double padding = 0.02;

    [[nodiscard]] core::Status validate() const;
};

class PhysicsCharacterCollisionWorld final : public ICharacterCollisionWorld {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<PhysicsCharacterCollisionWorld>>
    create(physics::IPhysicsWorld& physics_world, const world::ChunkDatabase& chunks,
           const world::VoxelPalette& palette, const world::WorldPosition& initial_position,
           CharacterShape initial_shape, PhysicsCharacterCollisionConfig config = {});

    [[nodiscard]] physics::PhysicsBackend backend() const noexcept;
    [[nodiscard]] core::Status set_fixed_delta_seconds(double fixed_delta_seconds);

    [[nodiscard]] core::Result<CharacterMoveResult> move(const world::WorldPosition& position,
                                                         const CharacterShape& shape,
                                                         math::Vec3d desired_delta,
                                                         double step_height = 0.0,
                                                         bool prevent_edge_drop = false) override;
    [[nodiscard]] core::Result<bool> overlaps(const world::WorldPosition& position,
                                              const CharacterShape& shape) override;
    [[nodiscard]] core::Result<world::WorldPosition>
    depenetrate(const world::WorldPosition& position, const CharacterShape& shape,
                std::uint32_t maximum_iterations = 8) override;
    [[nodiscard]] core::Result<bool> has_support(const world::WorldPosition& position,
                                                 const CharacterShape& shape,
                                                 double probe_distance = 0.05) override;
    [[nodiscard]] core::Result<std::optional<LedgeProbeResult>>
    probe_ledge(const world::WorldPosition& position, const CharacterShape& shape,
                math::Vec3d forward, double maximum_height, double reach = 0.7) override;
    [[nodiscard]] core::Result<bool>
    touches_occupancy(const world::WorldPosition& position, const CharacterShape& shape,
                      world::BlockLogicalOccupancy occupancy) override;
    [[nodiscard]] core::Result<double>
    fluid_submersion(const world::WorldPosition& position,
                     const CharacterShape& shape) override;
    [[nodiscard]] core::Result<bool> touches_tag(const world::WorldPosition& position,
                                                 const CharacterShape& shape,
                                                 std::string_view tag) override;

  private:
    PhysicsCharacterCollisionWorld(const world::ChunkDatabase& chunks,
                                   const world::VoxelPalette& palette,
                                   PhysicsCharacterCollisionConfig config,
                                   std::unique_ptr<physics::IPhysicsCharacter> character);

    [[nodiscard]] core::Status synchronize_position(const world::WorldPosition& position);
    [[nodiscard]] core::Result<bool> switch_shape(CharacterShape shape, bool force_test = false);
    [[nodiscard]] core::Result<world::WorldPosition>
    world_position(physics::Vec3 local_position) const;

    PhysicsCharacterCollisionConfig config_{};
    std::unique_ptr<physics::IPhysicsCharacter> character_;
    VoxelCharacterCollisionWorld voxel_queries_;
};

} // namespace heartstead::movement
