#include "engine/items/item_stack.hpp"
#include "engine/math/matrix.hpp"
#include "engine/movement/carried_load.hpp"
#include "engine/movement/character_collision.hpp"
#include "engine/movement/movement_prediction.hpp"
#include "engine/movement/player_camera.hpp"
#include "engine/movement/player_controller.hpp"
#include "engine/movement/player_controller_store.hpp"
#include "engine/movement/player_input.hpp"
#include "engine/movement/remote_player_interpolation.hpp"
#include "engine/simulation/fixed_step.hpp"
#include "engine/world/fluids/fluid_state.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace heartstead;

struct TestYard {
    world::VoxelPalette palette;
    world::ChunkDatabase chunks;
};

world::VoxelDefinition voxel(std::uint16_t type, std::string id) {
    world::VoxelDefinition result;
    result.type = type;
    result.prototype_id = core::PrototypeId::parse(std::move(id)).value();
    result.display_name = "Test";
    result.terrain_material = "test";
    result.mining_tool = "hand";
    return result;
}

TestYard make_yard() {
    TestYard yard;
    auto solid = voxel(1, "test:voxels/solid");
    assert(yard.palette.add(std::move(solid)));
    auto half = voxel(2, "test:voxels/half");
    half.logical_occupancy = world::BlockLogicalOccupancy::partial;
    half.occlusion = world::BlockOcclusionBehavior::model;
    half.collision_bounds = {{{0.0F, 0.0F, 0.0F}, {1.0F, 0.5F, 1.0F}}};
    half.occlusion_bounds = half.collision_bounds;
    assert(yard.palette.add(std::move(half)));
    auto water = voxel(3, "test:voxels/water");
    water.logical_occupancy = world::BlockLogicalOccupancy::fluid;
    water.collision_bounds.clear();
    water.occlusion = world::BlockOcclusionBehavior::none;
    water.occlusion_bounds.clear();
    water.tags = {"water"};
    assert(yard.palette.add(std::move(water)));
    auto ladder = voxel(4, "test:voxels/ladder");
    ladder.logical_occupancy = world::BlockLogicalOccupancy::decorative;
    ladder.collision_bounds.clear();
    ladder.occlusion = world::BlockOcclusionBehavior::none;
    ladder.occlusion_bounds.clear();
    ladder.tags = {"ladder"};
    assert(yard.palette.add(std::move(ladder)));

    auto& chunk = yard.chunks.get_or_create({0, 0, 0});
    for (std::uint16_t x = 0; x < world::VoxelChunk::edge_length; ++x) {
        for (std::uint16_t z = 0; z < world::VoxelChunk::edge_length; ++z) {
            assert(chunk.set({x, 0, z}, {1, 0, 0, 0}));
        }
    }
    assert(chunk.set({8, 1, 2}, {2, 0, 0, 0}));
    assert(chunk.set({12, 1, 2}, {1, 0, 0, 0}));
    assert(chunk.set({16, 1, 2}, {1, 0, 0, 0}));
    assert(chunk.set({16, 2, 2}, {1, 0, 0, 0}));
    assert(chunk.set({20, 1, 2}, {3, 0, world::full_fluid_state_bits(), 0}));
    assert(chunk.set({24, 1, 2}, {4, 0, 0, 0}));
    return yard;
}

movement::PlayerControllerState initial_state(double x = 2.5, double y = 1.0, double z = 2.5) {
    movement::PlayerControllerState state;
    state.position = world::WorldPosition{x, y, z};
    state.fall_origin = state.position;
    state.scripted_start = state.position;
    state.scripted_target = state.position;
    state.mode = movement::PlayerControllerMode::grounded;
    state.grounded = true;
    return state;
}

movement::PlayerInputFrame input(std::uint64_t tick, std::uint32_t held = 0,
                                 std::uint32_t pressed = 0, std::int16_t x = 0,
                                 std::int16_t z = 0) {
    return {movement::player_input_version, tick, tick, x, z, 0, 0, held, pressed};
}

