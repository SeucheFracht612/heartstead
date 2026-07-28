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
    config.footstep_event_id = "test:audio/footstep";
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
    movement::PlayerCameraFrame camera;
    camera.position =
        world::WorldPosition::from_anchor(player.position.anchor, {0.25, 1.6, 0.75}).value();
    camera.forward = {0.0, 0.0, -1.0};

    assert(presentation.update(*audio.value(), player, camera, 0.25F));
    assert(presentation.stats().emitted_footsteps == 0);
    assert(presentation.update(*audio.value(), player, camera, 0.25F));
    assert(presentation.stats().emitted_footsteps == 1);
    assert(audio.value()->stats().played_voices == 2);

    player.grounded = false;
    assert(presentation.update(*audio.value(), player, camera, 1.0F));
    assert(presentation.stats().emitted_footsteps == 1);
    assert(presentation.stats().ambient_starts == 1);
    assert(presentation.shutdown(*audio.value()));
}

} // namespace

int main() {
    test_ambient_and_distance_driven_footsteps();
    return 0;
}
