#pragma once

#include "engine/audio/audio_mixer.hpp"
#include "engine/audio/audio_types.hpp"
#include "engine/audio/sound_event.hpp"
#include "engine/core/result.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace heartstead::audio {

struct AudioSystemDesc {
    AudioBackend backend = AudioBackend::null_backend;
    const SoundEventRegistry* events = nullptr;
    AudioMixerConfig mixer{};
    std::uint32_t sample_rate = 48'000;
    std::uint32_t output_channels = 2;
    std::uint32_t period_frames = 256;

    [[nodiscard]] core::Status validate() const;
};

class IAudioSystem {
  public:
    virtual ~IAudioSystem() = default;

    [[nodiscard]] virtual AudioBackend backend() const noexcept = 0;
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
    [[nodiscard]] virtual AudioDeviceState device_state() const noexcept = 0;
    [[nodiscard]] virtual core::Result<AudioVoiceId> play(const AudioPlayRequest& request) = 0;
    [[nodiscard]] virtual core::Status stop(AudioVoiceId voice) = 0;
    [[nodiscard]] virtual core::Status set_emitter(AudioVoiceId voice,
                                                   AudioEmitterState emitter) = 0;
    [[nodiscard]] virtual core::Status set_listener(AudioListenerState listener) = 0;
    [[nodiscard]] virtual core::Status set_bus_gain(AudioBus bus, float gain) = 0;
    [[nodiscard]] virtual core::Status update(float delta_seconds) = 0;
    [[nodiscard]] virtual bool is_active(AudioVoiceId voice) const noexcept = 0;
    [[nodiscard]] virtual std::optional<AudioVoiceSnapshot>
    voice_snapshot(AudioVoiceId voice) const = 0;
    [[nodiscard]] virtual AudioSystemStats stats() const noexcept = 0;
};

[[nodiscard]] core::Result<std::unique_ptr<IAudioSystem>> create_audio_system(AudioSystemDesc desc);

} // namespace heartstead::audio