void test_fixed_step_and_input_codec() {
    simulation::FixedStepClock clock;
    std::uint32_t total_steps = 0;
    for (int frame = 0; frame < 144; ++frame) {
        auto result = clock.advance(frame < 143 ? 6'944 : 7'008);
        assert(result);
        total_steps += result.value().step_count;
    }
    assert(total_steps == 60);
    simulation::FixedStepClock catch_up;
    auto stalled = catch_up.advance(1'000'000);
    assert(stalled);
    assert(stalled.value().step_count == 8);
    assert(stalled.value().dropped_time_us > 800'000);

    const auto jump = movement::input_button_bit(movement::PlayerInputButton::jump);
    auto source = input(7, jump, jump, 12'345, -23'456);
    source.yaw_centidegrees = 9'000;
    source.pitch_centidegrees = -1'250;
    auto decoded =
        movement::PlayerInputTextCodec::decode(movement::PlayerInputTextCodec::encode(source));
    assert(decoded);
    assert(decoded.value().tick == source.tick);
    assert(decoded.value().move_x == source.move_x);
    assert(decoded.value().pressed(movement::PlayerInputButton::jump));
    assert(!movement::PlayerInputTextCodec::decode("1|2"));
    movement::PlayerInputBundle bundle{{input(5), input(6), input(7)}};
    auto bundle_round_trip = movement::PlayerInputBundleTextCodec::decode(
        movement::PlayerInputBundleTextCodec::encode(bundle));
    assert(bundle_round_trip && bundle_round_trip.value().frames.size() == 3);
    movement::ServerMovementInputQueue queue;
    auto first_bundle = queue.push_bundle(bundle_round_trip.value());
    assert(first_bundle && first_bundle.value() == 3);
    auto duplicate_bundle = queue.push_bundle(bundle_round_trip.value());
    assert(duplicate_bundle && duplicate_bundle.value() == 0);

    platform::HeadlessPlatform platform;
    auto window = platform.create_window({"movement input", 800, 600, true});
    assert(window);
    assert(platform.poll_event()); // window_created
    assert(platform.set_cursor_capture(window.value(), true));
    platform::PlatformEvent first_motion;
    first_motion.kind = platform::PlatformEventKind::mouse_moved;
    first_motion.window_id = window.value();
    first_motion.mouse_x = 100;
    first_motion.mouse_y = 100;
    assert(platform.queue_event(first_motion));
    assert(platform.poll_event());
    platform::PlatformEvent second_motion = first_motion;
    second_motion.mouse_x = 112;
    second_motion.mouse_y = 91;
    assert(platform.queue_event(second_motion));
    assert(platform.poll_event());
    auto snapshot = platform.input_snapshot(window.value());
    assert(snapshot && snapshot->mouse_delta_x == 12 && snapshot->mouse_delta_y == -9);

    movement::PlayerInputSampler sampler;
    sampler.set_look_sensitivity(std::numeric_limits<double>::max());
    platform::WindowInputSnapshot small_motion;
    small_motion.mouse_delta_x = 1;
    auto bounded = sampler.sample(small_motion, 1);
    assert(bounded.yaw_centidegrees == 12);
    assert(bounded.validate());

    sampler.set_look_sensitivity(movement::max_player_look_sensitivity_centidegrees_per_pixel);
    platform::WindowInputSnapshot extreme_motion;
    extreme_motion.mouse_delta_x = std::numeric_limits<std::int32_t>::max();
    extreme_motion.mouse_delta_y = std::numeric_limits<std::int32_t>::min();
    auto normalized = sampler.sample(extreme_motion, 2);
    assert(normalized.yaw_centidegrees >= -18'000 && normalized.yaw_centidegrees <= 18'000);
    assert(normalized.pitch_centidegrees == 8'900);
    assert(normalized.validate());

    platform::PlatformEvent key_down;
    key_down.kind = platform::PlatformEventKind::key_down;
    key_down.window_id = window.value();
    key_down.key = platform::KeyCode::w;
    assert(platform.queue_event(key_down));
    assert(platform.poll_event());
    assert(platform.is_key_down(window.value(), platform::KeyCode::w));
    platform::PlatformEvent focus_lost;
    focus_lost.kind = platform::PlatformEventKind::window_focus_lost;
    focus_lost.window_id = window.value();
    assert(platform.queue_event(focus_lost));
    assert(platform.poll_event());
    assert(!platform.is_key_down(window.value(), platform::KeyCode::w));
    assert(platform.was_key_released(window.value(), platform::KeyCode::w));
    assert(!platform.cursor_captured(window.value()));
}

void test_walk_jump_dash_and_step() {
    auto yard = make_yard();
    movement::VoxelCharacterCollisionWorld collision(yard.chunks, yard.palette);
    movement::PlayerController controller;
    auto state = initial_state();
    const auto start_z = state.position.approximate_global().z;
    for (std::uint64_t tick = 1; tick <= 60; ++tick) {
        auto result = controller.tick(state, input(tick, 0, 0, 0, 32'767), {}, collision);
        assert(result);
        state = result.value().state;
    }
    const auto walked = state.position.approximate_global().z - start_z;
    assert(walked > 4.2 && walked < 4.6);

    const auto jump = movement::input_button_bit(movement::PlayerInputButton::jump);
    auto jump_state = initial_state(3.5, 1.0, 3.5);
    auto maximum_y = jump_state.position.approximate_global().y;
    for (std::uint64_t tick = 1; tick <= 90; ++tick) {
        const auto buttons = tick == 1 ? jump : 0u;
        auto result = controller.tick(jump_state, input(tick, buttons, buttons), {}, collision);
        assert(result);
        jump_state = result.value().state;
        maximum_y = std::max(maximum_y, jump_state.position.approximate_global().y);
    }
    assert(std::abs(maximum_y - 2.25) < 0.03);
    assert(jump_state.grounded);

    const auto dash = movement::input_button_bit(movement::PlayerInputButton::dash);
    auto dash_state = initial_state(3.5, 1.0, 3.5);
    const auto dash_start = dash_state.position.approximate_global().z;
    for (std::uint64_t tick = 1; tick <= 9; ++tick) {
        const auto buttons = tick == 1 ? dash : 0u;
        auto result =
            controller.tick(dash_state, input(tick, buttons, buttons, 0, 32'767), {}, collision);
        assert(result);
        dash_state = result.value().state;
    }
    assert(std::abs((dash_state.position.approximate_global().z - dash_start) - 4.0) < 0.01);

    auto step_move =
        collision.move(world::WorldPosition{7.2, 1.0, 2.5}, {0.6, 1.8}, {1.2, 0.0, 0.0}, 0.6);
    assert(step_move);
    assert(step_move.value().stepped);
    assert(step_move.value().position.approximate_global().y > 1.45);

    auto recovered = collision.depenetrate(world::WorldPosition{8.5, 1.25, 2.5}, {0.6, 1.8});
    assert(recovered);
    auto recovered_overlap = collision.overlaps(recovered.value(), {0.6, 1.8});
    assert(recovered_overlap && !recovered_overlap.value());
}

void test_roll_stamina_and_encumbrance() {
    auto yard = make_yard();
    movement::VoxelCharacterCollisionWorld collision(yard.chunks, yard.palette);
    movement::PlayerController controller;
    const auto roll = movement::input_button_bit(movement::PlayerInputButton::roll);
    auto state = initial_state(3.5, 1.0, 5.5);
    const auto start = state.position.approximate_global().z;
    bool observed_iframe = false;
    for (std::uint64_t tick = 1; tick <= 33; ++tick) {
        const auto buttons = tick == 1 ? roll : 0u;
        auto result =
            controller.tick(state, input(tick, buttons, buttons, 0, 32'767), {}, collision);
        assert(result);
        state = result.value().state;
        observed_iframe |= state.invulnerable;
    }
    assert(observed_iframe);
    assert(std::abs((state.position.approximate_global().z - start) - 3.5) < 0.02);
    assert(state.stamina_milli == 80'000);

    assert(movement::PlayerController::encumbrance_tier(399) == movement::EncumbranceTier::light);
    assert(movement::PlayerController::encumbrance_tier(400) == movement::EncumbranceTier::medium);
    assert(movement::PlayerController::encumbrance_tier(1001) ==
           movement::EncumbranceTier::over_encumbered);
}

void test_snapshot_prediction_camera_and_load() {
    auto yard = make_yard();
    movement::VoxelCharacterCollisionWorld collision(yard.chunks, yard.palette);
    movement::PlayerController controller;
    auto state = initial_state();
    auto first = controller.tick(state, input(1, 0, 0, 0, 32'767), {}, collision);
    assert(first);
    state = first.value().state;
    movement::PlayerControllerSnapshot snapshot;
    snapshot.player_net_id = core::NetId::from_value(42);
    snapshot.state = state;
    snapshot.last_processed_input_sequence = state.last_input_sequence;
    snapshot.collision_world_revision = 9;
    const auto encoded = movement::PlayerControllerSnapshotTextCodec::encode(snapshot);
    auto decoded = movement::PlayerControllerSnapshotTextCodec::decode(encoded);
    assert(decoded);
    assert(decoded.value().state.position == snapshot.state.position);
    assert(decoded.value().collision_world_revision == 9);

    movement::MovementPredictionBuffer prediction;
    auto second_input = input(2, 0, 0, 0, 32'767);
    assert(prediction.record(second_input));
    auto predicted = controller.tick(state, second_input, {}, collision);
    assert(predicted);
    auto reconciled =
        prediction.reconcile(predicted.value().state, snapshot, controller, {}, collision);
    assert(reconciled);
    assert(reconciled.value().replayed_input_count == 1);
    assert(reconciled.value().state.position == predicted.value().state.position);

    movement::PlayerCameraRig camera;
    auto frame = camera.evaluate(state, movement::PlayerCameraPerspective::first_person, 1280, 720);
    assert(frame);
    assert(frame.value().view_projection.is_finite());
    assert(!frame.value().body.local_body_visible);
    auto third = camera.evaluate(state, movement::PlayerCameraPerspective::third_person, 1280, 720);
    assert(third);
    assert(third.value().body.local_body_visible);

    movement::RemotePlayerInterpolator interpolator;
    auto remote_a = snapshot;
    remote_a.state.simulation_tick = 10;
    remote_a.state.last_input_sequence = 10;
    remote_a.last_processed_input_sequence = 10;
    remote_a.state.position = world::WorldPosition{0.5, 1.0, 0.5};
    auto remote_b = remote_a;
    remote_b.state.simulation_tick = 20;
    remote_b.state.last_input_sequence = 20;
    remote_b.last_processed_input_sequence = 20;
    remote_b.state.position = world::WorldPosition{10.5, 1.0, 0.5};
    assert(interpolator.push(remote_a));
    assert(interpolator.push(remote_b));
    auto interpolated = interpolator.sample(21); // target tick 15
    assert(interpolated);
    assert(std::abs(interpolated.value().position.approximate_global().x - 5.5) < 0.001);

    const auto clay_id = core::PrototypeId::parse("test:items/clay").value();
    const std::vector<items::ItemDefinition> definitions{{clay_id, 64, {}, 1000}};
    const std::vector<items::ItemStack> stacks{{clay_id, 20, 64, 100}};
    auto load = movement::summarize_carried_load(stacks, definitions, {}, 50'000);
    assert(load);
    assert(load.value().load_per_mille == 400);
}

void test_environment_fall_and_verb_boundaries() {
    auto yard = make_yard();
    movement::VoxelCharacterCollisionWorld collision(yard.chunks, yard.palette);
    movement::PlayerController controller;

    auto submersion =
        collision.fluid_submersion(world::WorldPosition{20.5, 1.0, 2.5}, {0.6, 1.8});
    assert(submersion);
    assert(std::abs(submersion.value() - (1.0 / 1.8)) < 0.0001);

    auto swim_state = initial_state(20.5, 1.0, 2.5);
    auto swim = controller.tick(swim_state, input(1, 0, 0, 0, 32'767), {}, collision);
    assert(swim);
    assert(swim.value().state.mode == movement::PlayerControllerMode::swimming);
    assert(swim.value().state.velocity.z > 0.0);
    assert(swim.value().state.stamina_milli < swim_state.stamina_milli);
    assert(std::ranges::any_of(swim.value().events, [](const auto& event) {
        return event.kind == movement::MovementEventKind::entered_fluid;
    }));

    const auto crouch = movement::input_button_bit(movement::PlayerInputButton::crouch);
    auto& fluid_chunk = *yard.chunks.find({0, 0, 0});
    assert(fluid_chunk.set({20, 2, 2},
                           {3, 0, world::full_fluid_state_bits(), 0}));
    auto deep_state = initial_state(20.5, 1.2, 2.5);
    deep_state.grounded = false;
    deep_state.mode = movement::PlayerControllerMode::airborne;
    auto buoyant = controller.tick(deep_state, input(1), {}, collision);
    assert(buoyant);
    assert(buoyant.value().state.velocity.y > 0.0);
    auto diving = controller.tick(deep_state, input(1, crouch), {}, collision);
    assert(diving);
    assert(diving.value().state.velocity.y < 0.0);

    assert(fluid_chunk.set({20, 2, 2}, world::VoxelCell::air()));
    auto thin_state = world::encode_fluid_state(
        {1, false, false, world::FluidFlowDirection::positive_x});
    assert(thin_state);
    assert(fluid_chunk.set({20, 1, 2}, {3, 0, thin_state.value(), 0}));
    auto shallow = collision.fluid_submersion(
        world::WorldPosition{20.5, 1.0, 2.5}, {0.6, 1.8});
    assert(shallow);
    assert(std::abs(shallow.value() - (0.125 / 1.8)) < 0.0001);
    auto wading = controller.tick(initial_state(20.5, 1.0, 2.5), input(1), {}, collision);
    assert(wading);
    assert(wading.value().state.mode != movement::PlayerControllerMode::swimming);
    auto hysteretic_state = initial_state(20.5, 1.0, 2.5);
    hysteretic_state.mode = movement::PlayerControllerMode::swimming;
    hysteretic_state.grounded = false;
    auto hysteretic = controller.tick(hysteretic_state, input(1), {}, collision);
    assert(hysteretic);
    assert(hysteretic.value().state.mode == movement::PlayerControllerMode::swimming);

    auto leaving_state = hysteretic.value().state;
    leaving_state.position = world::WorldPosition{22.5, 1.0, 2.5};
    leaving_state.fall_origin = leaving_state.position;
    auto left = controller.tick(leaving_state, input(2), {}, collision);
    assert(left);
    assert(left.value().state.mode != movement::PlayerControllerMode::swimming);
    assert(std::ranges::any_of(left.value().events, [](const auto& event) {
        return event.kind == movement::MovementEventKind::left_fluid;
    }));

    const auto interact = movement::input_button_bit(movement::PlayerInputButton::interact);
    auto ladder_state = initial_state(24.5, 1.0, 2.5);
    auto climb = controller.tick(ladder_state, input(1, interact, 0, 0, 32'767), {}, collision);
    assert(climb);
    assert(climb.value().state.mode == movement::PlayerControllerMode::climbing);
    assert(climb.value().state.velocity.y > 0.0);

    auto one_metre = collision.probe_ledge(world::WorldPosition{11.25, 1.0, 2.5}, {0.6, 1.8},
                                           {1.0, 0.0, 0.0}, 2.0);
    assert(one_metre && one_metre.value().has_value());
    auto two_metre = collision.probe_ledge(world::WorldPosition{15.25, 1.0, 2.5}, {0.6, 1.8},
                                           {1.0, 0.0, 0.0}, 2.0);
    assert(two_metre && two_metre.value().has_value());

    auto& chunk = *yard.chunks.find({0, 0, 0});
    assert(chunk.set({19, 1, 2}, {1, 0, 0, 0}));
    assert(chunk.set({19, 2, 2}, {1, 0, 0, 0}));
    assert(chunk.set({19, 3, 2}, {1, 0, 0, 0}));
    auto too_high = collision.probe_ledge(world::WorldPosition{18.25, 1.0, 2.5}, {0.6, 1.8},
                                          {1.0, 0.0, 0.0}, 2.0);
    assert(too_high && !too_high.value().has_value());

    const auto sprint = movement::input_button_bit(movement::PlayerInputButton::sprint);
    auto vault_state = initial_state(11.25, 1.0, 2.5);
    auto vault_input = input(1, sprint, 0, 0, 32'767);
    vault_input.yaw_centidegrees = 9'000;
    auto vault = controller.tick(vault_state, vault_input, {}, collision);
    assert(vault);
    vault_state = vault.value().state;
    assert(vault_state.scripted_kind == movement::ScriptedMovementKind::vault);
    for (std::uint64_t vault_tick = 2; vault_tick <= 16; ++vault_tick) {
        auto continued = input(vault_tick, sprint, 0, 0, 32'767);
        continued.yaw_centidegrees = 9'000;
        auto result = controller.tick(vault_state, continued, {}, collision);
        assert(result);
        vault_state = result.value().state;
    }
    assert(vault_state.grounded);
    assert(vault_state.position.approximate_global().x > 12.2);

    auto fall_state = initial_state(5.5, 7.0, 5.5);
    fall_state.grounded = false;
    fall_state.mode = movement::PlayerControllerMode::airborne;
    fall_state.fall_origin = fall_state.position;
    std::uint64_t tick = 1;
    bool landed = false;
    for (; tick < 180 && !landed; ++tick) {
        auto result = controller.tick(fall_state, input(tick), {}, collision);
        assert(result);
        fall_state = result.value().state;
        landed = std::ranges::any_of(result.value().events, [](const auto& event) {
            return event.kind == movement::MovementEventKind::landed;
        });
    }
    assert(landed);
    assert(fall_state.pending_fall_distance.has_value());
    const auto roll = movement::input_button_bit(movement::PlayerInputButton::roll);
    auto rolled = controller.tick(fall_state, input(tick, roll, roll, 0, 32'767), {}, collision);
    assert(rolled);
    assert(std::ranges::any_of(rolled.value().events, [](const auto& event) {
        return event.kind == movement::MovementEventKind::fall_impact &&
               event.damage_multiplier == 0.5 && event.value > 4.0;
    }));
}

void test_resource_tiers_and_air_dash() {
    auto yard = make_yard();
    movement::VoxelCharacterCollisionWorld collision(yard.chunks, yard.palette);
    movement::PlayerController controller;
    const auto dash = movement::input_button_bit(movement::PlayerInputButton::dash);

    auto light_air = initial_state(4.5, 5.0, 4.5);
    light_air.grounded = false;
    light_air.mode = movement::PlayerControllerMode::airborne;
    auto dashed = controller.tick(light_air, input(1, dash, dash, 0, 32'767), {}, collision);
    assert(dashed);
    assert(dashed.value().state.mode == movement::PlayerControllerMode::dashing);
    assert(!dashed.value().state.air_dash_available);

    movement::PlayerMovementModifiers medium;
    medium.load_per_mille = 400;
    auto medium_air = initial_state(4.5, 5.0, 4.5);
    medium_air.grounded = false;
    medium_air.mode = movement::PlayerControllerMode::airborne;
    auto rejected = controller.tick(medium_air, input(1, dash, dash, 0, 32'767), medium, collision);
    assert(rejected);
    assert(rejected.value().state.mode == movement::PlayerControllerMode::airborne);
    assert(rejected.value().state.stamina_milli == 100'000);

    movement::PlayerMovementModifiers over;
    over.load_per_mille = 1001;
    auto over_state = initial_state(4.5, 1.0, 8.5);
    over_state.stamina_milli = 50'000;
    const auto start = over_state.position.approximate_global().z;
    for (std::uint64_t tick = 1; tick <= 60; ++tick) {
        auto result = controller.tick(over_state, input(tick, 0, 0, 0, 32'767), over, collision);
        assert(result);
        over_state = result.value().state;
    }
    assert(over_state.position.approximate_global().z - start > 2.0);
    assert(over_state.position.approximate_global().z - start < 2.4);
    assert(over_state.stamina_milli == 50'000);

    movement::PlayerMovementModifiers hungry;
    hungry.stamina_regen_per_mille = 500;
    auto regen_state = initial_state(4.5, 1.0, 12.5);
    regen_state.stamina_milli = 50'000;
    for (std::uint64_t tick = 1; tick <= 60; ++tick) {
        auto result = controller.tick(regen_state, input(tick), hungry, collision);
        assert(result);
        regen_state = result.value().state;
    }
    assert(regen_state.stamina_milli == 57'000);

    regen_state.stamina_milli = 14'999;
    regen_state.exhausted = true;
    auto unlock = controller.tick(regen_state, input(61), {}, collision);
    assert(unlock);
    assert(!unlock.value().state.exhausted);
}

void test_coyote_buffer_and_crouch_edge_guard() {
    auto yard = make_yard();
    auto& chunk = *yard.chunks.find({0, 0, 0});
    for (std::uint16_t x = 1; x <= 4; ++x) {
        for (std::uint16_t z = 4; z <= 12; ++z) {
            assert(chunk.set({x, 0, z}, world::VoxelCell::air()));
        }
    }
    movement::VoxelCharacterCollisionWorld collision(yard.chunks, yard.palette);
    movement::PlayerController controller;

    auto crouched = initial_state(2.5, 1.0, 3.5);
    const auto crouch = movement::input_button_bit(movement::PlayerInputButton::crouch);
    for (std::uint64_t tick = 1; tick <= 90; ++tick) {
        auto result = controller.tick(crouched, input(tick, crouch, 0, 0, 32'767), {}, collision);
        assert(result);
        crouched = result.value().state;
    }
    assert(crouched.grounded);
    assert(crouched.position.approximate_global().z < 4.35);

    auto coyote = initial_state(2.5, 1.0, 3.5);
    std::uint64_t tick = 1;
    while (tick < 90 && coyote.grounded) {
        auto result = controller.tick(coyote, input(tick, 0, 0, 0, 32'767), {}, collision);
        assert(result);
        coyote = result.value().state;
        ++tick;
    }
    assert(!coyote.grounded);
    auto coast = controller.tick(coyote, input(tick++, 0, 0, 0, 32'767), {}, collision);
    assert(coast);
    coyote = coast.value().state;
    const auto jump = movement::input_button_bit(movement::PlayerInputButton::jump);
    auto coyote_jump = controller.tick(coyote, input(tick, jump, jump, 0, 32'767), {}, collision);
    assert(coyote_jump);
    assert(coyote_jump.value().state.velocity.y > 0.0);

    auto buffered = initial_state(8.5, 2.0, 8.5);
    buffered.grounded = false;
    buffered.mode = movement::PlayerControllerMode::airborne;
    buffered.velocity.y = -1.0;
    buffered.fall_origin = buffered.position;
    bool buffered_press_sent = false;
    bool jumped_after_landing = false;
    for (std::uint64_t buffer_tick = 1; buffer_tick < 60; ++buffer_tick) {
        const auto height = buffered.position.approximate_global().y;
        const auto should_press = !buffered_press_sent && height < 1.35;
        const auto buttons = should_press ? jump : 0u;
        auto result =
            controller.tick(buffered, input(buffer_tick, buttons, buttons), {}, collision);
        assert(result);
        buffered = result.value().state;
        buffered_press_sent |= should_press;
        jumped_after_landing |= std::ranges::any_of(result.value().events, [](const auto& event) {
            return event.kind == movement::MovementEventKind::jumped;
        });
        if (jumped_after_landing) {
            break;
        }
    }
    assert(buffered_press_sent);
    assert(jumped_after_landing);
}

void test_determinism_far_world_store_and_load_normalization() {
    auto yard = make_yard();
    movement::VoxelCharacterCollisionWorld collision(yard.chunks, yard.palette);
    movement::PlayerController controller;
    auto first = initial_state(3.5, 1.0, 10.5);
    auto second = first;
    for (std::uint64_t tick = 1; tick <= 600; ++tick) {
        auto frame = input(tick, 0, 0, 0, tick % 120 < 60 ? 32'767 : -32'767);
        frame.yaw_centidegrees = static_cast<std::int16_t>((tick / 120) % 2 == 0 ? 0 : 9'000);
        auto a = controller.tick(first, frame, {}, collision);
        auto b = controller.tick(second, frame, {}, collision);
        assert(a && b);
        first = a.value().state;
        second = b.value().state;
    }
    movement::PlayerControllerSnapshot first_snapshot;
    first_snapshot.player_net_id = core::NetId::from_value(2);
    first_snapshot.state = first;
    first_snapshot.last_processed_input_sequence = first.last_input_sequence;
    movement::PlayerControllerSnapshot second_snapshot = first_snapshot;
    second_snapshot.state = second;
    assert(movement::PlayerControllerSnapshotTextCodec::encode(first_snapshot) ==
           movement::PlayerControllerSnapshotTextCodec::encode(second_snapshot));

    movement::PlayerControllerStore store;
    movement::PlayerControllerRecord record;
    record.runtime_handle = core::RuntimeHandle::from_value(7);
    record.net_id = core::NetId::from_value(8);
    record.save_id = core::SaveId::from_value(9);
    record.state = first;
    assert(store.insert(record));
    assert(store.find_by_net_id(record.net_id) != nullptr);
    assert(store.find_by_save_id(record.save_id) != nullptr);
    assert(store.size() == 1);
    assert(!store.insert(record));

    auto transient = first;
    transient.mode = movement::PlayerControllerMode::rolling;
    transient.scripted_kind = movement::ScriptedMovementKind::roll;
    transient.invulnerable = true;
    transient.crouched = true;
    auto normalized =
        movement::normalize_player_controller_after_load(transient, controller, collision);
    assert(normalized);
    assert(normalized.value().scripted_kind == movement::ScriptedMovementKind::none);
    assert(!normalized.value().invulnerable);
    assert(normalized.value().mode == movement::PlayerControllerMode::grounded);

    assert(movement::player_controller_transition_allowed(movement::PlayerControllerMode::grounded,
                                                          movement::PlayerControllerMode::airborne,
                                                          movement::PlayerTransitionCause::jump));
    assert(!movement::player_controller_transition_allowed(movement::PlayerControllerMode::grounded,
                                                           movement::PlayerControllerMode::swimming,
                                                           movement::PlayerTransitionCause::roll));

    world::ChunkDatabase far_chunks;
    constexpr std::int64_t far = 9'000'000'000'000'000;
    for (std::int64_t x = far - 2; x <= far + 2; ++x) {
        for (std::int64_t z = far - 2; z <= far + 2; ++z) {
            const auto location = world::block_to_chunk_local({x, 0, z});
            assert(far_chunks.get_or_create(location.chunk).set(location.local, {1, 0, 0, 0}));
        }
    }
    movement::VoxelCharacterCollisionWorld far_collision(far_chunks, yard.palette);
    auto far_position = world::WorldPosition::from_anchor({far, 1, far}, {0.5, 0.0, 0.5});
    assert(far_position);
    auto far_move = far_collision.move(far_position.value(), {0.6, 1.8}, {0.2, 0.0, 0.0}, 0.6);
    assert(far_move);
    assert(far_move.value().position.anchor.x == far);
    assert(std::abs(far_move.value().position.local_offset.x - 0.7) < 0.001);
}

} // namespace

int main() {
    test_fixed_step_and_input_codec();
    test_walk_jump_dash_and_step();
    test_roll_stamina_and_encumbrance();
    test_snapshot_prediction_camera_and_load();
    test_environment_fall_and_verb_boundaries();
    test_resource_tiers_and_air_dash();
    test_coyote_buffer_and_crouch_edge_guard();
    test_determinism_far_world_store_and_load_normalization();
    return 0;
}
