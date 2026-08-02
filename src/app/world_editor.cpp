#include "heartstead/app/world_editor.hpp"

#include "heartstead/render/opengl_renderer.hpp"
#include "heartstead/voxel/block_registry.hpp"
#include "heartstead/voxel/chunk.hpp"
#include "heartstead/voxel/greedy_mesher.hpp"
#include "heartstead/world/chunk_world.hpp"
#include "heartstead/world/world_generation.hpp"

#include <vector>

namespace heartstead::app {

bool WorldEditor::set_block(
    ChunkWorld& chunk_world,
    world::WorldEdits& edits,
    OpenGlRenderer& renderer,
    Int3 position,
    BlockId block) {
    if (position.y < 0 || position.y >= Chunk::edge) return true;
    const Int3 chunk_coordinate{
        floor_div(position.x, Chunk::edge), 0, floor_div(position.z, Chunk::edge)};
    const auto local_x = floor_mod(position.x, Chunk::edge);
    const auto local_y = floor_mod(position.y, Chunk::edge);
    const auto local_z = floor_mod(position.z, Chunk::edge);
    const auto index = Chunk::linear_index(local_x, local_y, local_z);
    edits.chunks[chunk_coordinate][index] = block;
    chunk_world.set_block(position, block);

    std::vector<Int3> coordinates{chunk_coordinate};
    if (local_x == 0) coordinates.push_back({chunk_coordinate.x - 1, 0, chunk_coordinate.z});
    if (local_x == Chunk::edge - 1)
        coordinates.push_back({chunk_coordinate.x + 1, 0, chunk_coordinate.z});
    if (local_z == 0) coordinates.push_back({chunk_coordinate.x, 0, chunk_coordinate.z - 1});
    if (local_z == Chunk::edge - 1)
        coordinates.push_back({chunk_coordinate.x, 0, chunk_coordinate.z + 1});

    const auto blocks = BlockRegistry::defaults();
    std::vector<ChunkMeshUpdate> mesh_updates;
    mesh_updates.reserve(coordinates.size());
    for (const auto coordinate : coordinates) {
        if (!renderer.has_chunk(coordinate)) continue;
        const auto* chunk = chunk_world.find_chunk(coordinate);
        if (chunk == nullptr) continue;
        const ChunkNeighbors neighbors{
            .negative_x = chunk_world.find_chunk({coordinate.x - 1, coordinate.y, coordinate.z}),
            .positive_x = chunk_world.find_chunk({coordinate.x + 1, coordinate.y, coordinate.z}),
            .negative_y = chunk_world.find_chunk({coordinate.x, coordinate.y - 1, coordinate.z}),
            .positive_y = chunk_world.find_chunk({coordinate.x, coordinate.y + 1, coordinate.z}),
            .negative_z = chunk_world.find_chunk({coordinate.x, coordinate.y, coordinate.z - 1}),
            .positive_z = chunk_world.find_chunk({coordinate.x, coordinate.y, coordinate.z + 1}),
        };
        mesh_updates.push_back({coordinate, GreedyMesher::build(*chunk, blocks, neighbors)});
    }
    return renderer.apply_chunk_updates(mesh_updates, {});
}

} // namespace heartstead::app
