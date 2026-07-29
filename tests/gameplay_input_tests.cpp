#include "engine/input/input_action.hpp"
#include "engine/world/chunks/chunk_replication.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"
#include "game/features/interaction/voxel_raycast.hpp"

#include <cassert>
#include <cmath>
#include <string_view>
#include <utility>

using namespace heartstead;

namespace {

void test_contextual_action_map_and_rebinding() {
    auto actions = input::InputActionMap::gameplay_defaults();
    platform::WindowInputSnapshot snapshot;
    snapshot.down_keys = {platform::KeyCode::w};
    snapshot.pressed_keys = {platform::KeyCode::w};
    snapshot.down_mouse_buttons = {platform::MouseButton::left};
    snapshot.pressed_mouse_buttons = {platform::MouseButton::left};
    snapshot.mouse_delta_x = 7;
    snapshot.mouse_delta_y = -3;
    auto gameplay = actions.evaluate(snapshot);
    assert(gameplay[input::InputAction::move_forward].held);
    assert(gameplay[input::InputAction::move_forward].pressed);
    assert(gameplay[input::InputAction::primary_action].pressed);
    assert(gameplay.look_delta_x == 7 && gameplay.look_delta_y == -3);
    snapshot.pressed_keys = {platform::KeyCode::f1, platform::KeyCode::f3, platform::KeyCode::f4};
    const auto development_controls = actions.evaluate(snapshot);
    assert(development_controls[input::InputAction::toggle_camera].pressed);
    assert(development_controls[input::InputAction::toggle_debug].pressed);
    assert(development_controls[input::InputAction::toggle_debug_geometry].pressed);

    actions.set_context(input::InputContext::inventory);
    auto inventory = actions.evaluate(snapshot);
    assert(!inventory[input::InputAction::move_forward].held);
    actions.set_context(input::InputContext::gameplay);
    assert(actions.rebind(input::InputBinding::keyboard(
        input::InputAction::move_forward, input::InputContext::gameplay, platform::KeyCode::r)));
    auto rebound = actions.evaluate(snapshot);
    assert(!rebound[input::InputAction::move_forward].held);
    snapshot.down_keys = {platform::KeyCode::r};
    assert(actions.evaluate(snapshot)[input::InputAction::move_forward].held);
    assert(!actions.bind(input::InputBinding::keyboard(
        input::InputAction::interact, input::InputContext::gameplay, platform::KeyCode::r)));
}

void test_voxel_dda_hits_faces_and_negative_coordinates() {
    world::ChunkDatabase chunks;
    auto& origin_chunk = chunks.get_or_create({0, 0, 0});
    assert(origin_chunk.set({2, 0, 2}, {1, 0, 0, 0}));
    auto downward = game::interaction::raycast_voxels(
        chunks, {world::WorldPosition{2.5, 2.5, 2.5}, {0.0, -1.0, 0.0}, 6.0});
    assert(downward && downward.value().hit.has_value());
    assert(downward.value().hit->block == world::BlockCoord(2, 0, 2));
    assert(downward.value().hit->adjacent_block == world::BlockCoord(2, 1, 2));
    assert(downward.value().hit->face_normal == math::Coord3i(0, 1, 0));
    assert(downward.value().hit->distance == 1.5);

    auto& negative_chunk = chunks.get_or_create({-1, 0, 0});
    assert(negative_chunk.set({31, 2, 2}, {1, 0, 0, 0}));
    auto negative = game::interaction::raycast_voxels(
        chunks, {world::WorldPosition{0.5, 2.5, 2.5}, {-1.0, 0.0, 0.0}, 2.0});
    assert(negative && negative.value().hit.has_value());
    assert(negative.value().hit->block == world::BlockCoord(-1, 2, 2));
    assert(negative.value().hit->face_normal == math::Coord3i(1, 0, 0));

    auto unloaded = game::interaction::raycast_voxels(
        chunks, {world::WorldPosition{31.5, 2.5, 2.5}, {1.0, 0.0, 0.0}, 4.0});
    assert(unloaded && !unloaded.value().hit.has_value());
    assert(unloaded.value().encountered_unloaded_chunk);
}

void test_voxel_dda_preserves_large_world_anchor() {
    constexpr std::int64_t anchor = 9'000'000'000'000'000LL;
    world::ChunkDatabase chunks;
    const world::BlockCoord block{anchor + 1, 4, anchor + 1};
    const auto address = world::block_to_chunk_local(block);
    auto& chunk = chunks.get_or_create(address.chunk);
    assert(chunk.set(address.local, {1, 0, 0, 0}));
    auto origin = world::WorldPosition::from_anchor({anchor, 4, anchor + 1}, {0.5, 0.5, 0.5});
    assert(origin);
    auto result = game::interaction::raycast_voxels(chunks, {origin.value(), {1.0, 0.0, 0.0}, 4.0});
    assert(result && result.value().hit.has_value());
    assert(result.value().hit->block == block);
    assert(result.value().hit->distance == 0.5);
}

void test_voxel_raycast_uses_declared_selection_bounds() {
    world::VoxelPalette palette;
    world::VoxelDefinition slab;
    slab.type = 1;
    slab.prototype_id = core::PrototypeId::parse("test:voxels/slab").value();
    slab.display_name = "Slab";
    slab.terrain_material = "stone";
    slab.mining_tool = "pick";
    slab.logical_occupancy = world::BlockLogicalOccupancy::partial;
    slab.collision_bounds = {{{0.0F, 0.0F, 0.0F}, {1.0F, 0.5F, 1.0F}}};
    slab.selection_bounds = slab.collision_bounds;
    slab.occlusion_bounds = slab.collision_bounds;
    slab.occlusion = world::BlockOcclusionBehavior::model;
    assert(palette.add(std::move(slab)));

    world::VoxelDefinition solid;
    solid.type = 2;
    solid.prototype_id = core::PrototypeId::parse("test:voxels/solid").value();
    solid.display_name = "Solid";
    solid.terrain_material = "stone";
    solid.mining_tool = "pick";
    assert(palette.add(std::move(solid)));

    world::ChunkDatabase chunks;
    auto& chunk = chunks.get_or_create({0, 0, 0});
    assert(chunk.set({2, 0, 2}, {1, 0}));
    assert(chunk.set({2, 0, 3}, {2, 0}));

    auto top = game::interaction::raycast_voxels(
        chunks, {world::WorldPosition{2.5, 2.5, 2.5}, {0.0, -1.0, 0.0}, 6.0}, &palette);
    assert(top && top.value().hit.has_value());
    assert(top.value().hit->block == (world::BlockCoord{2, 0, 2}));
    assert(top.value().hit->face_normal == (math::Coord3i{0, 1, 0}));
    assert(top.value().hit->adjacent_block == (world::BlockCoord{2, 1, 2}));
    assert(std::abs(top.value().hit->distance - 2.0) < 0.0001);

    auto above_slab = game::interaction::raycast_voxels(
        chunks, {world::WorldPosition{2.5, 0.75, 1.5}, {0.0, 0.0, 1.0}, 6.0}, &palette);
    assert(above_slab && above_slab.value().hit.has_value());
    assert(above_slab.value().hit->block == (world::BlockCoord{2, 0, 3}));
    assert(above_slab.value().hit->face_normal == (math::Coord3i{0, 0, -1}));
}

void test_chunk_snapshot_slices_round_trip_worst_case_geometry() {
    world::ChunkDatabase chunks;
    auto& chunk = chunks.get_or_create({-2, 3, 4});
    for (std::uint16_t z = 0; z < world::VoxelChunk::edge_length; ++z) {
        for (std::uint16_t x = 0; x < world::VoxelChunk::edge_length; ++x) {
            assert(chunk.set({x, 7, z}, {static_cast<std::uint16_t>((x + z) % 2U + 1U),
                                         static_cast<std::uint8_t>((x * 7U + z) % 256U), 3, 9}));
        }
    }
    auto slices = world::make_chunk_snapshot_slices(chunk);
    assert(slices && slices.value().size() == world::VoxelChunk::edge_length);
    for (const auto& slice : slices.value()) {
        const auto encoded = world::ChunkSnapshotSliceTextCodec::encode(slice);
        assert(encoded.size() < 64U * 1024U);
        auto decoded = world::ChunkSnapshotSliceTextCodec::decode(encoded);
        assert(decoded);
        assert(decoded.value().identity == slice.identity);
        assert(decoded.value().content_revision == slice.content_revision);
        assert(decoded.value().slice_y == slice.slice_y);
        assert(decoded.value().cells == slice.cells);
        const auto binary = world::ChunkSnapshotSliceBinaryCodec::encode(slice);
        auto decoded_binary = world::ChunkSnapshotSliceBinaryCodec::decode(binary);
        assert(decoded_binary && decoded_binary.value().cells == slice.cells);
        assert(binary.size() < encoded.size());
        assert(!world::ChunkSnapshotSliceBinaryCodec::decode(
            std::string_view(binary).substr(0, binary.size() - 1)));
    }
}

} // namespace

int main() {
    test_contextual_action_map_and_rebinding();
    test_voxel_dda_hits_faces_and_negative_coordinates();
    test_voxel_dda_preserves_large_world_anchor();
    test_voxel_raycast_uses_declared_selection_bounds();
    test_chunk_snapshot_slices_round_trip_worst_case_geometry();
    return 0;
}
