#include "engine/dirty/dirty_region.hpp"
#include "engine/world/blocks/block_model.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/chunks/chunk_edit_delta_codec.hpp"
#include "engine/world/fluids/fluid_state.hpp"
#include "engine/world/meshing/chunk_mesh_snapshot.hpp"
#include "engine/world/meshing/chunk_mesher.hpp"
#include "engine/world/meshing/greedy_chunk_mesher.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] heartstead::core::PrototypeId id(std::string value) {
    auto parsed = heartstead::core::PrototypeId::parse(value);
    assert(parsed);
    return std::move(*parsed);
}

[[nodiscard]] heartstead::world::VoxelPalette make_palette() {
    using namespace heartstead::world;

    VoxelPalette palette;

    BlockModelDefinition cube;
    cube.prototype_id = id("test:block_models/cube");
    cube.kind = BlockModelKind::cube;
    cube.boxes.push_back({{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}});
    cube.render_bounds = cube.boxes.front().bounds;
    assert(palette.add_block_model(cube));

    BlockModelDefinition leaf;
    leaf.prototype_id = id("test:block_models/leaf_cluster");
    leaf.kind = BlockModelKind::custom_voxel;
    leaf.boxes.push_back({{{-0.35F, -0.20F, -0.35F}, {1.35F, 1.20F, 1.35F}}});
    leaf.render_bounds = leaf.boxes.front().bounds;
    leaf.neighbor_dependency_radius = 2;
    leaf.mesh_invalidation_radius = 2;
    assert(palette.add_block_model(leaf));

    BlockModelDefinition cross;
    cross.prototype_id = id("test:block_models/cross");
    cross.kind = BlockModelKind::cross_plane;
    cross.render_bounds = {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
    assert(palette.add_block_model(cross));

    VoxelDefinition stone;
    stone.type = 1;
    stone.prototype_id = id("test:voxels/stone");
    stone.display_name = "Stone";
    stone.terrain_material = "stone";
    stone.mining_tool = "pickaxe";
    stone.block_model_id = cube.prototype_id;
    assert(palette.add(stone));

    VoxelDefinition leaves;
    leaves.type = 2;
    leaves.prototype_id = id("test:voxels/leaves");
    leaves.display_name = "Leaves";
    leaves.terrain_material = "foliage";
    leaves.mining_tool = "shears";
    leaves.block_model_id = leaf.prototype_id;
    leaves.logical_occupancy = BlockLogicalOccupancy::decorative;
    leaves.occlusion = BlockOcclusionBehavior::none;
    leaves.occlusion_bounds.clear();
    leaves.collision_bounds.clear();
    assert(palette.add(leaves));

    VoxelDefinition grass;
    grass.type = 3;
    grass.prototype_id = id("test:voxels/grass");
    grass.display_name = "Grass";
    grass.terrain_material = "foliage";
    grass.mining_tool = "shears";
    grass.block_model_id = cross.prototype_id;
    grass.logical_occupancy = BlockLogicalOccupancy::decorative;
    grass.occlusion = BlockOcclusionBehavior::none;
    grass.occlusion_bounds.clear();
    grass.collision_bounds.clear();
    assert(palette.add(grass));

    VoxelDefinition water;
    water.type = 4;
    water.prototype_id = id("test:voxels/water");
    water.display_name = "Water";
    water.terrain_material = "water";
    water.mining_tool = "bucket";
    water.block_model_id = cube.prototype_id;
    water.logical_occupancy = BlockLogicalOccupancy::fluid;
    water.occlusion = BlockOcclusionBehavior::none;
    water.occlusion_bounds.clear();
    water.collision_bounds.clear();
    assert(palette.add(water));

    return palette;
}

void test_mesh_sections_separate_render_phases() {
    auto palette = make_palette();
    heartstead::world::ChunkDatabase chunks;
    auto& chunk = chunks.get_or_create({7, -2, 5});
    assert(chunk.set({0, 0, 0}, {1, 255}));
    assert(chunk.set({2, 0, 0}, {3, 255}));
    assert(chunk.set({4, 0, 0}, {4, 255, heartstead::world::full_fluid_state_bits()}));
    auto table = heartstead::world::build_block_render_table_snapshot(&palette);
    assert(table);
    auto snapshot = heartstead::world::build_chunk_neighborhood_snapshot(chunks, chunk.identity(),
                                                                         table.value());
    assert(snapshot);
    auto mesh =
        heartstead::world::GreedyChunkMesher::build_surface_mesh(snapshot.value(), table.value());
    assert(mesh);
    assert(mesh.value().sections.size() == 3);
    assert(mesh.value().sections[0].material_index == 1);
    assert(mesh.value().sections[0].render_phase == heartstead::world::MeshingRenderPhase::opaque);
    assert(mesh.value().sections[1].material_index == 3);
    assert(mesh.value().sections[1].render_phase ==
           heartstead::world::MeshingRenderPhase::alpha_tested);
    assert(mesh.value().sections[2].material_index == 4);
    assert(mesh.value().sections[2].render_phase == heartstead::world::MeshingRenderPhase::fluid);
    std::uint32_t first_index = 0;
    for (const auto& section : mesh.value().sections) {
        assert(section.first_index == first_index);
        first_index += section.index_count;
    }
    assert(first_index == mesh.value().indices.size());
}

[[nodiscard]] bool close(float left, float right) {
    return std::abs(left - right) < 0.0001F;
}

void test_cross_chunk_occlusion_uses_neighbor_halo() {
    auto palette = make_palette();
    heartstead::world::ChunkDatabase chunks;
    auto& center = chunks.get_or_create({0, 0, 0});
    auto& positive_x = chunks.get_or_create({1, 0, 0});
    assert(center.set({31, 4, 5}, {1, 0}));
    assert(positive_x.set({0, 4, 5}, {1, 0}));

    auto mesh = heartstead::world::ChunkMesher::build_surface_mesh({center, &palette, &chunks, 1});
    assert(mesh);
    assert(mesh.value().required_halo_radius == 1);
    assert(mesh.value().face_count == 5);
}

void test_out_of_cell_geometry_requires_declared_halo() {
    auto palette = make_palette();
    heartstead::world::ChunkDatabase chunks;
    auto& center = chunks.get_or_create({-4, 9, 2});
    assert(center.set({0, 0, 0}, {2, 7, 41, 99}));

    auto insufficient =
        heartstead::world::ChunkMesher::build_surface_mesh({center, &palette, &chunks, 1});
    assert(!insufficient);
    assert(insufficient.error().code == "chunk_mesh.insufficient_halo");

    auto mesh = heartstead::world::ChunkMesher::build_surface_mesh({center, &palette, &chunks, 2});
    assert(mesh);
    assert(mesh.value().required_halo_radius == 2);
    assert(close(mesh.value().local_bounds.min.x, -0.35F));
    assert(close(mesh.value().local_bounds.min.y, -0.20F));
    assert(close(mesh.value().local_bounds.max.x, 1.35F));
    assert(close(mesh.value().local_bounds.max.y, 1.20F));
    assert(std::ranges::all_of(mesh.value().vertices, [](const auto& vertex) {
        return vertex.voxel_type == 2 && vertex.state_bits == 41;
    }));
}

void test_rich_invalidation_reaches_non_boundary_neighbor() {
    auto palette = make_palette();
    heartstead::world::ChunkDatabase chunks;
    auto& center = chunks.get_or_create({20, -30, 40});
    auto& neighbor = chunks.get_or_create({21, -30, 40});
    center.clear_all_dirty();
    neighbor.clear_all_dirty();

    heartstead::dirty::DirtyRegionTracker dirty;
    assert(chunks.set({20, -30, 40}, {30, 8, 8}, {2, 0}, dirty, palette));
    assert(neighbor.dirty().contains(heartstead::world::ChunkDirtyFlag::mesh));
    assert(!neighbor.dirty().contains(heartstead::world::ChunkDirtyFlag::collision));
    assert(std::ranges::any_of(dirty.regions(), [](const auto& region) {
        return region.kind == heartstead::dirty::DirtyRegionKind::chunk_mesh &&
               region.bounds.max.x >= 21;
    }));
}

void test_chunk_delta_v2_preserves_state_and_sparse_metadata() {
    using namespace heartstead::world;
    const ChunkCoord coord{-(std::int64_t{1} << 48), std::int64_t{1} << 47, -9};
    const VoxelEditRecord edit{coord, {1, 2, 3}, {1, 4, 5, 6}, {2, 7, 8, 9}};
    const std::vector<const VoxelEditRecord*> edits{&edit};
    const auto encoded = ChunkEditDeltaTextCodec::encode(coord, edits);
    assert(encoded.starts_with("heartstead.chunk_edit_delta.v2\n"));
    auto decoded = ChunkEditDeltaTextCodec::decode(coord, encoded);
    assert(decoded);
    assert(decoded.value().size() == 1);
    assert(decoded.value().front().previous == edit.previous);
    assert(decoded.value().front().next == edit.next);

    const std::string legacy =
        "heartstead.chunk_edit_delta.v1\ncoord=-281474976710656|140737488355328|-9\n"
        "edit=1|2|3|1|4|2|7\nend\n";
    auto legacy_decoded = ChunkEditDeltaTextCodec::decode(coord, legacy);
    assert(legacy_decoded);
    assert(legacy_decoded.value().front().previous.state_bits == 0);
    assert(legacy_decoded.value().front().previous.metadata_handle == 0);
}

void test_persisted_palette_keeps_ids_and_recovers_missing_blocks() {
    heartstead::modding::GenericPrototype stone;
    stone.kind = std::string(heartstead::modding::PrototypeKinds::voxel);
    stone.id = id("test:voxels/stone");
    stone.display_name = "Stone";
    stone.fields = {{"terrain_material", "stone"}, {"mining_tool", "pickaxe"}};

    heartstead::modding::GenericPrototype new_block = stone;
    new_block.id = id("test:voxels/new_block");
    new_block.display_name = "New Block";

    heartstead::modding::PrototypeRegistry registry;
    assert(!registry.build({stone, new_block}).has_errors());

    heartstead::world::VoxelPaletteManifest persisted;
    persisted.entries = {{5, id("removed:voxels/old_block")}, {9, stone.id}};
    auto palette = heartstead::world::voxel_palette_from_prototypes(registry, persisted);
    assert(palette);
    const auto* missing = palette.value().find_by_type(5);
    assert(missing != nullptr);
    assert(missing->prototype_id == id("removed:voxels/old_block"));
    assert(missing->missing_prototype);
    assert(palette.value().type_for(stone.id).value() == 9);
    assert(palette.value().type_for(new_block.id).value() == 10);
    assert(palette.value().manifest().entries.size() == 3);
}

} // namespace

int main() {
    test_mesh_sections_separate_render_phases();
    test_cross_chunk_occlusion_uses_neighbor_halo();
    test_out_of_cell_geometry_requires_declared_halo();
    test_rich_invalidation_reaches_non_boundary_neighbor();
    test_chunk_delta_v2_preserves_state_and_sparse_metadata();
    test_persisted_palette_keeps_ids_and_recovers_missing_blocks();
    return 0;
}
