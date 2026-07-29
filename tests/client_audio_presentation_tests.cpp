#include "engine/audio/audio_system.hpp"
#include "engine/audio/sound_event.hpp"
#include "engine/core/ids.hpp"
#include "game/presentation/client_audio_presentation.hpp"

#include <cassert>

namespace {

using namespace heartstead;

[[nodiscard]] core::PrototypeId id(std::string_view value) {
    return core::PrototypeId::parse(value).value();
}

[[nodiscard]] audio::SoundEventDefinition event(std::string_view value, bool spatialized,
                                                bool looping) {
    audio::SoundEventDefinition result;
    result.prototype_id = id(value);
    result.asset_id = "test:sounds/presentation.tone";
    result.spatialized = spatialized;
    result.looping = looping;
    return result;
}

void test_ambient_and_distance_driven_footsteps() {
    audio::SoundEventRegistry events;
    assert(events.add(event("test:audio/footstep", true, false)));
    assert(events.add(event("test:audio/ambient", false, true)));
    audio::AudioSystemDesc desc;
    desc.events = &events;
    auto audio = audio::create_audio_system(desc);
    assert(audio);

    game::ClientAudioPresentationConfig config;
    config.default_footstep_event_id = "test:audio/footstep";
    config.ambient_event_id = "test:audio/ambient";
    config.footstep_stride_meters = 1.0;
    game::ClientAudioPresentation presentation(config);
    assert(presentation.initialize(*audio.value()));
    assert(audio.value()->stats().active_voices == 1);

    movement::PlayerControllerState player;
    player.position = world::WorldPosition::from_anchor(
                          {8'000'000'000'000LL, 64, -8'000'000'000'000LL}, {0.25, 0.0, 0.75})
                          .value();
    player.velocity = {2.0, 0.0, 0.0};
    player.grounded = true;
    player.locomotion_animation.kind = animation::LocomotionAnimationKind::walk;
    movement::PlayerCameraFrame camera;
    camera.position =
        world::WorldPosition::from_anchor(player.position.anchor, {0.25, 1.6, 0.75}).value();
    camera.forward = {0.0, 0.0, -1.0};

    world::ChunkDatabase chunks;
    world::VoxelPalette palette;
    assert(presentation.update(*audio.value(), player, camera, chunks, palette, 0.25F));
    assert(presentation.stats().emitted_footsteps == 0);
    assert(presentation.update(*audio.value(), player, camera, chunks, palette, 0.25F));
    assert(presentation.stats().emitted_footsteps == 1);
    assert(presentation.stats().default_footsteps == 1);
    assert(audio.value()->stats().played_voices == 2);

    player.grounded = false;
    assert(presentation.update(*audio.value(), player, camera, chunks, palette, 1.0F));
    assert(presentation.stats().emitted_footsteps == 1);
    assert(presentation.stats().ambient_starts == 1);
    assert(presentation.shutdown(*audio.value()));
}

void test_partial_voxel_surface_selects_footstep_event() {
    audio::SoundEventRegistry events;
    assert(events.add(event("test:audio/stone_step", true, false)));
    assert(events.add(event("test:audio/ambient", false, true)));
    audio::AudioSystemDesc desc;
    desc.events = &events;
    auto audio = audio::create_audio_system(desc);
    assert(audio);

    game::ClientAudioPresentationConfig config;
    config.default_footstep_event_id = "test:audio/not_registered";
    config.ambient_event_id = "test:audio/ambient";
    config.footstep_stride_meters = 1.0;
    game::ClientAudioPresentation presentation(config);
    assert(presentation.initialize(*audio.value()));

    world::VoxelDefinition half_step;
    half_step.type = 1;
    half_step.prototype_id = id("test:voxels/half_step");
    half_step.display_name = "Half step";
    half_step.terrain_material = "stone";
    half_step.mining_tool = "pick";
    half_step.logical_occupancy = world::BlockLogicalOccupancy::partial;
    half_step.occlusion = world::BlockOcclusionBehavior::model;
    half_step.collision_bounds = {{{0.0F, 0.0F, 0.0F}, {1.0F, 0.5F, 1.0F}}};
    half_step.selection_bounds = half_step.collision_bounds;
    half_step.occlusion_bounds = half_step.collision_bounds;
    half_step.interaction.footstep_sound = id("test:audio/stone_step");
    world::VoxelPalette palette;
    assert(palette.add(std::move(half_step)));
    world::ChunkDatabase chunks;
    assert(chunks.get_or_create({0, 0, 0}).set({0, 0, 0}, {1, 0, 0, 0}));

    movement::PlayerControllerState player;
    player.position = world::WorldPosition{0.5, 0.5, 0.5};
    player.velocity = {2.0, 0.0, 0.0};
    player.grounded = true;
    player.locomotion_animation.kind = animation::LocomotionAnimationKind::walk;
    movement::PlayerCameraFrame camera;
    camera.position = world::WorldPosition{0.5, 2.1, 0.5};
    camera.forward = {0.0, 0.0, -1.0};

    assert(presentation.update(*audio.value(), player, camera, chunks, palette, 0.5F));
    assert(presentation.stats().emitted_footsteps == 1);
    assert(presentation.stats().surface_footsteps == 1);
    assert(presentation.stats().default_footsteps == 0);
    assert(presentation.shutdown(*audio.value()));
}

} // namespace

int main() {
    test_ambient_and_distance_driven_footsteps();
    test_partial_voxel_surface_selects_footstep_event();
    return 0;
}
