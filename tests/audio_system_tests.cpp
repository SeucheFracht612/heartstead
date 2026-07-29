#include "engine/assets/asset_catalog.hpp"
#include "engine/assets/asset_cooker.hpp"
#include "engine/assets/cooked_asset_store.hpp"
#include "engine/audio/audio_mixer.hpp"
#include "engine/audio/audio_system.hpp"
#include "engine/audio/sound_event.hpp"
#include "engine/core/logging.hpp"
#include "engine/debug/inspection.hpp"
#include "engine/modding/generic_prototype.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
        "0000000000000001",
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

void test_missing_event_uses_named_fallback_once_per_request_and_reports_once_per_id() {
    audio::SoundEventRegistry registry;
    auto fallback = event("test:audio/fallback");
    fallback.spatialized = true;
    assert(registry.add(fallback));

    audio::AudioMixerConfig config;
    config.maximum_voices = 8;
    config.fallback_event_id = "test:audio/fallback";
    audio::AudioMixer mixer(registry, config);

    std::vector<std::string> warnings;
    core::set_log_sink([&warnings](core::LogLevel level, std::string_view message) {
        if (level == core::LogLevel::warning) {
            warnings.emplace_back(message);
        }
    });
    audio::AudioEmitterState emitter;
    emitter.position = position(3, 4, 5);
    const auto missing_id = id("test:audio/not_registered");
    auto first = mixer.play({missing_id, emitter});
    auto second = mixer.play({missing_id, emitter});
    core::reset_log_sink();

    assert(first);
    assert(second);
    assert(mixer.snapshot(first.value())->event_id == id("test:audio/fallback"));
    assert(mixer.snapshot(second.value())->event_id == id("test:audio/fallback"));
    assert(mixer.stats().fallback_voices == 2);
    assert(mixer.stats().fallback_diagnostics == 1);
    assert(mixer.stats().played_voices == 2);
    assert(mixer.stats().rejected_voices == 0);
    assert(warnings.size() == 1);
    assert(warnings.front().find("test:audio/not_registered") != std::string::npos);
    assert(warnings.front().find("test:audio/fallback") != std::string::npos);

    audio::AudioMixer no_fallback(registry, {8, 0.02F, ""});
    auto rejected = no_fallback.play({id("test:audio/still_missing"), emitter});
    assert(!rejected);
    assert(rejected.error().code == "audio.event_missing");
    assert(no_fallback.stats().fallback_voices == 0);
    assert(no_fallback.stats().rejected_voices == 1);

    auto invalid_request = mixer.play({});
    assert(!invalid_request);
    assert(invalid_request.error().code == "audio.event_missing");
    assert(mixer.stats().fallback_voices == 2);
    assert(mixer.stats().fallback_diagnostics == 1);

    audio::AudioMixerConfig invalid;
    invalid.fallback_event_id = "not-a-logical-id";
    const auto invalid_status = invalid.validate();
    assert(!invalid_status);
    assert(invalid_status.error().code == "audio_mixer.invalid_fallback_event");
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
    std::array<float, 8> offline{};
    offline.fill(1.0F);
    assert(system.value()->render_offline(offline, 4));
    for (const auto sample : offline) {
        assert(sample == 0.0F);
    }
    auto snapshot = system.value()->voice_snapshot(voice.value());
    assert(snapshot.has_value());
    assert(snapshot->looping);
    assert(snapshot->streaming);
    assert(system.value()->stop(voice.value()));
    assert(!system.value()->is_active(voice.value()));
    const auto inspection = debug::Inspector::inspect(system.value()->stats());
    assert(inspection.object_type == "audio_system_stats");
    assert(inspection.find_field("active_voices")->value == "0");
    assert(inspection.find_field("played_voices")->value == "1");
    assert(inspection.issues.empty());
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void append_text(std::vector<std::uint8_t>& bytes, std::string_view value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void write_silent_wave(const std::filesystem::path& path) {
    constexpr std::uint32_t sample_rate = 48'000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits_per_sample = 16;
    constexpr std::uint32_t sample_count = 4'800;
    constexpr std::uint32_t data_size = sample_count * channels * (bits_per_sample / 8U);

    std::vector<std::uint8_t> bytes;
    bytes.reserve(44U + data_size);
    append_text(bytes, "RIFF");
    append_u32(bytes, 36U + data_size);
    append_text(bytes, "WAVEfmt ");
    append_u32(bytes, 16U);
    append_u16(bytes, 1U);
    append_u16(bytes, channels);
    append_u32(bytes, sample_rate);
    append_u32(bytes, sample_rate * channels * (bits_per_sample / 8U));
    append_u16(bytes, channels * (bits_per_sample / 8U));
    append_u16(bytes, bits_per_sample);
    append_text(bytes, "data");
    append_u32(bytes, data_size);
    bytes.resize(44U + data_size, 0U);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output.is_open());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    assert(output.good());
}

void test_miniaudio_backend_and_owner_thread_device_rebuild() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
                           ("heartstead_audio_system_tests_" + std::to_string(unique));
    assert(std::filesystem::create_directories(directory));
    const auto path = directory / "loop.wav";
    const auto tone_path = directory / "step.tone";
    write_silent_wave(path);
    {
        std::ofstream output(tone_path, std::ios::trunc);
        assert(output.is_open());
        output << "wave = \"noise\"\n"
                  "amplitude = 0.1\n"
                  "duration_ms = 80\n"
                  "attack_ms = 2\n"
                  "release_ms = 8\n"
                  "seed = 7\n";
        assert(output.good());
    }

    assets::AssetCatalog assets;
    assert(assets.add(assets::AssetRecord{
        "test:sounds/loop.wav",
        assets::AssetKind::sound,
        assets::VirtualPath{"test", "sounds/loop.wav"},
        assets::AssetSourceKind::mod,
        "test",
        0,
        path,
        "0000000000000001",
        false,
        {},
    }));
    assert(assets.add(assets::AssetRecord{
        "test:sounds/step.tone",
        assets::AssetKind::sound,
        assets::VirtualPath{"test", "sounds/step.tone"},
        assets::AssetSourceKind::mod,
        "test",
        0,
        tone_path,
        "0000000000000002",
        false,
        {},
    }));
    audio::SoundEventRegistry registry;
    auto loop = event("test:audio/miniaudio_loop");
    loop.asset_id = "test:sounds/loop.wav";
    loop.spatialized = false;
    loop.looping = true;
    loop.maximum_instances = 1;
    assert(registry.add(loop));
    auto tone = event("test:audio/miniaudio_tone");
    tone.asset_id = "test:sounds/step.tone";
    assert(registry.add(tone));

    assets::AssetCookConfig cook_config;
    cook_config.backend = assets::AssetCookBackend::production_converters;
    cook_config.output_root = directory / "cooked";
    assert(std::filesystem::create_directories(cook_config.output_root));
    auto cooked = assets::AssetCooker::cook(assets, cook_config);
    assert(cooked);
    auto cooked_store = assets::CookedAssetStore::load(cook_config.output_root);
    assert(cooked_store);
    assert(std::filesystem::remove(path));
    assert(std::filesystem::remove(tone_path));

    audio::AudioSystemDesc desc;
    desc.backend = audio::AudioBackend::miniaudio;
    desc.events = &registry;
    desc.assets = &assets;
    desc.cooked_assets = &cooked_store.value();
    desc.use_null_output_device = true;
    desc.mixer.fallback_event_id = "test:audio/miniaudio_tone";
    auto system = audio::create_audio_system(desc);
    assert(system);
    assert(system.value()->backend() == audio::AudioBackend::miniaudio);
    assert(system.value()->device_state() == audio::AudioDeviceState::running);
    auto voice = system.value()->play({id("test:audio/miniaudio_loop"), std::nullopt, 1.0F, 1.0F});
    assert(voice);
    assert(system.value()->update(0.01F));
    assert(system.value()->request_device_reinitialize());
    assert(system.value()->update(0.01F));
    assert(system.value()->device_state() == audio::AudioDeviceState::running);
    assert(system.value()->stats().device_reinitializations == 1);
    assert(system.value()->is_active(voice.value()));
    auto replacement =
        system.value()->play({id("test:audio/miniaudio_loop"), std::nullopt, 1.0F, 1.0F});
    assert(replacement);
    assert(!system.value()->is_active(voice.value()));
    assert(system.value()->is_active(replacement.value()));
    audio::AudioEmitterState tone_emitter;
    tone_emitter.position = position(2, 0, 0);
    auto tone_voice =
        system.value()->play({id("test:audio/miniaudio_tone"), tone_emitter, 1.0F, 1.0F});
    assert(tone_voice);
    assert(system.value()->voice_snapshot(tone_voice.value())->spatialized);
    assert(system.value()->stats().cached_assets == 2);
    assert(system.value()->stats().asset_cache_hits == 1);
    assert(system.value()->stats().source_asset_loads == 0);
    auto fallback_voice =
        system.value()->play({id("test:audio/missing_tone"), tone_emitter, 1.0F, 1.0F});
    assert(fallback_voice);
    auto fallback_snapshot = system.value()->voice_snapshot(fallback_voice.value());
    assert(fallback_snapshot.has_value());
    assert(fallback_snapshot->event_id == id("test:audio/miniaudio_tone"));
    assert(system.value()->stats().fallback_voices == 1);
    assert(system.value()->stats().cached_assets == 2);
    assert(system.value()->stats().asset_cache_hits == 2);
    assert(system.value()->stats().source_asset_loads == 0);
    assert(system.value()->stop(fallback_voice.value()));
    assert(system.value()->stop(tone_voice.value()));
    assert(system.value()->stop(replacement.value()));
    system.value().reset();
    assert(std::filesystem::remove_all(directory) > 0);
}

} // namespace

int main() {
    test_sound_event_prototype_resolution();
    test_floating_origin_spatial_math_and_gain_ramps();
    test_cone_attenuation_and_priority_stealing();
    test_missing_event_uses_named_fallback_once_per_request_and_reports_once_per_id();
    test_null_backend_owns_logical_voices();
    test_miniaudio_backend_and_owner_thread_device_rebuild();
    return 0;
}
