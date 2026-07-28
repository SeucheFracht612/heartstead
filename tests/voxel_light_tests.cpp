#include "engine/dirty/dirty_region.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/lighting/voxel_light.hpp"
#include "engine/world/meshing/chunk_mesh_snapshot.hpp"
#include "engine/world/meshing/greedy_chunk_mesher.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
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
    assert(result.add(voxel_definition(3, "test:voxels/filter", 32)));
    return result;
}

void insert_filled_chunk(world::ChunkDatabase& chunks, world::ChunkCoord coordinate,
                         world::VoxelCell cell) {
    std::vector<world::VoxelCell> cells(world::VoxelChunk::total_cells, cell);
    world::VoxelChunk chunk(coordinate);
    assert(chunk.load_generated_cells(std::move(cells)));
    assert(chunks.insert_generated(std::move(chunk)));
}

[[nodiscard]] std::uint8_t patch_light(const world::VoxelLightSolveResult& result,
                                       world::ChunkCoord chunk, world::VoxelCoord local) {
    const auto found = std::ranges::find_if(
        result.patches, [chunk](const auto& patch) { return patch.identity.coordinate == chunk; });
    assert(found != result.patches.end());
    constexpr auto edge = static_cast<std::size_t>(world::VoxelChunk::edge_length);
    const auto index = static_cast<std::size_t>(local.z) * edge * edge +
                       static_cast<std::size_t>(local.y) * edge + local.x;
    return found->lights[index];
}

[[nodiscard]] world::VoxelLightSolveResult solve(const world::ChunkDatabase& chunks,
                                                 const world::VoxelPalette& palette) {
    const auto snapshot = world::build_voxel_light_snapshot(chunks);
    auto solved = world::solve_voxel_light(snapshot, world::build_voxel_light_block_table(palette));
    assert(solved);
    return std::move(solved).value();
}

void test_sunlight_columns_and_absorption() {
    const auto palette = light_palette();
    world::ChunkDatabase chunks;
    insert_filled_chunk(chunks, {0, 0, 0}, world::VoxelCell{0, 19});
    for (std::uint16_t z = 0; z < world::VoxelChunk::edge_length; ++z) {
        for (std::uint16_t x = 0; x < world::VoxelChunk::edge_length; ++x) {
            assert(chunks.set({0, 0, 0}, {x, 0, z}, world::VoxelCell{1, 91}));
            assert(chunks.set({0, 0, 0}, {x, 20, z}, world::VoxelCell{3, 0}));
        }
    }

    const auto result = solve(chunks, palette);
    assert(result.stats.chunk_count == 1);
    assert(result.stats.sunlight_seed_count > 0);
    assert(patch_light(result, {0, 0, 0}, {4, 31, 4}) == 255);
    assert(patch_light(result, {0, 0, 0}, {4, 20, 4}) == 223);
    assert(patch_light(result, {0, 0, 0}, {4, 19, 4}) == 223);
    assert(patch_light(result, {0, 0, 0}, {4, 0, 4}) == 0);
}

void test_sunlight_floods_a_tunnel_across_a_chunk_border() {
    const auto palette = light_palette();
    world::ChunkDatabase chunks;
    insert_filled_chunk(chunks, {-1, 0, 0}, world::VoxelCell::air());
    insert_filled_chunk(chunks, {0, 0, 0}, world::VoxelCell{1, 0});
    for (std::uint16_t x = 0; x <= 5; ++x) {
        assert(chunks.set({0, 0, 0}, {x, 5, 5}, world::VoxelCell::air()));
    }

    const auto result = solve(chunks, palette);
    assert(patch_light(result, {-1, 0, 0}, {31, 5, 5}) == 255);
    assert(patch_light(result, {0, 0, 0}, {0, 5, 5}) == 239);
    assert(patch_light(result, {0, 0, 0}, {1, 5, 5}) == 223);
    assert(patch_light(result, {0, 0, 0}, {5, 5, 5}) == 159);
    assert(patch_light(result, {0, 0, 0}, {6, 5, 5}) == 0);
}

