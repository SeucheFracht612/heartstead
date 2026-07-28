#include "engine/movement/physics_character_collision.hpp"
#include "engine/movement/player_controller.hpp"

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
    assert(chunk.set({10, 1, 2}, {3, 0}));
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

    physics::PhysicsBodyDesc gentle_ramp;
    gentle_ramp.position = {8.0F, 2.7F, 8.0F};
    gentle_ramp.rotation_degrees = {0.0F, 0.0F, 30.0F};
    gentle_ramp.shape.half_extents = {3.0F, 0.1F, 1.0F};
    assert(physics_world.create_body(std::move(gentle_ramp)));

    physics::PhysicsBodyDesc steep_ramp;
    steep_ramp.position = {16.0F, 3.7F, 12.0F};
    steep_ramp.rotation_degrees = {0.0F, 0.0F, 60.0F};
    steep_ramp.shape.half_extents = {3.0F, 0.1F, 1.0F};
    assert(physics_world.create_body(std::move(steep_ramp)));
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

void test_jolt_step_jump_and_swim_stub() {
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

    auto gentle = movement::PhysicsCharacterCollisionWorld::create(
        *physics_world.value(), chunks, palette, world::WorldPosition{4.8, 1.0, 8.0}, {0.6, 1.8});
    assert(gentle);
    auto gentle_position = world::WorldPosition{4.8, 1.0, 8.0};
    for (std::uint32_t tick = 0; tick < 55; ++tick) {
        auto moved = gentle.value()->move(gentle_position, {0.6, 1.8}, {0.08, 0.0, 0.0}, 0.6);
        assert(moved);
        gentle_position = moved.value().position;
    }
    assert(gentle_position.approximate_global().x > 8.0);
    assert(gentle_position.approximate_global().y > 2.5);

    auto steep = movement::PhysicsCharacterCollisionWorld::create(
        *physics_world.value(), chunks, palette, world::WorldPosition{13.5, 1.0, 12.0}, {0.6, 1.8});
    assert(steep);
    auto steep_position = world::WorldPosition{13.5, 1.0, 12.0};
    for (std::uint32_t tick = 0; tick < 55; ++tick) {
        auto moved = steep.value()->move(steep_position, {0.6, 1.8}, {0.08, 0.0, 0.0}, 0.6);
        assert(moved);
        steep_position = moved.value().position;
    }
    assert(steep_position.approximate_global().x < 15.0);
    assert(steep_position.approximate_global().y < 1.6);

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
}

} // namespace

int main() {
    test_character_api_validation_and_capabilities();
    test_jolt_step_jump_and_swim_stub();
    return 0;
}
