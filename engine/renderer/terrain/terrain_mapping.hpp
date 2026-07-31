#pragma once

#include "engine/world/voxels/voxel_chunk.hpp"

#include <cstdint>

namespace heartstead::renderer::terrain {

// Packs signed chunk coordinates into ten bits per axis. The shader sign-extends each axis before
// reconstructing its stable position, preserving phase across the -1/0 world-coordinate planes.
[[nodiscard]] constexpr std::uint32_t
terrain_coordinate_key(world::ChunkCoord coordinate) noexcept {
    const auto axis = [](std::int64_t value) {
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) & 0x3ffU);
    };
    return axis(coordinate.x) | (axis(coordinate.y) << 10U) | (axis(coordinate.z) << 20U);
}

[[nodiscard]] constexpr std::uint32_t stable_terrain_variant_hash(world::ChunkCoord chunk,
                                                                  world::VoxelCoord local,
                                                                  std::uint32_t face) noexcept {
    const auto periodic_axis = [](std::int64_t chunk_axis, std::uint16_t local_axis) {
        return (static_cast<std::uint32_t>(static_cast<std::uint64_t>(chunk_axis) & 0x3ffU) *
                    world::VoxelChunk::edge_length +
                local_axis) &
               0x7fffU;
    };
    const auto x = periodic_axis(chunk.x, local.x);
    const auto y = periodic_axis(chunk.y, local.y);
    const auto z = periodic_axis(chunk.z, local.z);
    std::uint32_t hash = 0x4f1bbcdcU ^ face * 0x9e3779b9U;
    hash ^= x * 0x85ebca6bU;
    hash ^= y * 0xc2b2ae35U;
    hash ^= z * 0x27d4eb2fU;
    hash ^= hash >> 16U;
    hash *= 0x7feb352dU;
    hash ^= hash >> 15U;
    hash *= 0x846ca68bU;
    return hash ^ (hash >> 16U);
}

struct TerrainVariantSelection {
    std::uint32_t variant = 0;
    std::uint8_t quarter_turns = 0;
    bool mirrored = false;

    friend bool operator==(const TerrainVariantSelection&,
                           const TerrainVariantSelection&) = default;
};

[[nodiscard]] constexpr TerrainVariantSelection
select_terrain_variant(world::ChunkCoord chunk, world::VoxelCoord local, std::uint32_t face,
                       std::uint32_t variant_count, bool allow_rotations,
                       bool allow_mirroring) noexcept {
    const auto hash = stable_terrain_variant_hash(chunk, local, face);
    return {
        variant_count == 0U ? 0U : hash % variant_count,
        static_cast<std::uint8_t>(allow_rotations ? hash & 3U : 0U),
        allow_mirroring && (hash & 4U) != 0U,
    };
}

} // namespace heartstead::renderer::terrain
