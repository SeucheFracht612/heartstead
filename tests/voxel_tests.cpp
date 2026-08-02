#include "heartstead/core/types.hpp"
#include "heartstead/voxel/greedy_mesher.hpp"
#include "heartstead/world/chunk_world.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void coordinate_tests() {
    using heartstead::floor_div;
    using heartstead::floor_mod;
    check(floor_div(31, 32) == 0 && floor_mod(31, 32) == 31, "positive local coordinate");
    check(floor_div(32, 32) == 1 && floor_mod(32, 32) == 0, "positive chunk boundary");
    check(floor_div(-1, 32) == -1 && floor_mod(-1, 32) == 31, "negative local coordinate");
    check(floor_div(-32, 32) == -1 && floor_mod(-32, 32) == 0, "negative chunk boundary");
    check(floor_div(-33, 32) == -2 && floor_mod(-33, 32) == 31, "past negative boundary");
}

void storage_tests() {
    using namespace heartstead;
    Chunk chunk(7);
    check(chunk.storage_mode() == Chunk::StorageMode::uniform, "new chunks use uniform storage");
    check(chunk.get(4, 5, 6) == 7, "uniform reads return the fill block");
    check(chunk.get(-1, 0, 0) == air_block, "out-of-bounds reads return air");

    chunk.set(4, 5, 6, 8);
    check(chunk.storage_mode() == Chunk::StorageMode::palette, "first edit promotes to palette storage");
    check(chunk.get(4, 5, 6) == 8 && chunk.get(4, 5, 7) == 7, "palette edit preserves other voxels");

    // Exercise every packed bit width, including values that cross 64-bit word boundaries.
    for (heartstead::BlockId id = 9; id < 130; ++id) {
        chunk.set(static_cast<std::size_t>(id), id);
    }
    for (heartstead::BlockId id = 9; id < 130; ++id) {
        check(chunk.get(static_cast<std::size_t>(id)) == id, "bit-packed palette round trip");
    }

    Chunk varied;
    for (heartstead::BlockId id = 1; id <= 256; ++id) {
        varied.set(static_cast<std::size_t>(id), id);
    }
    check(varied.storage_mode() == Chunk::StorageMode::direct, "large palettes promote to direct storage");
    check(varied.get(1) == 1 && varied.get(256) == 256, "direct promotion preserves values");

    const auto revision = varied.revision();
    varied.set(1, 1);
    check(varied.revision() == revision, "no-op writes do not invalidate a chunk");
}

void meshing_tests() {
    using namespace heartstead;
    const auto blocks = BlockRegistry::defaults();
    check(blocks.is_renderable(5) && blocks.is_occluding(5), "tree trunks are opaque blocks");
    check(blocks.is_renderable(6) && !blocks.is_occluding(6), "tree leaves are renderable but non-occluding");
    check(blocks.is_solid(1) && blocks.is_solid(6) && !blocks.is_solid(air_block),
        "player collision uses solid block flags");
    check(blocks.is_occluding(7) && blocks.is_occluding(8), "sand and snow are opaque terrain blocks");

    Chunk empty;
    const auto empty_mesh = GreedyMesher::build(empty, blocks);
    check(empty_mesh.empty() && empty_mesh.quad_count == 0, "empty chunks generate no geometry");

    Chunk single;
    single.set(10, 11, 12, 1);
    const auto single_mesh = GreedyMesher::build(single, blocks);
    check(single_mesh.quad_count == 6, "one voxel has six faces");
    check(single_mesh.vertices.size() == 24 && single_mesh.indices.size() == 36, "one voxel mesh counts");

    Chunk solid(1);
    const auto solid_mesh = GreedyMesher::build(solid, blocks);
    check(solid_mesh.quad_count == 6, "greedy meshing reduces a solid chunk to six quads");
    check(solid_mesh.vertices.size() == 24 && solid_mesh.indices.size() == 36, "solid chunk mesh counts");

    Chunk pair;
    pair.set(1, 1, 1, 1);
    pair.set(2, 1, 1, 2);
    const auto pair_mesh = GreedyMesher::build(pair, blocks);
    check(pair_mesh.quad_count == 10, "material boundaries prevent invalid face merging");

    Chunk glass;
    glass.set(1, 1, 1, 4);
    glass.set(2, 1, 1, 4);
    const auto glass_mesh = GreedyMesher::build(glass, blocks);
    check(glass_mesh.quad_count == 6, "identical transparent blocks cull their shared face");

    Chunk solid_neighbor(1);
    const auto left_mesh = GreedyMesher::build(solid, blocks, {.positive_x = &solid_neighbor});
    const auto right_mesh = GreedyMesher::build(solid_neighbor, blocks, {.negative_x = &solid});
    check(left_mesh.quad_count == 5 && right_mesh.quad_count == 5,
        "neighbor-aware meshing culls shared chunk faces");

    Chunk boundary_voxel;
    boundary_voxel.set(0, 1, 1, 1);
    const auto empty_neighbor_mesh = GreedyMesher::build(empty, blocks, {.positive_x = &boundary_voxel});
    const auto owned_boundary_mesh = GreedyMesher::build(boundary_voxel, blocks, {.negative_x = &empty});
    check(empty_neighbor_mesh.empty() && owned_boundary_mesh.quad_count == 6,
        "boundary geometry is emitted only by its owning chunk");
}

void world_tests() {
    using namespace heartstead;
    ChunkWorld world;
    world.set_block({-1, -1, -33}, 7);
    check(world.chunk_count() == 1, "world creates sparse chunks on demand");
    check(world.find_chunk({-1, -1, -2}) != nullptr, "negative world coordinates select the correct chunk");
    check(world.get_block({-1, -1, -33}) == 7, "world block lookup crosses negative chunk boundaries");
    check(world.get_block({1000, 1000, 1000}) == air_block, "missing world chunks read as air");

    Chunk streamed_chunk(3);
    world.set_chunk({12, 0, -8}, std::move(streamed_chunk));
    check(world.find_chunk({12, 0, -8}) != nullptr && world.find_chunk({12, 0, -8})->get(0) == 3,
        "streaming inserts complete chunks");
    auto extracted = world.take_chunk({12, 0, -8});
    check(extracted.has_value() && extracted->get(0) == 3 && world.find_chunk({12, 0, -8}) == nullptr,
        "streaming extracts chunks without copying voxel storage");
    world.set_chunk({12, 0, -8}, std::move(*extracted));
    check(world.erase_chunk({12, 0, -8}) && !world.erase_chunk({12, 0, -8}),
        "streaming erases chunks and reports whether they existed");

    for (std::int32_t z = 0; z < 64; ++z) {
        for (std::int32_t x = 0; x < 64; ++x) {
            world.ensure_chunk({x, 0, z});
        }
    }
    check(world.chunk_count() == 4097, "world stores an independent 64x64 chunk test area");
}

} // namespace

int main() {
    coordinate_tests();
    storage_tests();
    meshing_tests();
    world_tests();

    if (failures == 0) {
        std::cout << "All voxel core tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " test(s) failed.\n";
    return EXIT_FAILURE;
}
