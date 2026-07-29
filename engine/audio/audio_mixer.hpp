#pragma once

#include "engine/audio/audio_types.hpp"
#include "engine/audio/sound_event.hpp"
#include "engine/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace heartstead::audio {

struct AudioMixerConfig {
    std::uint32_t maximum_voices = 128;
    float gain_ramp_seconds = 0.02F;
    std::string fallback_event_id = "base:audio/interaction_fallback";

    [[nodiscard]] core::Status validate() const;
};

class AudioMixer {
  public:
    explicit AudioMixer(const SoundEventRegistry& events, AudioMixerConfig config = {});

    [[nodiscard]] core::Result<AudioVoiceId> play(const AudioPlayRequest& request);
    [[nodiscard]] core::Status stop(AudioVoiceId voice);
    [[nodiscard]] core::Status mark_finished(AudioVoiceId voice);
    [[nodiscard]] core::Status set_emitter(AudioVoiceId voice, AudioEmitterState emitter);
    [[nodiscard]] core::Status set_listener(AudioListenerState listener);
    [[nodiscard]] core::Status set_bus_gain(AudioBus bus, float gain);
    [[nodiscard]] float bus_gain(AudioBus bus) const noexcept;
    [[nodiscard]] float target_bus_gain(AudioBus bus) const noexcept;
    [[nodiscard]] core::Status advance(float delta_seconds);

    [[nodiscard]] bool is_active(AudioVoiceId voice) const noexcept;
    [[nodiscard]] std::optional<AudioVoiceSnapshot> snapshot(AudioVoiceId voice) const;
    [[nodiscard]] std::vector<AudioVoiceSnapshot> snapshots() const;
    [[nodiscard]] const AudioSystemStats& stats() const noexcept;
    [[nodiscard]] const AudioListenerState& listener() const noexcept;

  private:
    struct Voice {
        AudioVoiceId id;
        const SoundEventDefinition* event = nullptr;
        std::optional<AudioEmitterState> emitter;
        float request_gain = 1.0F;
        float pitch = 1.0F;
        std::uint64_t sequence = 0;
        AudioSpatialParameters spatial{};
    };

    [[nodiscard]] core::Status validate_listener(const AudioListenerState& listener) const;
    [[nodiscard]] std::optional<std::size_t>
    voice_to_steal(const SoundEventDefinition& event) const;
    [[nodiscard]] AudioSpatialParameters spatial_parameters(const Voice& voice) const;
    [[nodiscard]] std::size_t bus_index(AudioBus bus) const noexcept;
    void erase_voice(std::size_t index, bool stolen);

    const SoundEventRegistry* events_ = nullptr;
    AudioMixerConfig config_{};
    AudioListenerState listener_{};
    std::array<float, 4> bus_gains_{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> target_bus_gains_{1.0F, 1.0F, 1.0F, 1.0F};
    std::vector<Voice> voices_;
    std::unordered_map<std::uint64_t, std::size_t> by_id_;
    std::uint64_t next_voice_id_ = 1;
    std::uint64_t next_sequence_ = 1;
    std::optional<core::PrototypeId> fallback_event_id_;
    std::unordered_set<std::string> reported_missing_events_;
    AudioSystemStats stats_{};
};

} // namespace heartstead::audio
