#include "engine/physics/chunk_collision_system.hpp"
#include "engine/world/collision/chunk_collision.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <utility>

namespace {

[[nodiscard]] heartstead::world::VoxelDefinition definition(std::uint16_t type, const char* id) {
    heartstead::world::VoxelDefinition result;
    result.type = type;
    result.prototype_id = heartstead::core::PrototypeId::parse(id).value();
    result.display_name = id;
    result.terrain_material = "stone";
    result.mining_tool = "pick";
    return result;
}

[[nodiscard]] heartstead::world::VoxelPalette collision_palette() {
    auto solid = definition(1, "test:voxels/solid");

    auto slab = definition(2, "test:voxels/slab");
    slab.logical_occupancy = heartstead::world::BlockLogicalOccupancy::partial;
    slab.collision_bounds = {{{0.0F, 0.0F, 0.0F}, {1.0F, 0.5F, 1.0F}}};
    slab.selection_bounds = slab.collision_bounds;
    slab.occlusion_bounds = slab.collision_bounds;
    slab.occlusion = heartstead::world::BlockOcclusionBehavior::model;

    auto water = definition(3, "test:voxels/water");
    water.logical_occupancy = heartstead::world::BlockLogicalOccupancy::fluid;
    water.collision_bounds.clear();
    water.occlusion_bounds.clear();
    water.occlusion = heartstead::world::BlockOcclusionBehavior::none;

    heartstead::world::VoxelPalette palette;
    assert(palette.add(std::move(solid)));
    assert(palette.add(std::move(slab)));
    assert(palette.add(std::move(water)));
    return palette;
}

void test_full_chunk_greedily_merges_to_one_box() {
    using namespace heartstead;
    auto palette = collision_palette();
    auto table = world::build_chunk_collision_table_snapshot(&palette);
    assert(table);

    world::ChunkDatabase chunks;
    auto& chunk = chunks.get_or_create({2, -1, 3});
    chunk.fill({1, 0});
    auto snapshot = world::build_chunk_collision_snapshot(chunks, chunk.identity(), table.value());
    assert(snapshot);
    auto collision = world::cook_chunk_collision(snapshot.value(), table.value());
    assert(collision);
    assert(collision.value().boxes.size() == 1);
    assert((collision.value().boxes.front().local_position == physics::Vec3{16.0F, 16.0F, 16.0F}));
    assert((collision.value().boxes.front().half_extents == physics::Vec3{16.0F, 16.0F, 16.0F}));
    assert(collision.value().stats.source_colliding_cells == world::VoxelChunk::total_cells);
    assert(collision.value().stats.output_boxes == 1);
}

void test_partial_and_non_colliding_prototypes_are_preserved() {
    using namespace heartstead;
    auto palette = collision_palette();
    auto table = world::build_chunk_collision_table_snapshot(&palette);
    assert(table);

    world::ChunkDatabase chunks;
    auto& chunk = chunks.get_or_create({0, 0, 0});
    assert(chunk.set({1, 2, 3}, {2, 0}));
    assert(chunk.set({2, 2, 3}, {3, 0}));
    auto snapshot = world::build_chunk_collision_snapshot(chunks, chunk.identity(), table.value());
    assert(snapshot);
    auto collision = world::cook_chunk_collision(snapshot.value(), table.value());
    assert(collision);
    assert(collision.value().boxes.size() == 1);
    const auto& slab = collision.value().boxes.front();
    assert((slab.local_position == physics::Vec3{1.5F, 2.25F, 3.5F}));
    assert((slab.half_extents == physics::Vec3{0.5F, 0.25F, 0.5F}));
    assert(collision.value().stats.source_colliding_cells == 1);
    assert(collision.value().stats.source_collision_boxes == 1);
}

void test_snapshot_rejects_stale_identity_and_unknown_types() {
    using namespace heartstead;
    auto palette = collision_palette();
    auto table = world::build_chunk_collision_table_snapshot(&palette);
    assert(table);

    world::ChunkDatabase chunks;
    auto& chunk = chunks.get_or_create({0, 0, 0});
    const auto old_identity = chunk.identity();
    assert(chunks.erase(chunk.coord()));
    auto& replacement = chunks.get_or_create({0, 0, 0});
    assert(replacement.identity() != old_identity);
    auto stale = world::build_chunk_collision_snapshot(chunks, old_identity, table.value());
    assert(!stale);
    assert(stale.error().code == "chunk_collision.stale_snapshot_identity");

    assert(replacement.set({0, 0, 0}, {99, 0}));
    auto snapshot =
        world::build_chunk_collision_snapshot(chunks, replacement.identity(), table.value());
    assert(snapshot);
    auto unknown = world::cook_chunk_collision(snapshot.value(), table.value());
    assert(!unknown);
    assert(unknown.error().code == "chunk_collision.unknown_voxel_type");
}

[[nodiscard]] bool wait_for_collision_revision(
    heartstead::physics::ChunkCollisionSystem& collision_system,
    heartstead::world::ChunkDatabase& chunks, heartstead::dirty::DirtyRegionTracker& dirty_regions,
    const heartstead::world::VoxelPalette& palette, heartstead::world::ChunkCoord coordinate,
    std::uint64_t expected_revision) {
    for (std::uint32_t attempt = 0; attempt < 500; ++attempt) {
        assert(collision_system.update(chunks, dirty_regions, palette));
        const auto* record = collision_system.find(coordinate);
        if (record != nullptr && record->content_revision == expected_revision) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void test_system_cooks_rebuilds_and_removes_terrain(heartstead::physics::PhysicsBackend backend) {
    using namespace heartstead;
    if (!physics::physics_backend_info(backend).available) {
        return;
    }

    auto physics_world =
        physics::create_physics_world(physics::PhysicsWorldDesc{.backend = backend});
    assert(physics_world);
    auto palette = collision_palette();
    world::ChunkDatabase chunks;
    dirty::DirtyRegionTracker dirty_regions;
    constexpr world::ChunkCoord coordinate{0, 0, 0};
    auto& chunk = chunks.get_or_create(coordinate);
    for (std::uint16_t z = 0; z < world::VoxelChunk::edge_length; ++z) {
        for (std::uint16_t x = 0; x < world::VoxelChunk::edge_length; ++x) {
            assert(chunk.set({x, 0, z}, {1, 0}));
        }
    }
    assert(dirty_regions.mark_single(dirty::DirtyRegionKind::chunk_collision,
                                     {coordinate.x, coordinate.y, coordinate.z}, "test"));
    assert(dirty_regions.mark_single(dirty::DirtyRegionKind::chunk_mesh,
                                     {coordinate.x, coordinate.y, coordinate.z}, "test"));

    auto collision_system = physics::ChunkCollisionSystem::create(*physics_world.value(), palette);
    assert(collision_system);
    const auto initial_revision = chunk.content_revision();
    assert(wait_for_collision_revision(*collision_system.value(), chunks, dirty_regions, palette,
                                       coordinate, initial_revision));
    const auto* initial_record = collision_system.value()->find(coordinate);
    assert(initial_record != nullptr);
    assert(initial_record->box_count == 1);
    assert(physics_world.value()->body_state(initial_record->body_id).has_value());
    assert(dirty_regions.count(dirty::DirtyRegionKind::chunk_collision) == 0);
    assert(dirty_regions.count(dirty::DirtyRegionKind::chunk_mesh) == 1);

    physics::PhysicsBodyDesc dropped;
    dropped.motion_type = physics::BodyMotionType::dynamic;
    dropped.position = {2.5F, 5.0F, 2.5F};
    dropped.mass = 1.0F;
    auto dropped_body = physics_world.value()->create_body(std::move(dropped));
    assert(dropped_body);
    for (std::uint32_t step = 0; step < 300; ++step) {
        assert(physics_world.value()->step({1.0F / 60.0F}));
    }
    const auto settled = physics_world.value()->body_state(dropped_body.value());
    assert(settled.has_value());
    assert(std::abs(settled->position.y - 1.5F) < 0.06F);

    const auto old_body = initial_record->body_id;
    assert(chunks.set(coordinate, {0, 0, 0}, world::VoxelCell::air(), dirty_regions));
    const auto rebuilt_revision = chunks.find(coordinate)->content_revision();
    assert(wait_for_collision_revision(*collision_system.value(), chunks, dirty_regions, palette,
                                       coordinate, rebuilt_revision));
    const auto* rebuilt_record = collision_system.value()->find(coordinate);
    assert(rebuilt_record != nullptr);
    assert(rebuilt_record->body_id != old_body);
    assert(rebuilt_record->box_count > 1);
    assert(!physics_world.value()->body_state(old_body).has_value());
    const auto rebuilt_body = rebuilt_record->body_id;

    assert(chunks.erase(coordinate));
    assert(collision_system.value()->update(chunks, dirty_regions, palette));
    assert(collision_system.value()->find(coordinate) == nullptr);
    assert(!physics_world.value()->body_state(rebuilt_body).has_value());
    assert(collision_system.value()->stats().resident_body_count == 0);
}

} // namespace

int main() {
    test_full_chunk_greedily_merges_to_one_box();
    test_partial_and_non_colliding_prototypes_are_preserved();
    test_snapshot_rejects_stale_identity_and_unknown_types();
    test_system_cooks_rebuilds_and_removes_terrain(heartstead::physics::PhysicsBackend::headless);
    test_system_cooks_rebuilds_and_removes_terrain(heartstead::physics::PhysicsBackend::jolt);
    return 0;
}
