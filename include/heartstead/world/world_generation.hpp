#pragma once

#include "heartstead/core/types.hpp"
#include "heartstead/voxel/chunk.hpp"
#include "heartstead/voxel/mesh.hpp"
#include "heartstead/world/chunk_world.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace heartstead::world {

struct WorldArea {
    std::int32_t chunks_per_axis{64};
    Int3 center_chunk{};

    [[nodiscard]] std::int32_t first_x() const noexcept { return center_chunk.x - chunks_per_axis / 2; }
    [[nodiscard]] std::int32_t first_z() const noexcept { return center_chunk.z - chunks_per_axis / 2; }
    [[nodiscard]] std::int32_t minimum_x() const noexcept { return first_x() * Chunk::edge; }
    [[nodiscard]] std::int32_t minimum_z() const noexcept { return first_z() * Chunk::edge; }
    [[nodiscard]] std::int32_t maximum_x() const noexcept {
        return (first_x() + chunks_per_axis) * Chunk::edge;
    }
    [[nodiscard]] std::int32_t maximum_z() const noexcept {
        return (first_z() + chunks_per_axis) * Chunk::edge;
    }
};

struct WorldEdits {
    std::unordered_map<Int3, std::unordered_map<std::size_t, BlockId>> chunks;
};

[[nodiscard]] std::vector<Int3> world_coordinates(const WorldArea& area);
[[nodiscard]] Chunk generate_chunk(Int3 chunk_coordinate, const WorldEdits& edits);
[[nodiscard]] ChunkWorld make_world(const WorldArea& area, const WorldEdits& edits);
[[nodiscard]] WorldMesh mesh_world(const ChunkWorld& world, const WorldArea& area);

} // namespace heartstead::world
