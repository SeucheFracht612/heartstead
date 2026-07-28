#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/chunks/chunk_edit_delta_codec.hpp"
#include "engine/world/fluids/chunk_fluid_system.hpp"
#include "engine/world/fluids/fluid_simulation.hpp"
#include "engine/world/fluids/fluid_state.hpp"
#include "engine/world/meshing/chunk_mesh_snapshot.hpp"
#include "engine/world/meshing/greedy_chunk_mesher.hpp"
#include "engine/world/regions/region_graph.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "engine/world/worldgen/terrain_generator.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] heartstead::core::PrototypeId id(std::string_view value) {
    auto parsed = heartstead::core::PrototypeId::parse(value);
    assert(parsed);
    return parsed.value();
}

struct FluidFixture {
    heartstead::world::VoxelPalette palette;
    heartstead::world::RegionGraph regions;
    heartstead::core::PrototypeId stone_id = id("test:voxels/stone");
    heartstead::core::PrototypeId water_id = id("test:voxels/water");

    FluidFixture() {
        heartstead::world::VoxelDefinition stone;
        stone.type = 1;
        stone.prototype_id = stone_id;
        stone.display_name = "Stone";
        stone.terrain_material = "stone";
        stone.mining_tool = "pickaxe";
        assert(palette.add(std::move(stone)));

        heartstead::world::VoxelDefinition water;
        water.type = 2;
        water.prototype_id = water_id;
        water.display_name = "Water";
        water.terrain_material = "water";
        water.mining_tool = "bucket";
        water.logical_occupancy = heartstead::world::BlockLogicalOccupancy::fluid;
        water.collision_bounds.clear();
        water.occlusion_bounds.clear();
        water.occlusion = heartstead::world::BlockOcclusionBehavior::none;
        water.light_absorption = 8;
        assert(palette.add(std::move(water)));

        heartstead::world::RegionDescriptor region;
        region.id = "test_ocean";
        region.age = "age_0";
        region.biome_cluster = "coast";
        region.resource_rules.push_back({stone_id, "terrain", 1.0});
        assert(regions.add_region(std::move(region)));
    }
};

void test_fluid_state_round_trip_and_rejects_reserved_state() {
    using namespace heartstead::world;
    const FluidState expected{5, true, false, FluidFlowDirection::positive_z};
    auto encoded = encode_fluid_state(expected);
    assert(encoded);
    assert((encoded.value() & fluid_amount_mask) == 5);
    assert((encoded.value() & fluid_falling_bit) != 0);
    assert(decode_fluid_state(encoded.value()).value() == expected);

    auto partial_source =
        encode_fluid_state({7, false, true, FluidFlowDirection::none});
    assert(!partial_source);
    assert(partial_source.error().code == "fluid_state.partial_source");
    auto reserved = decode_fluid_state(0x8008U);
    assert(!reserved);
    assert(reserved.error().code == "fluid_state.reserved_bits");
    assert(fluid_surface_height(1) == 0.125F);
    assert(fluid_surface_height(maximum_fluid_amount) == 1.0F);
}

void test_palette_creates_full_finite_fluid_cells() {
    using namespace heartstead::world;
    FluidFixture fixture;
    auto water = fixture.palette.cell_for(fixture.water_id, 91);
    assert(water);
    assert(water.value().light == 91);
    auto state = decode_fluid_cell(water.value(), fixture.palette);
    assert(state);
    assert((state.value() == FluidState{maximum_fluid_amount, false, false,
                                       FluidFlowDirection::none}));

    auto invalid = water.value();
    invalid.metadata_handle = 4;
    assert(!decode_fluid_cell(invalid, fixture.palette));
    assert(!decode_fluid_cell({1, 0, full_fluid_state_bits()}, fixture.palette));
}

