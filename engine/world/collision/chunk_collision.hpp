#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/chunks/chunk_identity.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace heartstead::world {

struct ChunkCollisionBlockInfo {
    bool defined = false;
    std::vector<math::Bounds3f> bounds;

    [[nodiscard]] bool is_full_cube() const noexcept;
};

struct ChunkCollisionTableSnapshot {
    std::uint64_t revision = 1;
    bool legacy_cube_fallback = false;
    std::vector<ChunkCollisionBlockInfo> blocks;

    [[nodiscard]] const ChunkCollisionBlockInfo* find(std::uint16_t type) const noexcept;
    [[nodiscard]] core::Status validate() const;
};

struct ChunkCollisionSnapshot {
    ChunkIdentity identity{};
    std::uint64_t content_revision = 0;
    std::uint64_t collision_table_revision = 0;
    std::vector<VoxelCell> cells;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkCollisionCookStats {
    std::uint32_t source_colliding_cells = 0;
    std::uint32_t source_collision_boxes = 0;
    std::uint32_t merged_full_cube_boxes = 0;
    std::uint32_t output_boxes = 0;
};

struct ChunkCollisionShape {
    ChunkIdentity identity{};
    std::uint64_t content_revision = 0;
    std::uint64_t collision_table_revision = 0;
    std::vector<physics::CompoundShapeChild> boxes;
    ChunkCollisionCookStats stats{};

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] core::Status validate() const;
};

[[nodiscard]] core::Result<ChunkCollisionTableSnapshot>
build_chunk_collision_table_snapshot(const VoxelPalette* palette);

[[nodiscard]] core::Result<ChunkCollisionSnapshot>
build_chunk_collision_snapshot(const ChunkDatabase& chunks, ChunkIdentity identity,
                               const ChunkCollisionTableSnapshot& collision_table,
                               std::vector<VoxelCell> reusable_cells = {});

[[nodiscard]] core::Result<ChunkCollisionShape>
cook_chunk_collision(const ChunkCollisionSnapshot& snapshot,
                     const ChunkCollisionTableSnapshot& collision_table);

} // namespace heartstead::world