void build_emitter_scene(world::ChunkDatabase& chunks, bool reverse_insert_order) {
    if (reverse_insert_order) {
        insert_filled_chunk(chunks, {1, 0, 0}, world::VoxelCell{1, 0});
        insert_filled_chunk(chunks, {0, 0, 0}, world::VoxelCell{1, 0});
    } else {
        insert_filled_chunk(chunks, {0, 0, 0}, world::VoxelCell{1, 0});
        insert_filled_chunk(chunks, {1, 0, 0}, world::VoxelCell{1, 0});
    }
    assert(chunks.set({0, 0, 0}, {31, 5, 5}, world::VoxelCell{2, 0}));
    for (std::uint16_t x = 0; x <= 4; ++x) {
        assert(chunks.set({1, 0, 0}, {x, 5, 5}, world::VoxelCell::air()));
    }
}

void test_block_light_place_remove_and_load_order() {
    const auto palette = light_palette();
    world::ChunkDatabase first;
    world::ChunkDatabase second;
    build_emitter_scene(first, false);
    build_emitter_scene(second, true);

    const auto first_result = solve(first, palette);
    const auto second_result = solve(second, palette);
    assert(first_result.patches.size() == second_result.patches.size());
    for (std::size_t index = 0; index < first_result.patches.size(); ++index) {
        assert(first_result.patches[index].identity.coordinate ==
               second_result.patches[index].identity.coordinate);
        assert(first_result.patches[index].lights == second_result.patches[index].lights);
    }
    assert(first_result.stats.block_light_seed_count == 1);
    assert(patch_light(first_result, {0, 0, 0}, {31, 5, 5}) == 224);
    assert(patch_light(first_result, {1, 0, 0}, {0, 5, 5}) == 208);
    assert(patch_light(first_result, {1, 0, 0}, {4, 5, 5}) == 144);

    assert(first.set({0, 0, 0}, {31, 5, 5}, world::VoxelCell{1, 224}));
    const auto removed = solve(first, palette);
    assert(removed.stats.block_light_seed_count == 0);
    assert(patch_light(removed, {1, 0, 0}, {0, 5, 5}) == 0);
    assert(patch_light(removed, {1, 0, 0}, {4, 5, 5}) == 0);
}

void test_external_light_sources_share_block_light_propagation() {
    const auto palette = light_palette();
    world::ChunkDatabase chunks;
    insert_filled_chunk(chunks, {0, 0, 0}, world::VoxelCell{1, 0});
    insert_filled_chunk(chunks, {1, 0, 0}, world::VoxelCell{1, 0});
    assert(chunks.set({0, 0, 0}, {31, 5, 5}, world::VoxelCell::air()));
    for (std::uint16_t x = 0; x <= 3; ++x) {
        assert(chunks.set({1, 0, 0}, {x, 5, 5}, world::VoxelCell::air()));
    }
    auto snapshot = world::build_voxel_light_snapshot(chunks);
    snapshot.sources.push_back({{31, 5, 5}, 204});
    auto solved = world::solve_voxel_light(snapshot, world::build_voxel_light_block_table(palette));
    assert(solved);
    assert(solved.value().stats.block_light_seed_count == 1);
    assert(patch_light(solved.value(), {0, 0, 0}, {31, 5, 5}) == 204);
    assert(patch_light(solved.value(), {1, 0, 0}, {0, 5, 5}) == 188);
    assert(patch_light(solved.value(), {1, 0, 0}, {3, 5, 5}) == 140);
}