void test_worldgen_seeds_ocean_sources_below_sea_level() {
    using namespace heartstead::world;
    FluidFixture fixture;
    TerrainGenerationConfig config;
    config.world_seed = 42;
    config.region_id = "test_ocean";
    config.base_surface_y = 1;
    config.sea_level = 4;
    config.ocean_voxel_id = fixture.water_id;
    auto generated = DeterministicTerrainGenerator::generate_chunk(
        {0, 0, 0}, config, fixture.regions, fixture.palette);
    assert(generated);
    assert(generated.value().get({0, 1, 0}).value().type == 1);
    for (std::uint16_t y = 2; y <= 4; ++y) {
        const auto cell = generated.value().get({0, y, 0}).value();
        assert(cell.type == 2);
        const auto state = decode_fluid_cell(cell, fixture.palette);
        assert(state);
        assert(state.value().source);
        assert(state.value().amount == maximum_fluid_amount);
    }
    assert(generated.value().get({0, 5, 0}).value().is_air());

    config.ocean_voxel_id = fixture.stone_id;
    auto invalid = DeterministicTerrainGenerator::generate_chunk(
        {0, 0, 0}, config, fixture.regions, fixture.palette);
    assert(!invalid);
    assert(invalid.error().code == "terrain_generator.invalid_ocean_voxel");
}

[[nodiscard]] heartstead::world::ChunkMesh
fluid_mesh(heartstead::world::ChunkDatabase& chunks,
           const heartstead::world::VoxelPalette& palette,
           heartstead::world::ChunkIdentity identity) {
    auto table = heartstead::world::build_block_render_table_snapshot(&palette);
    assert(table);
    auto snapshot =
        heartstead::world::build_chunk_neighborhood_snapshot(chunks, identity, table.value());
    assert(snapshot);
    auto mesh = heartstead::world::GreedyChunkMesher::build_surface_mesh(
        snapshot.value(), table.value());
    assert(mesh);
    return std::move(mesh).value();
}

void test_fluid_mesher_uses_levels_and_flow_uvs() {
    using namespace heartstead::world;
    FluidFixture fixture;
    ChunkDatabase chunks;
    auto& chunk = chunks.get_or_create({0, 0, 0});
    auto state =
        encode_fluid_state({4, false, false, FluidFlowDirection::positive_x});
    assert(state);
    assert(chunk.set({5, 5, 5}, {2, 180, state.value()}));
    const auto mesh = fluid_mesh(chunks, fixture.palette, chunk.identity());
    assert(mesh.face_count == 6);
    assert(mesh.sections.size() == 1);
    assert(mesh.sections.front().render_phase == MeshingRenderPhase::fluid);
    std::vector<ChunkMeshVertex> top;
    std::ranges::copy_if(mesh.vertices, std::back_inserter(top),
                         [](const auto& vertex) { return vertex.normal.y > 0.99F; });
    assert(top.size() == 4);
    assert(std::ranges::all_of(top, [](const auto& vertex) {
        return std::abs(vertex.position.y - 5.5F) < 0.0001F;
    }));
    assert(top[0].u == 0.0F && top[0].v == 1.0F);
    assert(top[1].u == 1.0F && top[1].v == 1.0F);

    auto falling = encode_fluid_state({3, true, false, FluidFlowDirection::none});
    assert(falling);
    assert(chunk.set({5, 5, 5}, {2, 180, falling.value()}));
    const auto falling_mesh = fluid_mesh(chunks, fixture.palette, chunk.identity());
    assert(std::ranges::any_of(falling_mesh.vertices, [](const auto& vertex) {
        return vertex.normal.y > 0.99F && std::abs(vertex.position.y - 6.0F) < 0.0001F;
    }));
}

void test_fluid_mesher_matches_surface_at_chunk_border() {
    using namespace heartstead::world;
    FluidFixture fixture;
    ChunkDatabase chunks;
    auto& left = chunks.get_or_create({0, 0, 0});
    auto& right = chunks.get_or_create({1, 0, 0});
    auto full = encode_fluid_state({8, false, false, FluidFlowDirection::positive_x});
    auto half = encode_fluid_state({4, false, false, FluidFlowDirection::positive_x});
    assert(full && half);
    assert(left.set({31, 5, 5}, {2, 200, full.value()}));
    assert(right.set({0, 5, 5}, {2, 200, half.value()}));
    const auto left_mesh = fluid_mesh(chunks, fixture.palette, left.identity());
    const auto right_mesh = fluid_mesh(chunks, fixture.palette, right.identity());
    assert(left_mesh.face_count == 5);
    assert(right_mesh.face_count == 5);
    assert(std::ranges::none_of(left_mesh.vertices,
                                [](const auto& vertex) { return vertex.normal.x > 0.99F; }));
    assert(std::ranges::none_of(right_mesh.vertices,
                                [](const auto& vertex) { return vertex.normal.x < -0.99F; }));
    assert(std::ranges::any_of(left_mesh.vertices, [](const auto& vertex) {
        return vertex.normal.y > 0.5F && std::abs(vertex.position.x - 32.0F) < 0.0001F &&
               std::abs(vertex.position.y - 5.75F) < 0.0001F;
    }));
    assert(std::ranges::any_of(right_mesh.vertices, [](const auto& vertex) {
        return vertex.normal.y > 0.5F && std::abs(vertex.position.x) < 0.0001F &&
               std::abs(vertex.position.y - 5.75F) < 0.0001F;
    }));
}

