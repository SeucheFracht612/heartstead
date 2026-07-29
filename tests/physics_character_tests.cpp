#include "engine/movement/physics_character_collision.hpp"
#include "engine/movement/player_controller.hpp"
#include "engine/world/fluids/fluid_state.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <utility>

namespace {

[[nodiscard]] heartstead::world::VoxelPalette character_palette() {
    using namespace heartstead;
    world::VoxelDefinition solid;
    solid.type = 1;
    solid.prototype_id = core::PrototypeId::parse("test:voxels/solid").value();
    solid.display_name = "solid";
    solid.terrain_material = "stone";
    solid.mining_tool = "pick";

    world::VoxelDefinition slab = solid;
    slab.type = 2;
    slab.prototype_id = core::PrototypeId::parse("test:voxels/slab").value();
    slab.display_name = "slab";
    slab.logical_occupancy = world::BlockLogicalOccupancy::partial;
    slab.collision_bounds = {{{0.0F, 0.0F, 0.0F}, {1.0F, 0.5F, 1.0F}}};
    slab.selection_bounds = slab.collision_bounds;
    slab.occlusion_bounds = slab.collision_bounds;
    slab.occlusion = world::BlockOcclusionBehavior::model;

    world::VoxelDefinition water = solid;
    water.type = 3;
    water.prototype_id = core::PrototypeId::parse("test:voxels/water").value();
    water.display_name = "water";
    water.logical_occupancy = world::BlockLogicalOccupancy::fluid;
    water.collision_bounds.clear();
    water.occlusion_bounds.clear();
    water.occlusion = world::BlockOcclusionBehavior::none;

    world::VoxelPalette palette;
    assert(palette.add(std::move(solid)));
    assert(palette.add(std::move(slab)));
    assert(palette.add(std::move(water)));
    return palette;
}

void fill_floor_and_step(heartstead::world::ChunkDatabase& chunks) {
    using namespace heartstead;
    auto& chunk = chunks.get_or_create({0, 0, 0});
    for (std::uint16_t z = 0; z < world::VoxelChunk::edge_length; ++z) {
        for (std::uint16_t x = 0; x < world::VoxelChunk::edge_length; ++x) {
            assert(chunk.set({x, 0, z}, {1, 0}));
        }
    }
    assert(chunk.set({4, 1, 2}, {2, 0}));
    // A voxel "slope" is a staircase of authored collision shapes, not an inclined plane.
    assert(chunk.set({6, 1, 8}, {2, 0}));
    assert(chunk.set({7, 1, 8}, {1, 0}));
    assert(chunk.set({8, 1, 8}, {1, 0}));
    assert(chunk.set({8, 2, 8}, {2, 0}));
    assert(chunk.set({9, 1, 8}, {1, 0}));
    assert(chunk.set({9, 2, 8}, {1, 0}));
    assert(chunk.set({15, 1, 12}, {1, 0}));
    assert(chunk.set({10, 1, 2}, {3, 0, world::full_fluid_state_bits()}));
}

void create_floor_and_step_bodies(heartstead::physics::IPhysicsWorld& physics_world) {
    using namespace heartstead;
    physics::PhysicsBodyDesc floor;
    floor.position = {16.0F, 0.5F, 16.0F};
    floor.shape.half_extents = {16.0F, 0.5F, 16.0F};
    assert(physics_world.create_body(std::move(floor)));

    physics::PhysicsBodyDesc step;
    step.position = {4.5F, 1.25F, 2.5F};
    step.shape.half_extents = {0.5F, 0.25F, 0.5F};
    assert(physics_world.create_body(std::move(step)));

    const auto create_static_box = [&physics_world](physics::Vec3 position,
                                                    physics::Vec3 half_extents) {
        physics::PhysicsBodyDesc body;
        body.position = position;
        body.shape.half_extents = half_extents;
        assert(physics_world.create_body(std::move(body)));
    };
    create_static_box({6.5F, 1.25F, 8.5F}, {0.5F, 0.25F, 0.5F});
    create_static_box({7.5F, 1.5F, 8.5F}, {0.5F, 0.5F, 0.5F});
    create_static_box({8.5F, 1.75F, 8.5F}, {0.5F, 0.75F, 0.5F});
    create_static_box({9.5F, 2.0F, 8.5F}, {0.5F, 1.0F, 0.5F});

    // A full-height voxel above the floor is deliberately too high to auto-step.
    create_static_box({15.5F, 1.5F, 12.5F}, {0.5F, 0.5F, 0.5F});
}

[[nodiscard]] heartstead::movement::PlayerInputFrame
input(std::uint64_t tick, std::uint32_t held = 0, std::uint32_t pressed = 0) {
    return {heartstead::movement::player_input_version, tick, tick, 0, 0, 0, 0, held, pressed};
}

void test_character_api_validation_and_capabilities() {
    using namespace heartstead;
    auto headless = physics::create_physics_world({});
    assert(headless);
    auto unsupported = headless.value()->create_character({});
    assert(!unsupported);
    assert(unsupported.error().code == "physics.character_controllers_unsupported");

    physics::PhysicsCharacterDesc invalid;
    invalid.shape.height = invalid.shape.width;
    assert(!physics::validate_physics_character_desc(invalid));
    assert(physics::physics_backend_capabilities(physics::PhysicsBackend::jolt)
               .supports_character_controllers);
}

void test_jolt_voxel_steps_jump_and_swim_buoyancy() {
    using namespace heartstead;
    if (!physics::physics_backend_info(physics::PhysicsBackend::jolt).available) {
        return;
    }

    auto physics_world = physics::create_physics_world(
        physics::PhysicsWorldDesc{.backend = physics::PhysicsBackend::jolt});
    assert(physics_world);
    create_floor_and_step_bodies(*physics_world.value());
    auto palette = character_palette();
    world::ChunkDatabase chunks;
    fill_floor_and_step(chunks);

    auto collision = movement::PhysicsCharacterCollisionWorld::create(
        *physics_world.value(), chunks, palette, world::WorldPosition{2.5, 1.0, 2.5}, {0.6, 1.8});
    assert(collision);
    auto supported =
        collision.value()->has_support(world::WorldPosition{2.5, 1.0, 2.5}, {0.6, 1.8});
    assert(supported && supported.value());

    auto croucher = movement::PhysicsCharacterCollisionWorld::create(
        *physics_world.value(), chunks, palette, world::WorldPosition{2.5, 1.0, 4.5}, {0.6, 1.2});
    assert(croucher);
    auto crouch_position = world::WorldPosition{2.5, 1.0, 4.5};
    for (std::uint32_t tick = 0; tick < 20; ++tick) {
        auto moved =
            croucher.value()->move(crouch_position, {0.6, 1.2}, {0.0, 0.0, 0.05}, 0.6, true);
        assert(moved);
        assert(moved.value().grounded);
        crouch_position = moved.value().position;
    }
    assert(crouch_position.approximate_global().z > 5.4);

    auto position = world::WorldPosition{2.5, 1.0, 2.5};
    bool stepped = false;
    for (std::uint32_t tick = 0; tick < 20; ++tick) {
        auto moved = collision.value()->move(position, {0.6, 1.8}, {0.1, 0.0, 0.0}, 0.6);
        assert(moved);
        position = moved.value().position;
        stepped |= moved.value().stepped;
    }
    assert(stepped);
    assert(position.approximate_global().x > 4.0);
    assert(std::abs(position.approximate_global().y - 1.5) < 0.06);

    auto terrace = movement::PhysicsCharacterCollisionWorld::create(
        *physics_world.value(), chunks, palette, world::WorldPosition{4.8, 1.0, 8.0}, {0.6, 1.8});
    assert(terrace);
    auto terrace_position = world::WorldPosition{4.8, 1.0, 8.0};
    for (std::uint32_t tick = 0; tick < 55; ++tick) {
        auto moved = terrace.value()->move(terrace_position, {0.6, 1.8}, {0.08, 0.0, 0.0}, 0.6);
        assert(moved);
        terrace_position = moved.value().position;
    }
    assert(terrace_position.approximate_global().x > 9.0);
    assert(terrace_position.approximate_global().y > 2.9);

    auto high_ledge = movement::PhysicsCharacterCollisionWorld::create(
        *physics_world.value(), chunks, palette, world::WorldPosition{13.5, 1.0, 12.5}, {0.6, 1.8});
    assert(high_ledge);
    auto ledge_position = world::WorldPosition{13.5, 1.0, 12.5};
    auto maximum_ledge_y = ledge_position.approximate_global().y;
    for (std::uint32_t tick = 0; tick < 55; ++tick) {
        auto moved = high_ledge.value()->move(ledge_position, {0.6, 1.8}, {0.08, 0.0, 0.0}, 0.6);
        assert(moved);
        ledge_position = moved.value().position;
        maximum_ledge_y = std::max(maximum_ledge_y, ledge_position.approximate_global().y);
    }
    assert(ledge_position.approximate_global().x < 15.0);
    assert(maximum_ledge_y < 1.06);

    physics::PhysicsBodyDesc pushable;
    pushable.motion_type = physics::BodyMotionType::dynamic;
    pushable.position = {6.0F, 1.5F, 18.0F};
    pushable.mass = 1.0F;
    auto pushable_body = physics_world.value()->create_body(std::move(pushable));
    assert(pushable_body);
    auto pusher = movement::PhysicsCharacterCollisionWorld::create(
        *physics_world.value(), chunks, palette, world::WorldPosition{4.5, 1.0, 18.0}, {0.6, 1.8});
    assert(pusher);
    auto pusher_position = world::WorldPosition{4.5, 1.0, 18.0};
    for (std::uint32_t tick = 0; tick < 40; ++tick) {
        auto moved = pusher.value()->move(pusher_position, {0.6, 1.8}, {0.08, 0.0, 0.0}, 0.6);
        assert(moved);
        pusher_position = moved.value().position;
        assert(physics_world.value()->step({1.0F / 60.0F}));
    }
    const auto pushed = physics_world.value()->body_state(pushable_body.value());
    assert(pushed.has_value());
    assert(pushed->position.x > 6.1F);

    auto jumper = movement::PhysicsCharacterCollisionWorld::create(
        *physics_world.value(), chunks, palette, world::WorldPosition{2.5, 1.0, 5.5}, {0.6, 1.8});
    assert(jumper);
    movement::PlayerController controller;
    movement::PlayerControllerState state;
    state.position = world::WorldPosition{2.5, 1.0, 5.5};
    state.fall_origin = state.position;
    state.scripted_start = state.position;
    state.scripted_target = state.position;
    state.mode = movement::PlayerControllerMode::grounded;
    state.grounded = true;
    const auto jump = movement::input_button_bit(movement::PlayerInputButton::jump);
    auto maximum_y = state.position.approximate_global().y;
    for (std::uint64_t tick = 1; tick <= 90; ++tick) {
        const auto buttons = tick == 1 ? jump : 0U;
        auto ticked = controller.tick(state, input(tick, buttons, buttons), {}, *jumper.value());
        assert(ticked);
        state = ticked.value().state;
        maximum_y = std::max(maximum_y, state.position.approximate_global().y);
    }
    assert(std::abs(maximum_y - 2.25) < 0.06);
    assert(state.grounded);

    auto swimming = collision.value()->touches_occupancy(
        world::WorldPosition{10.5, 1.0, 2.5}, {0.6, 1.8}, world::BlockLogicalOccupancy::fluid);
    assert(swimming && swimming.value());
    auto submerged =
        collision.value()->fluid_submersion(world::WorldPosition{10.5, 1.0, 2.5}, {0.6, 1.8});
    assert(submerged);
    assert(std::abs(submerged.value() - (1.0 / 1.8)) < 0.0001);
}

} // namespace

int main() {
    test_character_api_validation_and_capabilities();
    test_jolt_voxel_steps_jump_and_swim_buoyancy();
    return 0;
}