void test_derived_apply_is_revision_checked_and_not_a_save_edit() {
    const auto palette = light_palette();
    world::ChunkDatabase chunks;
    insert_filled_chunk(chunks, {0, 0, 0}, world::VoxelCell::air());
    chunks.clear_edit_log();
    chunks.clear_all_dirty();
    auto* chunk = chunks.find({0, 0, 0});
    assert(chunk != nullptr);
    chunk->mark_dirty(world::ChunkDirtyFlag::lighting);
    const auto revision = chunk->content_revision();
    const auto result = solve(chunks, palette);

    dirty::DirtyRegionTracker dirty_regions;
    auto applied = world::apply_voxel_light(chunks, dirty_regions, result);
    assert(applied);
    assert(applied.value().patch_count == 1);
    assert(applied.value().changed_chunk_count == 1);
    assert(applied.value().changed_cell_count == world::VoxelChunk::total_cells);
    assert(chunks.edit_log().empty());
    assert(chunk->content_revision() == revision + 1);
    assert(chunk->dirty().contains(world::ChunkDirtyFlag::mesh));
    assert(chunk->dirty().contains(world::ChunkDirtyFlag::replication));
    assert(!chunk->dirty().contains(world::ChunkDirtyFlag::save));
    assert(!chunk->dirty().contains(world::ChunkDirtyFlag::collision));
    assert(!chunk->dirty().contains(world::ChunkDirtyFlag::lighting));
    assert(dirty_regions.count(dirty::DirtyRegionKind::chunk_mesh) == 1);

    const auto stale_snapshot = world::build_voxel_light_snapshot(chunks);
    auto stale =
        world::solve_voxel_light(stale_snapshot, world::build_voxel_light_block_table(palette));
    assert(stale);
    assert(chunks.set({0, 0, 0}, {1, 1, 1}, world::VoxelCell{1, 0}));
    auto rejected = world::apply_voxel_light(chunks, dirty_regions, stale.value());
    assert(!rejected);
    assert(rejected.error().code == "voxel_light.stale_result");
}

void test_mesher_lights_solid_faces_from_their_exposed_neighbor() {
    const auto palette = light_palette();
    world::ChunkDatabase chunks;
    insert_filled_chunk(chunks, {0, 0, 0}, world::VoxelCell::air());
    assert(chunks.set({0, 0, 0}, {4, 4, 4}, world::VoxelCell{1, 0}));
    const auto result = solve(chunks, palette);
    dirty::DirtyRegionTracker dirty_regions;
    assert(world::apply_voxel_light(chunks, dirty_regions, result));

    auto render_table = world::build_block_render_table_snapshot(&palette);
    assert(render_table);
    const auto* chunk = chunks.find({0, 0, 0});
    assert(chunk != nullptr);
    auto neighborhood =
        world::build_chunk_neighborhood_snapshot(chunks, chunk->identity(), render_table.value());
    assert(neighborhood);
    auto mesh =
        world::GreedyChunkMesher::build_surface_mesh(neighborhood.value(), render_table.value());
    assert(mesh);
    assert(mesh.value().face_count == 6);
    assert(std::ranges::all_of(mesh.value().vertices,
                               [](const auto& vertex) { return vertex.light > 0; }));
    assert(std::ranges::all_of(mesh.value().vertices, [](const auto& vertex) {
        return vertex.normal.y < 0.9F || vertex.light == 255;
    }));
    assert(std::ranges::all_of(mesh.value().vertices, [](const auto& vertex) {
        return vertex.normal.y > -0.9F || vertex.light == 239;
    }));
}

void test_invalid_inputs_fail_closed() {
    world::VoxelLightBlockTable invalid_table;
    assert(!invalid_table.validate());
    world::VoxelLightSnapshot invalid_snapshot;
    invalid_snapshot.chunks.push_back({});
    assert(!invalid_snapshot.validate());
    world::VoxelLightSnapshot invalid_sources;
    invalid_sources.sources = {{{0, 0, 0}, 10}, {{0, 0, 0}, 20}};
    assert(!invalid_sources.validate());

    world::VoxelChunk chunk({0, 0, 0});
    const std::vector<std::uint8_t> short_light(1, 255);
    assert(!chunk.apply_derived_light(short_light));
}

} // namespace

int main() {
    test_sunlight_columns_and_absorption();
    test_sunlight_floods_a_tunnel_across_a_chunk_border();
    test_block_light_place_remove_and_load_order();
    test_external_light_sources_share_block_light_propagation();
    test_derived_apply_is_revision_checked_and_not_a_save_edit();
    test_mesher_lights_solid_faces_from_their_exposed_neighbor();
    test_invalid_inputs_fail_closed();
    return 0;
}