[[nodiscard]] std::size_t cell_index(heartstead::world::VoxelCoord coordinate) {
    constexpr auto edge =
        static_cast<std::size_t>(heartstead::world::VoxelChunk::edge_length);
    return static_cast<std::size_t>(coordinate.z) * edge * edge +
           static_cast<std::size_t>(coordinate.y) * edge +
           static_cast<std::size_t>(coordinate.x);
}

[[nodiscard]] heartstead::world::VoxelChunk
dam_chunk(heartstead::world::ChunkCoord coordinate, bool source_chunk) {
    using namespace heartstead::world;
    std::vector<VoxelCell> cells(VoxelChunk::total_cells, VoxelCell::air());
    for (std::uint16_t z = 0; z < VoxelChunk::edge_length; ++z) {
        for (std::uint16_t x = 0; x < VoxelChunk::edge_length; ++x) {
            cells[cell_index({x, 0, z})] = VoxelCell{1};
        }
    }
    if (source_chunk) {
        cells[cell_index({30, 1, 16})] =
            VoxelCell{2, 0, full_fluid_source_state_bits()};
        for (std::uint16_t z = 0; z < VoxelChunk::edge_length; ++z) {
            cells[cell_index({31, 1, z})] = VoxelCell{1};
        }
    }
    VoxelChunk chunk(coordinate);
    assert(chunk.load_generated_cells(std::move(cells)));
    return chunk;
}

[[nodiscard]] heartstead::world::ChunkDatabase dam_world(bool reverse_load_order) {
    using namespace heartstead::world;
    ChunkDatabase chunks;
    if (reverse_load_order) {
        assert(chunks.insert_generated(dam_chunk({1, 0, 0}, false)));
        assert(chunks.insert_generated(dam_chunk({0, 0, 0}, true)));
    } else {
        assert(chunks.insert_generated(dam_chunk({0, 0, 0}, true)));
        assert(chunks.insert_generated(dam_chunk({1, 0, 0}, false)));
    }
    return chunks;
}

struct SettledFluid {
    std::uint64_t next_tick = 0;
    std::size_t steps = 0;
};

