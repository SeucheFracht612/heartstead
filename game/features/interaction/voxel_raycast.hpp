#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/coords/world_position.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <optional>

namespace heartstead::game::interaction {

struct VoxelRay {
    world::WorldPosition origin;
    math::Vec3d direction{};
    double maximum_distance = 6.0;
};

struct VoxelRaycastHit {
    world::BlockCoord block;
    world::BlockCoord adjacent_block;
    math::Coord3i face_normal{};
    world::VoxelCell cell;
    double distance = 0.0;
};

struct VoxelRaycastResult {
    std::optional<VoxelRaycastHit> hit;
    bool encountered_unloaded_chunk = false;
};

[[nodiscard]] core::Result<VoxelRaycastResult>
raycast_voxels(const world::ChunkDatabase& chunks, const VoxelRay& ray,
               const world::VoxelPalette* palette = nullptr);

} // namespace heartstead::game::interaction
