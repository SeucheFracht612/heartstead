#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/coords/world_position.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace heartstead::movement {

struct CharacterShape {
    double width = 0.6;
    double height = 1.8;

    [[nodiscard]] core::Status validate() const;
};

struct CharacterCollisionBox {
    math::Bounds3d bounds;
    world::BlockCoord block;
    const world::VoxelDefinition* voxel = nullptr;
    bool unloaded = false;
};

struct CharacterMoveResult {
    world::WorldPosition position;
    math::Vec3d requested_delta{};
    math::Vec3d applied_delta{};
    math::Vec3d depenetration_delta{};
    bool hit_x = false;
    bool hit_y = false;
    bool hit_z = false;
    bool grounded = false;
    bool hit_ceiling = false;
    bool stepped = false;
    bool blocked_by_unloaded_chunk = false;
};

struct LedgeProbeResult {
    world::WorldPosition target_feet;
    double ledge_height = 0.0;
    world::BlockCoord ledge_block;
};

class ICharacterCollisionWorld {
  public:
    virtual ~ICharacterCollisionWorld() = default;

    [[nodiscard]] virtual core::Result<CharacterMoveResult>
    move(const world::WorldPosition& position, const CharacterShape& shape,
         math::Vec3d desired_delta, double step_height = 0.0, bool prevent_edge_drop = false) = 0;

    [[nodiscard]] virtual core::Result<bool> overlaps(const world::WorldPosition& position,
                                                      const CharacterShape& shape) = 0;
    [[nodiscard]] virtual core::Result<world::WorldPosition>
    depenetrate(const world::WorldPosition& position, const CharacterShape& shape,
                std::uint32_t maximum_iterations = 8) = 0;
    [[nodiscard]] virtual core::Result<bool> has_support(const world::WorldPosition& position,
                                                         const CharacterShape& shape,
                                                         double probe_distance = 0.05) = 0;
    [[nodiscard]] virtual core::Result<std::optional<LedgeProbeResult>>
    probe_ledge(const world::WorldPosition& position, const CharacterShape& shape,
                math::Vec3d forward, double maximum_height, double reach = 0.7) = 0;
    [[nodiscard]] virtual core::Result<bool>
    touches_occupancy(const world::WorldPosition& position, const CharacterShape& shape,
                      world::BlockLogicalOccupancy occupancy) = 0;
    [[nodiscard]] virtual core::Result<double>
    fluid_submersion(const world::WorldPosition& position, const CharacterShape& shape) = 0;
    [[nodiscard]] virtual core::Result<bool> touches_tag(const world::WorldPosition& position,
                                                         const CharacterShape& shape,
                                                         std::string_view tag) = 0;
};

class VoxelCharacterCollisionWorld final : public ICharacterCollisionWorld {
  public:
    VoxelCharacterCollisionWorld(const world::ChunkDatabase& chunks,
                                 const world::VoxelPalette& palette) noexcept;

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
    [[nodiscard]] core::Result<std::vector<CharacterCollisionBox>>
    collision_boxes(world::BlockCoord origin, math::Bounds3d local_bounds) const;
    [[nodiscard]] core::Result<std::vector<CharacterCollisionBox>>
    voxel_boxes(world::BlockCoord origin, math::Bounds3d local_bounds,
                bool include_non_colliding) const;

    const world::ChunkDatabase* chunks_ = nullptr;
    const world::VoxelPalette* palette_ = nullptr;
};

[[nodiscard]] math::Bounds3d character_local_bounds(const world::WorldPosition& position,
                                                    world::BlockCoord origin,
                                                    const CharacterShape& shape) noexcept;

} // namespace heartstead::movement
