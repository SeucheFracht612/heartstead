#pragma once

#include "engine/core/ids.hpp"
#include "engine/math/vector.hpp"
#include "engine/world/coords/world_position.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace heartstead::audio {

struct AudioVoiceIdTag;
using AudioVoiceId = core::StrongU64Id<AudioVoiceIdTag>;

enum class AudioBackend {
    null_backend,
    miniaudio,
};

enum class AudioBus : std::uint8_t {
    master,
    music,
    sfx,
    ambient,
};

enum class AudioDeviceState : std::uint8_t {
    unavailable,
    running,
    reinitializing,
    silent_fallback,
};

struct AudioListenerState {
    world::WorldPosition position{};
    math::Vec3f velocity{};
    math::Vec3f forward{0.0F, 0.0F, -1.0F};
    math::Vec3f up{0.0F, 1.0F, 0.0F};
};

struct AudioEmitterState {
    world::WorldPosition position{};
    math::Vec3f velocity{};
    math::Vec3f forward{0.0F, 0.0F, -1.0F};
};

struct AudioPlayRequest {
    core::PrototypeId event_id;
    std::optional<AudioEmitterState> emitter;
    float gain = 1.0F;
    float pitch = 1.0F;
};

struct AudioSpatialParameters {
    float gain = 0.0F;
    float pan = 0.0F;
    float left_gain = 0.0F;
    float right_gain = 0.0F;
    float distance = 0.0F;
};

struct AudioVoiceSnapshot {
    AudioVoiceId id;
    core::PrototypeId event_id;
    AudioBus bus = AudioBus::sfx;
    AudioSpatialParameters spatial{};
    float pitch = 1.0F;
    std::uint8_t priority = 128;
    bool looping = false;
    bool streaming = false;
    bool spatialized = false;
};

struct AudioSystemStats {
    std::uint32_t active_voices = 0;
    std::uint32_t maximum_voices = 0;
    std::uint64_t played_voices = 0;
    std::uint64_t stopped_voices = 0;
    std::uint64_t stolen_voices = 0;
    std::uint64_t rejected_voices = 0;
    std::uint64_t device_reinitializations = 0;
    std::uint64_t device_failures = 0;
    std::uint32_t cached_assets = 0;
    std::uint64_t asset_cache_hits = 0;
    std::uint64_t source_asset_loads = 0;
    float master_gain = 1.0F;
};

[[nodiscard]] std::string_view audio_backend_name(AudioBackend backend) noexcept;
[[nodiscard]] std::string_view audio_bus_name(AudioBus bus) noexcept;
[[nodiscard]] std::string_view audio_device_state_name(AudioDeviceState state) noexcept;

} // namespace heartstead::audio
