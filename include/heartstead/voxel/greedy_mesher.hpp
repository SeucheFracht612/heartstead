#pragma once

#include "heartstead/voxel/block_registry.hpp"
#include "heartstead/voxel/chunk.hpp"
#include "heartstead/voxel/mesh.hpp"

namespace heartstead {

struct ChunkNeighbors {
    const Chunk* negative_x{};
    const Chunk* positive_x{};
    const Chunk* negative_y{};
    const Chunk* positive_y{};
    const Chunk* negative_z{};
    const Chunk* positive_z{};
};

class GreedyMesher {
public:
    [[nodiscard]] static ChunkMesh build(
        const Chunk& chunk,
        const BlockRegistry& blocks,
        const ChunkNeighbors& neighbors = {});
};

} // namespace heartstead