[[nodiscard]] SettledFluid settle(heartstead::world::ChunkDatabase& chunks,
                                  heartstead::dirty::DirtyRegionTracker& dirty_regions,
                                  const heartstead::world::FluidBlockTable& table,
                                  std::uint64_t first_tick = 0,
                                  std::size_t maximum_steps = 128) {
    using namespace heartstead::world;
    auto snapshot = build_fluid_simulation_snapshot(chunks);
    auto active = fluid_cells_and_neighbors(snapshot, table);
    std::uint64_t tick = first_tick;
    for (std::size_t step = 0; step < maximum_steps; ++step, ++tick) {
        snapshot = build_fluid_simulation_snapshot(chunks);
        auto result = simulate_fluid_step(snapshot, table, active, 32'768, tick);
        assert(result);
        auto applied = apply_fluid_step(chunks, dirty_regions, result.value());
        assert(applied);
        active = std::move(result.value().next_active);
        if (active.empty()) {
            return {tick + 1, step + 1};
        }
    }
    assert(false && "fluid fixture failed to settle");
    return {};
}

void test_dam_break_crosses_chunk_border_and_is_load_order_independent() {
    using namespace heartstead::world;
    FluidFixture fixture;
    const auto table = build_fluid_block_table(fixture.palette);

    auto forward = dam_world(false);
    auto reverse = dam_world(true);
    heartstead::dirty::DirtyRegionTracker forward_dirty;
    heartstead::dirty::DirtyRegionTracker reverse_dirty;
    auto forward_tick = settle(forward, forward_dirty, table);
    auto reverse_tick = settle(reverse, reverse_dirty, table);
    assert(forward_tick.steps == reverse_tick.steps);

    assert(forward.set({0, 0, 0}, {31, 1, 16}, VoxelCell::air()));
    assert(reverse.set({0, 0, 0}, {31, 1, 16}, VoxelCell::air()));
    forward_tick = settle(forward, forward_dirty, table, forward_tick.next_tick);
    reverse_tick = settle(reverse, reverse_dirty, table, reverse_tick.next_tick);
    assert(forward_tick.steps == reverse_tick.steps);

    for (const auto coordinate : {ChunkCoord{0, 0, 0}, ChunkCoord{1, 0, 0}}) {
        const auto* forward_chunk = forward.find(coordinate);
        const auto* reverse_chunk = reverse.find(coordinate);
        assert(forward_chunk != nullptr && reverse_chunk != nullptr);
        assert(std::ranges::equal(forward_chunk->cells(), reverse_chunk->cells()));
    }
    const auto across_border = forward.get({1, 0, 0}, {0, 1, 16}).value();
    auto state = decode_fluid_state(across_border.state_bits);
    assert(across_border.type == 2 && state);
    assert(state.value().amount == 6);
    assert(state.value().flow == FluidFlowDirection::positive_x);
}

void test_source_removal_cleans_up_and_budget_defers_work() {
    using namespace heartstead::world;
    FluidFixture fixture;
    const auto table = build_fluid_block_table(fixture.palette);
    auto chunks = dam_world(false);
    heartstead::dirty::DirtyRegionTracker dirty_regions;
    auto settled = settle(chunks, dirty_regions, table);
    assert(chunks.set({0, 0, 0}, {30, 1, 16}, VoxelCell::air()));

    auto snapshot = build_fluid_simulation_snapshot(chunks);
    auto active = fluid_cells_and_neighbors(snapshot, table);
    auto budgeted = simulate_fluid_step(snapshot, table, active, 1, settled.next_tick);
    assert(budgeted);
    assert(budgeted.value().stats.processed_active_cell_count == 1);
    assert(budgeted.value().stats.budget_exhausted);
    assert(budgeted.value().stats.deferred_active_cell_count + 1 == active.size());

    (void)settle(chunks, dirty_regions, table, settled.next_tick);
    for (const auto* chunk : chunks.records()) {
        assert(std::ranges::none_of(chunk->cells(),
                                    [](const VoxelCell& cell) { return cell.type == 2; }));
    }
}

void test_stale_fluid_result_fails_before_application() {
    using namespace heartstead::world;
    FluidFixture fixture;
    const auto table = build_fluid_block_table(fixture.palette);
    auto chunks = dam_world(false);
    auto snapshot = build_fluid_simulation_snapshot(chunks);
    auto active = fluid_cells_and_neighbors(snapshot, table);
    auto result = simulate_fluid_step(snapshot, table, active, 32'768, 0);
    assert(result && !result.value().changes.empty());
    assert(chunks.set({0, 0, 0}, {1, 1, 1}, VoxelCell{1}));
    heartstead::dirty::DirtyRegionTracker dirty_regions;
    auto applied = apply_fluid_step(chunks, dirty_regions, result.value());
    assert(!applied);
    assert(applied.error().code == "fluid_simulation.stale_result");
}

void run_system_ticks(heartstead::world::ChunkFluidSystem& system,
                      heartstead::world::ChunkDatabase& chunks,
                      heartstead::dirty::DirtyRegionTracker& dirty_regions,
                      const heartstead::world::VoxelPalette& palette,
                      std::uint64_t first_tick, std::uint64_t end_tick) {
    for (auto tick = first_tick; tick < end_tick; ++tick) {
        assert(system.update(chunks, dirty_regions, palette, tick));
    }
}

void test_chunk_fluid_system_consumes_dirty_work_and_reports_backlog() {
    using namespace heartstead::world;
    FluidFixture fixture;
    auto chunks = dam_world(false);
    heartstead::dirty::DirtyRegionTracker dirty_regions;
    ChunkFluidSystemConfig config;
    config.simulation_tick_interval = 1;
    config.maximum_active_cells_per_step = 1;
    auto system = ChunkFluidSystem::create(fixture.palette, config);
    assert(system);
    assert(system.value()->update(chunks, dirty_regions, fixture.palette, 0));
    assert(system.value()->stats().topology_rebuilds == 1);
    assert(system.value()->stats().budget_exhausted);
    assert(system.value()->stats().processed_cells_this_update == 1);
    assert(system.value()->stats().active_cell_count > 0);

    assert(chunks.set({0, 0, 0}, {31, 1, 16}, VoxelCell::air(), dirty_regions,
                      fixture.palette));
    assert(dirty_regions.count(heartstead::dirty::DirtyRegionKind::water_network) == 1);
    assert(system.value()->update(chunks, dirty_regions, fixture.palette, 1));
    assert(system.value()->stats().dirty_regions_consumed == 1);
    assert(dirty_regions.count(heartstead::dirty::DirtyRegionKind::water_network) == 0);
}

void test_save_reload_mid_flow_matches_uninterrupted_simulation() {
    using namespace heartstead::world;
    FluidFixture fixture;
    ChunkFluidSystemConfig config;
    config.simulation_tick_interval = 1;

    auto continuous = dam_world(false);
    heartstead::dirty::DirtyRegionTracker continuous_dirty;
    auto continuous_system = ChunkFluidSystem::create(fixture.palette, config);
    assert(continuous_system);
    run_system_ticks(*continuous_system.value(), continuous, continuous_dirty, fixture.palette,
                     0, 12);
    assert(continuous.set({0, 0, 0}, {31, 1, 16}, VoxelCell::air(), continuous_dirty,
                          fixture.palette));
    run_system_ticks(*continuous_system.value(), continuous, continuous_dirty, fixture.palette,
                     12, 15);

    const auto saved_edits = continuous.edit_log();
    std::map<ChunkCoord, std::vector<const VoxelEditRecord*>> edits_by_chunk;
    for (const auto& edit : saved_edits) {
        edits_by_chunk[edit.chunk_coord].push_back(&edit);
    }
    std::vector<VoxelEditRecord> restored_edits;
    for (const auto& [coordinate, edits] : edits_by_chunk) {
        const auto encoded = ChunkEditDeltaTextCodec::encode(coordinate, edits);
        auto decoded = ChunkEditDeltaTextCodec::decode(coordinate, encoded);
        assert(decoded);
        restored_edits.insert(restored_edits.end(), decoded.value().begin(),
                              decoded.value().end());
    }

    auto reloaded = dam_world(false);
    heartstead::dirty::DirtyRegionTracker reloaded_dirty;
    assert(reloaded.apply_saved_edits(restored_edits, reloaded_dirty));
    auto reloaded_system = ChunkFluidSystem::create(fixture.palette, config);
    assert(reloaded_system);

    run_system_ticks(*continuous_system.value(), continuous, continuous_dirty, fixture.palette,
                     15, 32);
    run_system_ticks(*reloaded_system.value(), reloaded, reloaded_dirty, fixture.palette, 15, 32);
    for (const auto coordinate : {ChunkCoord{0, 0, 0}, ChunkCoord{1, 0, 0}}) {
        const auto* continuous_chunk = continuous.find(coordinate);
        const auto* reloaded_chunk = reloaded.find(coordinate);
        assert(continuous_chunk != nullptr && reloaded_chunk != nullptr);
        assert(std::ranges::equal(continuous_chunk->cells(), reloaded_chunk->cells()));
    }
}

} // namespace

int main() {
    test_fluid_state_round_trip_and_rejects_reserved_state();
    test_palette_creates_full_finite_fluid_cells();
    test_worldgen_seeds_ocean_sources_below_sea_level();
    test_fluid_mesher_uses_levels_and_flow_uvs();
    test_fluid_mesher_matches_surface_at_chunk_border();
    test_dam_break_crosses_chunk_border_and_is_load_order_independent();
    test_source_removal_cleans_up_and_budget_defers_work();
    test_stale_fluid_result_fails_before_application();
    test_chunk_fluid_system_consumes_dirty_work_and_reports_backlog();
    test_save_reload_mid_flow_matches_uninterrupted_simulation();
    return 0;
}
