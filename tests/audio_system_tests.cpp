#include "engine/assets/asset_catalog.hpp"
#include "engine/audio/audio_mixer.hpp"
#include "engine/audio/audio_system.hpp"
#include "engine/audio/sound_event.hpp"
#include "engine/modding/generic_prototype.hpp"

#include <cassert>
#include <cmath>

namespace {

using namespace heartstead;

[[nodiscard]] core::PrototypeId id(std::string_view value) {
    return core::PrototypeId::parse(value).value();
}

[[nodiscard]] audio::SoundEventDefinition event(std::string_view value, std::uint8_t priority = 128,
                                                std::uint32_t maximum_instances = 8) {
    audio::SoundEventDefinition result;
    result.prototype_id = id(value);
    result.asset_id = "test:sounds/test.wav";
    result.priority = priority;
    result.maximum_instances = maximum_instances;
    return result;
}

[[nodiscard]] world::WorldPosition position(std::int64_t x, std::int64_t y, std::int64_t z,
                                            math::Vec3d local = {}) {
    return world::WorldPosition::from_anchor({x, y, z}, local).value();
}

[[nodiscard]] bool near(float lhs, float rhs, float tolerance = 0.0001F) {
    return std::abs(lhs - rhs) <= tolerance;
}

void test_sound_event_prototype_resolution() {
    assets::AssetCatalog assets;
    auto added = assets.add(assets::AssetRecord{
        "test:sounds/footstep.wav",
        assets::AssetKind::sound,
        assets::VirtualPath{"test", "sounds/footstep.wav"},
        assets::AssetSourceKind::mod,
        "test",
        0,
        "footstep.wav",
        "hash",
        false,
        {},
    });
    assert(added);

    modding::GenericPrototype prototype;
    prototype.kind = std::string(modding::PrototypeKinds::sound_event);
    prototype.id = id("test:audio/footstep");
    prototype.fields = {
        {"asset", "test:sounds/footstep.wav"},
        {"bus", "sfx"},
        {"gain", "0.75"},
        {"minimum_distance", "2"},
        {"maximum_distance", "18"},
        {"priority", "200"},
        {"maximum_instances", "4"},
        {"spatialized", "true"},
    };
    auto definition = audio::sound_event_definition_from_prototype(prototype, assets);
    assert(definition);
    assert(definition.value().prototype_id == prototype.id);
    assert(definition.value().asset_id == "test:sounds/footstep.wav");
    assert(definition.value().bus == audio::AudioBus::sfx);
    assert(near(definition.value().gain, 0.75F));
    assert(definition.value().priority == 200);
    assert(definition.value().maximum_instances == 4);

    prototype.fields["asset"] = "test:sounds/missing.wav";
    auto missing = audio::sound_event_definition_from_prototype(prototype, assets);
    assert(!missing);
    assert(missing.error().code == "sound_event.asset_missing");
}

void test_floating_origin_spatial_math_and_gain_ramps() {
    audio::SoundEventRegistry registry;
    auto spatial = event("test:audio/spatial");
    spatial.minimum_distance = 1.0F;
    spatial.maximum_distance = 11.0F;
    assert(registry.add(spatial));

    audio::AudioMixer mixer(registry, {8, 0.02F});
    audio::AudioListenerState listener;
    listener.position = position(4'000'000'000'000LL, 20, -4'000'000'000'000LL, {0.25, 0.5, 0.75});
    assert(mixer.set_listener(listener));

    audio::AudioEmitterState emitter;
    emitter.position = position(4'000'000'000'006LL, 20, -4'000'000'000'000LL, {0.25, 0.5, 0.75});
    auto voice = mixer.play({id("test:audio/spatial"), emitter, 1.0F, 1.0F});
    assert(voice);
    auto snapshot = mixer.snapshot(voice.value());
    assert(snapshot.has_value());
    assert(near(snapshot->spatial.distance, 6.0F));
    assert(near(snapshot->spatial.gain, 0.5F));
    assert(near(snapshot->spatial.pan, 1.0F));
    assert(near(snapshot->spatial.left_gain, 0.0F));
    assert(near(snapshot->spatial.right_gain, 0.5F));

    assert(mixer.set_bus_gain(audio::AudioBus::sfx, 0.0F));
    assert(mixer.advance(0.01F));
    snapshot = mixer.snapshot(voice.value());
    assert(snapshot.has_value());
    assert(near(snapshot->spatial.gain, 0.25F));
    assert(near(mixer.bus_gain(audio::AudioBus::sfx), 0.5F));
    assert(near(mixer.target_bus_gain(audio::AudioBus::sfx), 0.0F));
}

void test_cone_attenuation_and_priority_stealing() {
    audio::SoundEventRegistry registry;
    auto quiet = event("test:audio/quiet", 10, 1);
    quiet.minimum_distance = 2.0F;
    quiet.cone_inner_angle_degrees = 60.0F;
    quiet.cone_outer_angle_degrees = 120.0F;
    quiet.cone_outer_gain = 0.25F;
    assert(registry.add(quiet));
    assert(registry.add(event("test:audio/important", 220)));

    audio::AudioMixer mixer(registry, {1, 0.0F});
    assert(mixer.set_listener({position(0, 0, 2)}));
    audio::AudioEmitterState emitter;
    emitter.position = position(0, 0, 0);
    emitter.forward = {0.0F, 0.0F, -1.0F};
    auto quiet_voice = mixer.play({id("test:audio/quiet"), emitter});
    assert(quiet_voice);
    auto quiet_snapshot = mixer.snapshot(quiet_voice.value());
    assert(quiet_snapshot.has_value());
    assert(near(quiet_snapshot->spatial.gain, 0.25F));

    auto important = mixer.play({id("test:audio/important"), emitter});
    assert(important);
    assert(!mixer.is_active(quiet_voice.value()));
    assert(mixer.is_active(important.value()));
    assert(mixer.stats().stolen_voices == 1);

    auto rejected = mixer.play({id("test:audio/quiet"), emitter});
    assert(!rejected);
    assert(rejected.error().code == "audio.voice_limit");
    assert(mixer.stats().rejected_voices == 1);
}

void test_null_backend_owns_logical_voices() {
    audio::SoundEventRegistry registry;
    auto ambient = event("test:audio/ambient");
    ambient.bus = audio::AudioBus::ambient;
    ambient.spatialized = false;
    ambient.looping = true;
    ambient.streaming = true;
    assert(registry.add(ambient));

    audio::AudioSystemDesc desc;
    desc.events = &registry;
    auto system = audio::create_audio_system(desc);
    assert(system);
    assert(system.value()->backend() == audio::AudioBackend::null_backend);
    assert(system.value()->device_state() == audio::AudioDeviceState::unavailable);

    auto voice = system.value()->play({id("test:audio/ambient"), std::nullopt, 0.8F, 1.0F});
    assert(voice);
    assert(system.value()->is_active(voice.value()));
    assert(system.value()->update(1.0F / 60.0F));
    auto snapshot = system.value()->voice_snapshot(voice.value());
    assert(snapshot.has_value());
    assert(snapshot->looping);
    assert(snapshot->streaming);
    assert(system.value()->stop(voice.value()));
    assert(!system.value()->is_active(voice.value()));
}

} // namespace

int main() {
    test_sound_event_prototype_resolution();
    test_floating_origin_spatial_math_and_gain_ramps();
    test_cone_attenuation_and_priority_stealing();
    test_null_backend_owns_logical_voices();
    return 0;
}
