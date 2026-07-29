#include "engine/audio/miniaudio/miniaudio_backend.hpp"

#include "engine/audio/procedural_tone.hpp"

#if HEARTSTEAD_HAS_MINIAUDIO
#include <miniaudio.h>
#endif

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heartstead::audio::miniaudio {

#if HEARTSTEAD_HAS_MINIAUDIO

namespace {

[[nodiscard]] core::Error miniaudio_error(std::string code, std::string message, ma_result result) {
    return core::Error{std::move(code), std::move(message) + ": " + ma_result_description(result)};
}

class MiniaudioSystem final : public IAudioSystem {
  public:
    explicit MiniaudioSystem(const AudioSystemDesc& desc)
        : desc_(desc), mixer_(*desc.events, desc.mixer) {}

    MiniaudioSystem(const MiniaudioSystem&) = delete;
    MiniaudioSystem& operator=(const MiniaudioSystem&) = delete;

    ~MiniaudioSystem() override {
        shutting_down_.store(true, std::memory_order_release);
        uninitialize_device();
        for (auto& [id, voice] : voices_) {
            (void)id;
            uninitialize_voice(*voice);
        }
        voices_.clear();
        unregister_cached_assets();
        uninitialize_groups();
        if (engine_initialized_) {
            ma_engine_uninit(&engine_);
            engine_initialized_ = false;
        }
        if (context_initialized_) {
            ma_context_uninit(&context_);
            context_initialized_ = false;
        }
    }

    [[nodiscard]] core::Status initialize() {
        auto context_config = ma_context_config_init();
        const ma_backend null_backend[] = {ma_backend_null};
        auto result =
            ma_context_init(desc_.use_null_output_device ? null_backend : nullptr,
                            desc_.use_null_output_device ? 1U : 0U, &context_config, &context_);
        if (result != MA_SUCCESS) {
            const auto error =
                miniaudio_error("audio.context_init_failed",
                                "miniaudio could not initialize its platform context", result);
            return core::Status::failure(error.code, error.message);
        }
        context_initialized_ = true;

        auto engine_config = ma_engine_config_init();
        engine_config.noDevice = MA_TRUE;
        engine_config.channels = desc_.output_channels;
        engine_config.sampleRate = desc_.sample_rate;
        engine_config.periodSizeInFrames = desc_.period_frames;
        result = ma_engine_init(&engine_config, &engine_);
        if (result != MA_SUCCESS) {
            const auto error =
                miniaudio_error("audio.engine_init_failed",
                                "miniaudio could not initialize its mixer engine", result);
            return core::Status::failure(error.code, error.message);
        }
        engine_initialized_ = true;

        auto status = initialize_groups();
        if (!status) {
            return status;
        }

        if (desc_.open_output_device) {
            if (!initialize_device()) {
                device_state_ = AudioDeviceState::silent_fallback;
                ++device_failures_;
            }
        }
        return core::Status::ok();
    }

    [[nodiscard]] AudioBackend backend() const noexcept override {
        return AudioBackend::miniaudio;
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return audio_backend_name(backend());
    }

    [[nodiscard]] AudioDeviceState device_state() const noexcept override {
        return device_state_;
    }

    [[nodiscard]] core::Result<AudioVoiceId> play(const AudioPlayRequest& request) override {
        auto logical_voice = mixer_.play(request);
        if (!logical_voice) {
            return logical_voice;
        }
        prune_inactive_backend_voices();
        const auto* event = desc_.events->find(request.event_id);
        const auto* asset = event == nullptr || desc_.assets == nullptr
                                ? nullptr
                                : desc_.assets->find_active(event->asset_id);
        if (event == nullptr || asset == nullptr) {
            (void)mixer_.stop(logical_voice.value());
            return core::Result<AudioVoiceId>::failure(
                "audio.asset_missing", "audio event asset is unavailable to the playback backend");
        }

        auto backend_voice = std::make_unique<BackendVoice>();
        ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
        if (event->looping) {
            flags |= MA_SOUND_FLAG_LOOPING;
        }
        std::string path;
        const auto use_cooked_asset = desc_.cooked_assets != nullptr;
        if (use_cooked_asset) {
            auto prepared = prepare_cooked_asset(*event);
            if (!prepared) {
                (void)mixer_.stop(logical_voice.value());
                return core::Result<AudioVoiceId>::failure(prepared.error().code,
                                                           prepared.error().message);
            }
            path = event->asset_id;
        } else {
            path = asset->source_path.string();
            ++source_asset_loads_;
        }
        ma_result result = MA_ERROR;
        if (!use_cooked_asset && is_procedural_tone_asset(asset->source_path)) {
            auto tone = load_procedural_tone_asset(asset->source_path, desc_.sample_rate);
            if (!tone) {
                (void)mixer_.stop(logical_voice.value());
                return core::Result<AudioVoiceId>::failure(tone.error().code, tone.error().message);
            }
            backend_voice->pcm = std::move(tone).value().mono_samples;
            auto buffer_config = ma_audio_buffer_config_init(
                ma_format_f32, 1, static_cast<ma_uint64>(backend_voice->pcm.size()),
                backend_voice->pcm.data(), nullptr);
            buffer_config.sampleRate = desc_.sample_rate;
            result = ma_audio_buffer_init(&buffer_config, &backend_voice->buffer);
            if (result == MA_SUCCESS) {
                backend_voice->buffer_initialized = true;
                result = ma_sound_init_from_data_source(
                    &engine_, reinterpret_cast<ma_data_source*>(&backend_voice->buffer), flags,
                    group(event->bus), &backend_voice->sound);
                backend_voice->sound_initialized = result == MA_SUCCESS;
            }
        } else {
            // Self-contained cooked buffers cannot use miniaudio's file-stream flag. They remain
            // registered once by logical ID and decode through the resource manager.
            flags |= use_cooked_asset
                         ? MA_SOUND_FLAG_DECODE
                         : (event->streaming ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE);
            result = ma_sound_init_from_file(&engine_, path.c_str(), flags, group(event->bus),
                                             nullptr, &backend_voice->sound);
            backend_voice->sound_initialized = result == MA_SUCCESS;
        }
        if (result != MA_SUCCESS) {
            uninitialize_voice(*backend_voice);
            (void)mixer_.stop(logical_voice.value());
            const auto error = miniaudio_error(
                "audio.sound_init_failed",
                "miniaudio could not initialize audio asset '" + event->asset_id + "'", result);
            return core::Result<AudioVoiceId>::failure(error.code, error.message);
        }
        apply_voice_parameters(logical_voice.value(), backend_voice->sound);
        result = ma_sound_start(&backend_voice->sound);
        if (result != MA_SUCCESS) {
            uninitialize_voice(*backend_voice);
            (void)mixer_.stop(logical_voice.value());
            const auto error = miniaudio_error("audio.sound_start_failed",
                                               "miniaudio could not start the audio voice", result);
            return core::Result<AudioVoiceId>::failure(error.code, error.message);
        }
        voices_.emplace(logical_voice.value().value(), std::move(backend_voice));
        return logical_voice;
    }

    [[nodiscard]] core::Status stop(AudioVoiceId voice) override {
        const auto found = voices_.find(voice.value());
        if (found == voices_.end()) {
            return core::Status::failure("audio.voice_missing", "audio voice is not active");
        }
        uninitialize_voice(*found->second);
        voices_.erase(found);
        return mixer_.stop(voice);
    }

    [[nodiscard]] core::Status set_emitter(AudioVoiceId voice, AudioEmitterState emitter) override {
        auto status = mixer_.set_emitter(voice, std::move(emitter));
        if (status) {
            const auto found = voices_.find(voice.value());
            if (found != voices_.end()) {
                apply_voice_parameters(voice, found->second->sound);
            }
        }
        return status;
    }

    [[nodiscard]] core::Status set_listener(AudioListenerState listener) override {
        auto status = mixer_.set_listener(std::move(listener));
        if (status) {
            apply_all_voice_parameters();
        }
        return status;
    }

    [[nodiscard]] core::Status set_bus_gain(AudioBus bus, float gain) override {
        return mixer_.set_bus_gain(bus, gain);
    }

    [[nodiscard]] core::Status update(float delta_seconds) override {
        auto status = mixer_.advance(delta_seconds);
        if (!status) {
            return status;
        }

        retry_elapsed_seconds_ += delta_seconds;
        if (device_reinitialize_requested_.exchange(false, std::memory_order_acq_rel) ||
            (desc_.open_output_device && device_state_ == AudioDeviceState::silent_fallback &&
             retry_elapsed_seconds_ >= device_retry_seconds_)) {
            retry_elapsed_seconds_ = 0.0F;
            rebuild_device();
        }

        std::vector<AudioVoiceId> finished;
        finished.reserve(voices_.size());
        for (const auto& [id, voice] : voices_) {
            const auto voice_id = AudioVoiceId::from_value(id);
            const auto snapshot = mixer_.snapshot(voice_id);
            if (!snapshot.has_value()) {
                finished.push_back(voice_id);
                continue;
            }
            if (!snapshot->looping && ma_sound_at_end(&voice->sound)) {
                finished.push_back(voice_id);
                continue;
            }
            apply_voice_parameters(voice_id, voice->sound);
        }
        for (const auto voice : finished) {
            const auto found = voices_.find(voice.value());
            if (found != voices_.end()) {
                uninitialize_voice(*found->second);
                voices_.erase(found);
            }
            if (mixer_.is_active(voice)) {
                (void)mixer_.mark_finished(voice);
            }
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Status render_offline(std::span<float> interleaved_output,
                                              std::uint32_t frame_count) override {
        const auto required = static_cast<std::size_t>(frame_count) * desc_.output_channels;
        if (frame_count == 0 || interleaved_output.size() != required) {
            return core::Status::failure(
                "audio.invalid_offline_buffer",
                "offline audio output must exactly match frame count times channel count");
        }
        if (device_initialized_) {
            return core::Status::failure(
                "audio.offline_with_device",
                "offline rendering requires an audio system without an output device");
        }
        const auto result =
            ma_engine_read_pcm_frames(&engine_, interleaved_output.data(), frame_count, nullptr);
        if (result != MA_SUCCESS) {
            const auto error =
                miniaudio_error("audio.offline_render_failed",
                                "miniaudio could not render the offline audio block", result);
            return core::Status::failure(error.code, error.message);
        }
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
        auto result = mixer_.stats();
        result.device_reinitializations = device_reinitializations_;
        result.device_failures = device_failures_;
        result.cached_assets = static_cast<std::uint32_t>(cached_assets_.size());
        result.asset_cache_hits = asset_cache_hits_;
        result.source_asset_loads = source_asset_loads_;
        return result;
    }

    [[nodiscard]] core::Status request_device_reinitialize() override {
        if (!desc_.open_output_device) {
            return core::Status::failure("audio.output_device_disabled",
                                         "audio system was configured without an output device");
        }
        device_reinitialize_requested_.store(true, std::memory_order_release);
        return core::Status::ok();
    }

  private:
    struct CachedAudioAsset {
        std::vector<std::uint8_t> encoded;
        std::vector<float> decoded_mono;
        bool registered = false;
    };

    struct BackendVoice {
        ma_sound sound{};
        ma_audio_buffer buffer{};
        std::vector<float> pcm;
        bool sound_initialized = false;
        bool buffer_initialized = false;
    };

    [[nodiscard]] core::Status prepare_cooked_asset(const SoundEventDefinition& event) {
        if (const auto existing = cached_assets_.find(event.asset_id);
            existing != cached_assets_.end()) {
            ++asset_cache_hits_;
            return core::Status::ok();
        }
        auto payload = desc_.cooked_assets->load_payload(event.asset_id);
        if (!payload) {
            return core::Status::failure(
                payload.error().code,
                "failed to load cooked audio asset '" + event.asset_id + "': " +
                    payload.error().message);
        }
        if ((payload.value().kind != assets::AssetKind::sound &&
             payload.value().kind != assets::AssetKind::music) ||
            payload.value().profile != "production") {
            return core::Status::failure(
                "audio.invalid_cooked_asset",
                "audio event requires a production-cooked sound or music asset: " +
                    event.asset_id);
        }
        const auto container = payload.value().metadata.find("audio.container");
        const auto runtime_format = payload.value().metadata.find("audio.runtime_format");
        if (container == payload.value().metadata.end() ||
            runtime_format == payload.value().metadata.end()) {
            return core::Status::failure(
                "audio.invalid_cooked_asset",
                "cooked audio asset is missing its container or runtime format: " +
                    event.asset_id);
        }

        CachedAudioAsset cached;
        cached.encoded = std::move(payload).value().bytes;
        const auto [inserted, was_inserted] =
            cached_assets_.emplace(event.asset_id, std::move(cached));
        if (!was_inserted) {
            ++asset_cache_hits_;
            return core::Status::ok();
        }

        auto* resource_manager = ma_engine_get_resource_manager(&engine_);
        ma_result registered = MA_ERROR;
        if (container->second == "tone" &&
            runtime_format->second == "heartstead.audio.procedural_tone.v1") {
            const auto manifest = std::string_view{
                reinterpret_cast<const char*>(inserted->second.encoded.data()),
                inserted->second.encoded.size()};
            auto tone = load_procedural_tone_asset(
                manifest, desc_.sample_rate, std::filesystem::path(event.asset_id));
            if (!tone) {
                cached_assets_.erase(inserted);
                return core::Status::failure(
                    tone.error().code,
                    "failed to materialize cooked tone '" + event.asset_id + "': " +
                        tone.error().message);
            }
            inserted->second.decoded_mono = std::move(tone).value().mono_samples;
            registered = ma_resource_manager_register_decoded_data(
                resource_manager, inserted->first.c_str(), inserted->second.decoded_mono.data(),
                static_cast<ma_uint64>(inserted->second.decoded_mono.size()), ma_format_f32, 1,
                desc_.sample_rate);
        } else if ((container->second == "wav" || container->second == "flac" ||
                    container->second == "ogg") &&
                   runtime_format->second == "heartstead.audio.container.v1") {
            registered = ma_resource_manager_register_encoded_data(
                resource_manager, inserted->first.c_str(), inserted->second.encoded.data(),
                inserted->second.encoded.size());
        } else {
            cached_assets_.erase(inserted);
            return core::Status::failure(
                "audio.unsupported_cooked_asset",
                "unsupported cooked audio runtime format for '" + event.asset_id + "': " +
                    runtime_format->second + " (" + container->second + ')');
        }
        if (registered != MA_SUCCESS) {
            cached_assets_.erase(inserted);
            const auto error = miniaudio_error(
                "audio.asset_registration_failed",
                "miniaudio could not register cooked audio asset '" + event.asset_id + "'",
                registered);
            return core::Status::failure(error.code, error.message);
        }
        inserted->second.registered = true;
        return core::Status::ok();
    }

    void unregister_cached_assets() noexcept {
        if (!engine_initialized_) {
            cached_assets_.clear();
            return;
        }
        auto* resource_manager = ma_engine_get_resource_manager(&engine_);
        for (auto& [logical_id, asset] : cached_assets_) {
            if (asset.registered) {
                (void)ma_resource_manager_unregister_data(resource_manager, logical_id.c_str());
                asset.registered = false;
            }
        }
        cached_assets_.clear();
    }

    static void data_callback(ma_device* device, void* output, const void* input,
                              ma_uint32 frame_count) {
        auto* self = static_cast<MiniaudioSystem*>(device->pUserData);
        if (self != nullptr && self->engine_initialized_) {
            (void)ma_engine_read_pcm_frames(&self->engine_, output, frame_count, nullptr);
        }
        (void)input;
    }

    static void notification_callback(const ma_device_notification* notification) {
        if (notification == nullptr || notification->pDevice == nullptr) {
            return;
        }
        auto* self = static_cast<MiniaudioSystem*>(notification->pDevice->pUserData);
        if (self == nullptr || self->shutting_down_.load(std::memory_order_acquire) ||
            self->suppress_device_notification_.load(std::memory_order_acquire)) {
            return;
        }
        if (notification->type == ma_device_notification_type_stopped) {
            self->device_reinitialize_requested_.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] core::Status initialize_groups() {
        auto result = ma_sound_group_init(&engine_, 0, nullptr, &master_group_);
        if (result != MA_SUCCESS) {
            const auto error =
                miniaudio_error("audio.group_init_failed",
                                "miniaudio could not initialize the master group", result);
            return core::Status::failure(error.code, error.message);
        }
        master_group_initialized_ = true;
        result = ma_sound_group_init(&engine_, 0, &master_group_, &music_group_);
        if (result != MA_SUCCESS) {
            const auto error =
                miniaudio_error("audio.group_init_failed",
                                "miniaudio could not initialize the music group", result);
            return core::Status::failure(error.code, error.message);
        }
        music_group_initialized_ = true;
        result = ma_sound_group_init(&engine_, 0, &master_group_, &sfx_group_);
        if (result != MA_SUCCESS) {
            const auto error = miniaudio_error(
                "audio.group_init_failed", "miniaudio could not initialize the SFX group", result);
            return core::Status::failure(error.code, error.message);
        }
        sfx_group_initialized_ = true;
        result = ma_sound_group_init(&engine_, 0, &master_group_, &ambient_group_);
        if (result != MA_SUCCESS) {
            const auto error =
                miniaudio_error("audio.group_init_failed",
                                "miniaudio could not initialize the ambient group", result);
            return core::Status::failure(error.code, error.message);
        }
        ambient_group_initialized_ = true;
        return core::Status::ok();
    }

    void uninitialize_groups() {
        if (ambient_group_initialized_) {
            ma_sound_group_uninit(&ambient_group_);
            ambient_group_initialized_ = false;
        }
        if (sfx_group_initialized_) {
            ma_sound_group_uninit(&sfx_group_);
            sfx_group_initialized_ = false;
        }
        if (music_group_initialized_) {
            ma_sound_group_uninit(&music_group_);
            music_group_initialized_ = false;
        }
        if (master_group_initialized_) {
            ma_sound_group_uninit(&master_group_);
            master_group_initialized_ = false;
        }
    }

    [[nodiscard]] ma_sound_group* group(AudioBus bus) noexcept {
        switch (bus) {
        case AudioBus::master:
            return &master_group_;
        case AudioBus::music:
            return &music_group_;
        case AudioBus::sfx:
            return &sfx_group_;
        case AudioBus::ambient:
            return &ambient_group_;
        }
        return &master_group_;
    }

    [[nodiscard]] bool initialize_device() {
        if (!context_initialized_ || !engine_initialized_) {
            return false;
        }
        auto config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = desc_.output_channels;
        config.sampleRate = desc_.sample_rate;
        config.periodSizeInFrames = desc_.period_frames;
        config.dataCallback = data_callback;
        config.notificationCallback = notification_callback;
        config.pUserData = this;
        auto result = ma_device_init(&context_, &config, &device_);
        if (result != MA_SUCCESS) {
            return false;
        }
        device_initialized_ = true;
        result = ma_device_start(&device_);
        if (result != MA_SUCCESS) {
            uninitialize_device();
            return false;
        }
        device_state_ = AudioDeviceState::running;
        return true;
    }

    void uninitialize_device() {
        if (!device_initialized_) {
            return;
        }
        suppress_device_notification_.store(true, std::memory_order_release);
        ma_device_uninit(&device_);
        device_initialized_ = false;
        suppress_device_notification_.store(false, std::memory_order_release);
    }

    void rebuild_device() {
        device_state_ = AudioDeviceState::reinitializing;
        uninitialize_device();
        ++device_reinitializations_;
        if (!initialize_device()) {
            ++device_failures_;
            device_state_ = AudioDeviceState::silent_fallback;
        }
    }

    void apply_voice_parameters(AudioVoiceId id, ma_sound& sound) {
        const auto snapshot = mixer_.snapshot(id);
        if (!snapshot.has_value()) {
            return;
        }
        ma_sound_set_volume(&sound, snapshot->spatial.gain);
        ma_sound_set_pan_mode(&sound, ma_pan_mode_pan);
        ma_sound_set_pan(&sound, snapshot->spatial.pan);
        ma_sound_set_pitch(&sound, snapshot->pitch);
    }

    void apply_all_voice_parameters() {
        for (auto& [id, voice] : voices_) {
            apply_voice_parameters(AudioVoiceId::from_value(id), voice->sound);
        }
    }

    void prune_inactive_backend_voices() {
        for (auto iterator = voices_.begin(); iterator != voices_.end();) {
            if (mixer_.is_active(AudioVoiceId::from_value(iterator->first))) {
                ++iterator;
                continue;
            }
            uninitialize_voice(*iterator->second);
            iterator = voices_.erase(iterator);
        }
    }

    static void uninitialize_voice(BackendVoice& voice) {
        if (voice.sound_initialized) {
            ma_sound_uninit(&voice.sound);
            voice.sound_initialized = false;
        }
        if (voice.buffer_initialized) {
            ma_audio_buffer_uninit(&voice.buffer);
            voice.buffer_initialized = false;
        }
    }

    AudioSystemDesc desc_{};
    AudioMixer mixer_;
    ma_context context_{};
    ma_engine engine_{};
    ma_device device_{};
    ma_sound_group master_group_{};
    ma_sound_group music_group_{};
    ma_sound_group sfx_group_{};
    ma_sound_group ambient_group_{};
    std::unordered_map<std::uint64_t, std::unique_ptr<BackendVoice>> voices_;
    std::unordered_map<std::string, CachedAudioAsset> cached_assets_;
    std::atomic<bool> device_reinitialize_requested_{false};
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> suppress_device_notification_{false};
    AudioDeviceState device_state_ = AudioDeviceState::unavailable;
    std::uint64_t device_reinitializations_ = 0;
    std::uint64_t device_failures_ = 0;
    std::uint64_t asset_cache_hits_ = 0;
    std::uint64_t source_asset_loads_ = 0;
    float retry_elapsed_seconds_ = 0.0F;
    float device_retry_seconds_ = 1.0F;
    bool context_initialized_ = false;
    bool engine_initialized_ = false;
    bool device_initialized_ = false;
    bool master_group_initialized_ = false;
    bool music_group_initialized_ = false;
    bool sfx_group_initialized_ = false;
    bool ambient_group_initialized_ = false;
};

} // namespace

core::Result<std::unique_ptr<IAudioSystem>> create_system(const AudioSystemDesc& desc) {
    auto system = std::make_unique<MiniaudioSystem>(desc);
    auto status = system->initialize();
    if (!status) {
        return core::Result<std::unique_ptr<IAudioSystem>>::failure(status.error().code,
                                                                    status.error().message);
    }
    return core::Result<std::unique_ptr<IAudioSystem>>::success(std::move(system));
}

#else

core::Result<std::unique_ptr<IAudioSystem>> create_system(const AudioSystemDesc&) {
    return core::Result<std::unique_ptr<IAudioSystem>>::failure(
        "audio.miniaudio_unavailable", "miniaudio backend was disabled at build time");
}

#endif

} // namespace heartstead::audio::miniaudio
