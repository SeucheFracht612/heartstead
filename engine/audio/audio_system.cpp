#include "engine/audio/audio_system.hpp"

#include "engine/audio/miniaudio/miniaudio_backend.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace heartstead::audio {

namespace {

class NullAudioSystem final : public IAudioSystem {
  public:
    explicit NullAudioSystem(const AudioSystemDesc& desc)
        : mixer_(*desc.events, desc.mixer), output_channels_(desc.output_channels) {}

    [[nodiscard]] AudioBackend backend() const noexcept override {
        return AudioBackend::null_backend;
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return audio_backend_name(backend());
    }

    [[nodiscard]] AudioDeviceState device_state() const noexcept override {
        return AudioDeviceState::unavailable;
    }

    [[nodiscard]] core::Result<AudioVoiceId> play(const AudioPlayRequest& request) override {
        return mixer_.play(request);
    }

    [[nodiscard]] core::Status stop(AudioVoiceId voice) override {
        return mixer_.stop(voice);
    }

    [[nodiscard]] core::Status set_emitter(AudioVoiceId voice, AudioEmitterState emitter) override {
        return mixer_.set_emitter(voice, emitter);
    }

    [[nodiscard]] core::Status set_listener(AudioListenerState listener) override {
        return mixer_.set_listener(listener);
    }

    [[nodiscard]] core::Status set_bus_gain(AudioBus bus, float gain) override {
        return mixer_.set_bus_gain(bus, gain);
    }

    [[nodiscard]] core::Status update(float delta_seconds) override {
        return mixer_.advance(delta_seconds);
    }

    [[nodiscard]] core::Status render_offline(std::span<float> interleaved_output,
                                              std::uint32_t frame_count) override {
        const auto required = static_cast<std::size_t>(frame_count) * output_channels_;
        if (frame_count == 0 || interleaved_output.size() != required) {
            return core::Status::failure(
                "audio.invalid_offline_buffer",
                "offline audio output must exactly match frame count times channel count");
        }
        std::ranges::fill(interleaved_output, 0.0F);
        return core::Status::ok();
    }

    [[nodiscard]] bool is_active(AudioVoiceId voice) const noexcept override {
        return mixer_.is_active(voice);
    }

    [[nodiscard]] std::optional<AudioVoiceSnapshot>
    voice_snapshot(AudioVoiceId voice) const override {
        return mixer_.snapshot(voice);
    }

    [[nodiscard]] AudioSystemStats stats() const noexcept override {
        return mixer_.stats();
    }

    [[nodiscard]] core::Status request_device_reinitialize() override {
        return core::Status::ok();
    }

  private:
    AudioMixer mixer_;
    std::uint32_t output_channels_ = 2;
};

} // namespace

std::string_view audio_backend_name(AudioBackend backend) noexcept {
    switch (backend) {
    case AudioBackend::null_backend:
        return "null";
    case AudioBackend::miniaudio:
        return "miniaudio";
    }
    return "unknown";
}

std::string_view audio_bus_name(AudioBus bus) noexcept {
    switch (bus) {
    case AudioBus::master:
        return "master";
    case AudioBus::music:
        return "music";
    case AudioBus::sfx:
        return "sfx";
    case AudioBus::ambient:
        return "ambient";
    }
    return "unknown";
}

std::string_view audio_device_state_name(AudioDeviceState state) noexcept {
    switch (state) {
    case AudioDeviceState::unavailable:
        return "unavailable";
    case AudioDeviceState::running:
        return "running";
    case AudioDeviceState::reinitializing:
        return "reinitializing";
    case AudioDeviceState::silent_fallback:
        return "silent_fallback";
    }
    return "unknown";
}

core::Status AudioSystemDesc::validate() const {
    if (events == nullptr) {
        return core::Status::failure("audio.missing_event_registry",
                                     "audio system requires a sound event registry");
    }
    if (backend == AudioBackend::miniaudio && assets == nullptr) {
        return core::Status::failure("audio.missing_asset_catalog",
                                     "miniaudio backend requires the active asset catalog");
    }
    auto status = mixer.validate();
    if (!status) {
        return status;
    }
    if (sample_rate < 8'000 || sample_rate > 384'000) {
        return core::Status::failure("audio.invalid_sample_rate",
                                     "audio sample rate must be between 8000 and 384000");
    }
    if (output_channels == 0 || output_channels > 8) {
        return core::Status::failure("audio.invalid_channel_count",
                                     "audio output channel count must be between one and eight");
    }
    if (period_frames == 0 || period_frames > 16'384) {
        return core::Status::failure("audio.invalid_period",
                                     "audio output period must be between one and 16384 frames");
    }
    if (!open_output_device && use_null_output_device) {
        return core::Status::failure(
            "audio.invalid_output_configuration",
            "null output device selection requires output device creation");
    }
    return core::Status::ok();
}

core::Result<std::unique_ptr<IAudioSystem>> create_audio_system(AudioSystemDesc desc) {
    auto status = desc.validate();
    if (!status) {
        return core::Result<std::unique_ptr<IAudioSystem>>::failure(status.error().code,
                                                                    status.error().message);
    }
    switch (desc.backend) {
    case AudioBackend::null_backend:
        return core::Result<std::unique_ptr<IAudioSystem>>::success(
            std::make_unique<NullAudioSystem>(desc));
    case AudioBackend::miniaudio:
        return miniaudio::create_system(desc);
    }
    return core::Result<std::unique_ptr<IAudioSystem>>::failure("audio.unknown_backend",
                                                                "unknown audio backend");
}

} // namespace heartstead::audio
