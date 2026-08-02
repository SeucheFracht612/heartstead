#include "engine/renderer/terrain/far_terrain_world_surface.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

[[nodiscard]] heartstead::world::VoxelChunk make_surface_chunk(heartstead::world::ChunkCoord coord,
                                                               std::uint16_t local_height,
                                                               std::uint16_t material) {
    using namespace heartstead;
    std::vector<world::VoxelCell> cells(world::VoxelChunk::total_cells, world::VoxelCell::air());
    constexpr auto edge = static_cast<std::size_t>(world::VoxelChunk::edge_length);
    for (std::size_t z = 0; z < edge; ++z) {
        for (std::size_t y = 0; y <= local_height; ++y) {
            for (std::size_t x = 0; x < edge; ++x) {
                cells[z * edge * edge + y * edge + x] = world::VoxelCell{material, 0};
            }
        }
    }
    world::VoxelChunk chunk(coord);
    assert(chunk.load_generated_cells(std::move(cells)));
    return chunk;
}

void test_cache_tracks_changed_horizontal_columns() {
    using namespace heartstead;
    world::WorldState world;
    assert(world.chunks().insert_generated(make_surface_chunk({0, 0, 0}, 2, 1),
                                           world.dirty_regions()));
    assert(world.chunks().insert_generated(make_surface_chunk({0, 1, 0}, 1, 2),
                                           world.dirty_regions()));
    assert(world.chunks().insert_generated(make_surface_chunk({1, 0, 0}, 4, 3),
                                           world.dirty_regions()));

    renderer::FarTerrainWorldSurfaceCache cache;
    const auto initial_revision = renderer::far_terrain_world_surface_revision(world);
    const auto initial_regions = cache.synchronize(world, initial_revision);
    assert(initial_regions.size() == 2);
    assert(cache.revision() == initial_revision);
    assert(cache.sample_count() ==
           2U * world::VoxelChunk::edge_length * world::VoxelChunk::edge_length);

    const auto upper = cache.sample(3.25, 7.75, renderer::FarTerrainDomain::surface);
    assert(upper.valid);
    assert(upper.height == 34.0);
    assert(upper.material == 2);
    const auto adjacent = cache.sample(34.5, 5.5, renderer::FarTerrainDomain::surface);
    assert(adjacent.valid);
    assert(adjacent.height == 5.0);
    assert(adjacent.material == 3);
    assert(!cache.sample(3.0, 7.0, renderer::FarTerrainDomain::underground).valid);
    assert(!cache
                .sample(std::numeric_limits<double>::infinity(), 0.0,
                        renderer::FarTerrainDomain::surface)
                .valid);

    assert(world.chunks().set({1, 0, 0}, {4, 8, 4}, world::VoxelCell{3, 0}, world.dirty_regions()));
    const auto edited_revision = renderer::far_terrain_world_surface_revision(world);
    assert(edited_revision != initial_revision);
    const auto edited_regions = cache.synchronize(world, edited_revision);
    assert(edited_regions.size() == 1);
    assert(edited_regions.front().min.x == 32.0);
    assert(edited_regions.front().max.x == 64.0);
    assert(cache.sample(36.0, 4.0, renderer::FarTerrainDomain::surface).height == 9.0);

    assert(world.chunks().erase({0, 1, 0}));
    const auto unloaded_revision = renderer::far_terrain_world_surface_revision(world);
    const auto unloaded_regions = cache.synchronize(world, unloaded_revision);
    assert(unloaded_regions.size() == 1);
    const auto exposed = cache.sample(3.0, 7.0, renderer::FarTerrainDomain::surface);
    assert(exposed.valid);
    assert(exposed.height == 3.0);
    assert(exposed.material == 1);

    assert(cache.synchronize(world, unloaded_revision).empty());
    cache.clear();
    assert(cache.revision() == 0);
    assert(cache.sample_count() == 0);
    assert(!cache.sample(3.0, 7.0, renderer::FarTerrainDomain::surface).valid);
}

void test_direct_sampler_matches_cache() {
    using namespace heartstead;
    world::WorldState world;
    assert(world.chunks().insert_generated(make_surface_chunk({-1, 0, 2}, 6, 4),
                                           world.dirty_regions()));
    renderer::FarTerrainWorldSurfaceCache cache;
    static_cast<void>(
        cache.synchronize(world, renderer::far_terrain_world_surface_revision(world)));
    const auto direct = renderer::sample_far_terrain_world_surface(
        world, -4.25, 70.5, renderer::FarTerrainDomain::surface);
    const auto cached = cache.sample(-4.25, 70.5, renderer::FarTerrainDomain::surface);
    assert(direct.valid && cached.valid);
    assert(direct.height == cached.height);
    assert(direct.material == cached.material);
}

} // namespace

int main() {
    test_cache_tracks_changed_horizontal_columns();
    test_direct_sampler_matches_cache();
    return 0;
}
