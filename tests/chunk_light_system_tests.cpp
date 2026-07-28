#include "engine/dirty/dirty_region.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/lighting/chunk_light_system.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using namespace heartstead;

[[nodiscard]] world::VoxelDefinition voxel_definition(std::uint16_t type, const char* id,
                                                      std::uint8_t absorption = 255,
                                                      std::uint8_t emission = 0) {
    world::VoxelDefinition result;
    result.type = type;
    result.prototype_id = *core::PrototypeId::parse(id);
    result.display_name = id;
    result.terrain_material = "stone";
    result.mining_tool = "pick";
    result.light_absorption = absorption;
    result.light_emission = emission;
    return result;
}

[[nodiscard]] world::VoxelPalette light_palette() {
    world::VoxelPalette result;
    assert(result.add(voxel_definition(1, "test:voxels/opaque")));
    assert(result.add(voxel_definition(2, "test:voxels/lamp", 255, 224)));
    return result;
}

void insert_filled_chunk(world::ChunkDatabase& chunks, dirty::DirtyRegionTracker& dirty_regions,
                         world::ChunkCoord coordinate, world::VoxelCell cell) {
    std::vector<world::VoxelCell> cells(world::VoxelChunk::total_cells, cell);
    world::VoxelChunk chunk(coordinate);
    assert(chunk.load_generated_cells(std::move(cells)));
    assert(chunks.insert_generated(std::move(chunk), dirty_regions));
}

void run_until_applied(world::ChunkLightSystem& system, world::ChunkDatabase& chunks,
                       dirty::DirtyRegionTracker& dirty_regions, const world::VoxelPalette& palette,
                       std::uint64_t target) {
    for (std::size_t update = 0; update < 5'000 && system.stats().applied_fields < target;
         ++update) {
        assert(system.update(chunks, dirty_regions, palette));
        assert(system.stats().snapshot_cells_copied_this_update <= 4096);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(system.stats().applied_fields >= target);
}

void test_invalid_configuration() {
    const auto palette = light_palette();
    world::ChunkLightSystemConfig config;
    config.max_snapshot_cells_per_update = 0;
    assert(!world::ChunkLightSystem::create(palette, config));
    config.max_snapshot_cells_per_update = 1;
    config.apply_time_budget_ms = 0.0;
    assert(!world::ChunkLightSystem::create(palette, config));
}

void test_budgeted_snapshot_and_async_apply() {
    const auto palette = light_palette();
    world::ChunkDatabase chunks;
    dirty::DirtyRegionTracker dirty_regions;
    insert_filled_chunk(chunks, dirty_regions, {0, 0, 0}, world::VoxelCell::air());

    world::ChunkLightSystemConfig config;
    config.max_snapshot_cells_per_update = 4096;
    auto system = world::ChunkLightSystem::create(palette, config);
    assert(system);
    run_until_applied(*system.value(), chunks, dirty_regions, palette, 1);

    const auto cell = chunks.get({0, 0, 0}, {8, 8, 8});
    assert(cell);
    assert(cell.value().light == 255);
    assert(system.value()->stats().submitted_fields == 1);
    assert(system.value()->stats().total_snapshot_cells_copied == world::VoxelChunk::total_cells);
    assert(system.value()->stats().total_sunlight_queue_visits > 0);
    assert(system.value()->stats().changed_chunks_this_update == 1);
    assert(system.value()->changed_chunks().size() == 1);
}

void test_snapshot_revision_change_restarts_without_applying_stale_light() {
    const auto palette = light_palette();
    world::ChunkDatabase chunks;
    dirty::DirtyRegionTracker dirty_regions;
    insert_filled_chunk(chunks, dirty_regions, {0, 0, 0}, world::VoxelCell{1, 0});

    world::ChunkLightSystemConfig config;
    config.max_snapshot_cells_per_update = 1024;
    auto system = world::ChunkLightSystem::create(palette, config);
    assert(system);
    assert(system.value()->update(chunks, dirty_regions, palette));
    assert(system.value()->stats().snapshot_in_progress);
    assert(chunks.set({0, 0, 0}, {4, 4, 4}, world::VoxelCell::air(), dirty_regions, palette));
    assert(system.value()->update(chunks, dirty_regions, palette));
    assert(system.value()->stats().stale_snapshots == 1);

    run_until_applied(*system.value(), chunks, dirty_regions, palette, 1);
    const auto carved = chunks.get({0, 0, 0}, {4, 4, 4});
    assert(carved);
    assert(carved.value().light == 0);
}

void test_emitter_removal_relights_across_chunk_boundary() {
    const auto palette = light_palette();
    world::ChunkDatabase chunks;
    dirty::DirtyRegionTracker dirty_regions;
    insert_filled_chunk(chunks, dirty_regions, {0, 0, 0}, world::VoxelCell{1, 0});
    insert_filled_chunk(chunks, dirty_regions, {1, 0, 0}, world::VoxelCell{1, 0});
    assert(chunks.set({0, 0, 0}, {31, 5, 5}, world::VoxelCell{2, 0}, dirty_regions, palette));
    for (std::uint16_t x = 0; x <= 4; ++x) {
        assert(chunks.set({1, 0, 0}, {x, 5, 5}, world::VoxelCell::air(), dirty_regions, palette));
    }

    world::ChunkLightSystemConfig config;
    config.max_snapshot_cells_per_update = 4096;
    auto system = world::ChunkLightSystem::create(palette, config);
    assert(system);
    run_until_applied(*system.value(), chunks, dirty_regions, palette, 1);
    auto lit = chunks.get({1, 0, 0}, {4, 5, 5});
    assert(lit);
    assert(lit.value().light == 144);

    assert(chunks.set({0, 0, 0}, {31, 5, 5}, world::VoxelCell{1, 0}, dirty_regions, palette));
    run_until_applied(*system.value(), chunks, dirty_regions, palette, 2);
    const auto dark = chunks.get({1, 0, 0}, {4, 5, 5});
    assert(dark);
    assert(dark.value().light == 0);
    assert(system.value()->stats().total_block_light_queue_visits > 0);
}

} // namespace

int main() {
    test_invalid_configuration();
    test_budgeted_snapshot_and_async_apply();
    test_snapshot_revision_change_restarts_without_applying_stale_light();
    test_emitter_removal_relights_across_chunk_boundary();
    return 0;
}
