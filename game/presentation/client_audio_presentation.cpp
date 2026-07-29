#include "game/presentation/client_audio_presentation.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace heartstead::game {

core::Status ClientAudioPresentationConfig::validate() const {
    if (!core::PrototypeId::parse(default_footstep_event_id).has_value() ||
        !core::PrototypeId::parse(ambient_event_id).has_value()) {
        return core::Status::failure(
            "client_audio.invalid_event",
            "client audio presentation event ids must be valid prototype ids");
    }
    if (!std::isfinite(footstep_stride_meters) || footstep_stride_meters <= 0.0 ||
        !std::isfinite(minimum_footstep_speed) || minimum_footstep_speed < 0.0) {
        return core::Status::failure(
            "client_audio.invalid_footstep_config",
            "client audio footstep stride and minimum speed must be finite and valid");
    }
    if (maximum_footsteps_per_update == 0) {
        return core::Status::failure("client_audio.invalid_footstep_budget",
                                     "client audio must permit at least one footstep per update");
    }
    return core::Status::ok();
}

ClientAudioPresentation::ClientAudioPresentation(ClientAudioPresentationConfig config)
    : config_(std::move(config)),
      default_footstep_event_id_(core::PrototypeId::parse(config_.default_footstep_event_id)
                                     .value_or(core::PrototypeId{})),
      ambient_event_id_(
          core::PrototypeId::parse(config_.ambient_event_id).value_or(core::PrototypeId{})) {}

core::Status ClientAudioPresentation::initialize(audio::IAudioSystem& audio) {
    auto status = config_.validate();
    if (!status) {
        return status;
    }
    if (initialized_) {
        return core::Status::failure("client_audio.already_initialized",
                                     "client audio presentation is already initialized");
    }
    initialized_ = true;
    status = ensure_ambient(audio);
    if (!status) {
        initialized_ = false;
    }
    return status;
}

core::Status ClientAudioPresentation::update(audio::IAudioSystem& audio,
                                             const movement::PlayerControllerState& player,
                                             const movement::PlayerCameraFrame& camera,
                                             const world::ChunkDatabase& chunks,
                                             const world::VoxelPalette& palette,
                                             float delta_seconds) {
    if (!initialized_) {
        return core::Status::failure("client_audio.not_initialized",
                                     "client audio presentation must be initialized");
    }
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0F) {
        return core::Status::failure("client_audio.invalid_delta",
                                     "client audio delta must be finite and non-negative");
    }

    audio::AudioListenerState listener;
    listener.position = camera.position;
    listener.velocity = {static_cast<float>(player.velocity.x),
                         static_cast<float>(player.velocity.y),
                         static_cast<float>(player.velocity.z)};
    listener.forward = {static_cast<float>(camera.forward.x), static_cast<float>(camera.forward.y),
                        static_cast<float>(camera.forward.z)};
    if (math::length_squared(math::cross(listener.forward, listener.up)) <= 0.000001F) {
        listener.forward = {0.0F, 0.0F, -1.0F};
    }
    auto status = audio.set_listener(listener);
    if (!status) {
        return status;
    }
    status = ensure_ambient(audio);
    if (!status) {
        return status;
    }

    const auto planar_speed = std::sqrt((player.velocity.x * player.velocity.x) +
                                        (player.velocity.z * player.velocity.z));
    const auto walking =
        player.locomotion_animation.kind == animation::LocomotionAnimationKind::walk ||
        player.locomotion_animation.kind == animation::LocomotionAnimationKind::run;
    if (player.grounded && walking && planar_speed >= config_.minimum_footstep_speed) {
        walked_distance_meters_ += planar_speed * static_cast<double>(delta_seconds);
        movement::VoxelCharacterCollisionWorld collision(chunks, palette);
        const movement::CharacterShape shape{0.6, player.crouched ? 1.2 : 1.8};
        auto support = collision.supporting_voxel(player.position, shape, 0.12);
        if (!support) {
            return core::Status::failure(support.error().code, support.error().message);
        }
        auto footstep_event = default_footstep_event_id_;
        const auto uses_surface_sound =
            support.value().has_value() && support.value()->voxel != nullptr &&
            support.value()->voxel->interaction.footstep_sound.has_value();
        if (uses_surface_sound) {
            footstep_event = *support.value()->voxel->interaction.footstep_sound;
        }
        std::uint32_t emitted = 0;
        while (walked_distance_meters_ >= config_.footstep_stride_meters &&
               emitted < config_.maximum_footsteps_per_update) {
            audio::AudioEmitterState emitter;
            emitter.position = player.position;
            emitter.velocity = listener.velocity;
            emitter.forward = listener.forward;
            auto voice = audio.play({footstep_event, emitter, 1.0F, 1.0F});
            if (!voice) {
                if (voice.error().code == "audio.voice_limit") {
                    ++stats_.dropped_footsteps;
                } else {
                    return core::Status::failure(voice.error().code, voice.error().message);
                }
            } else {
                ++stats_.emitted_footsteps;
                if (uses_surface_sound) {
                    ++stats_.surface_footsteps;
                } else {
                    ++stats_.default_footsteps;
                }
            }
            walked_distance_meters_ -= config_.footstep_stride_meters;
            ++emitted;
        }
        if (emitted == config_.maximum_footsteps_per_update) {
            walked_distance_meters_ =
                std::min(walked_distance_meters_, config_.footstep_stride_meters);
        }
    } else {
        walked_distance_meters_ =
            std::min(walked_distance_meters_, config_.footstep_stride_meters * 0.5);
    }
    return audio.update(delta_seconds);
}

core::Status ClientAudioPresentation::shutdown(audio::IAudioSystem& audio) {
    if (!initialized_) {
        return core::Status::ok();
    }
    if (ambient_voice_.has_value() && audio.is_active(*ambient_voice_)) {
        auto status = audio.stop(*ambient_voice_);
        if (!status) {
            return status;
        }
    }
    ambient_voice_.reset();
    initialized_ = false;
    return core::Status::ok();
}

const ClientAudioPresentationStats& ClientAudioPresentation::stats() const noexcept {
    return stats_;
}

core::Status ClientAudioPresentation::ensure_ambient(audio::IAudioSystem& audio) {
    if (ambient_voice_.has_value() && audio.is_active(*ambient_voice_)) {
        return core::Status::ok();
    }
    auto voice = audio.play({ambient_event_id_, std::nullopt, 1.0F, 1.0F});
    if (!voice) {
        return core::Status::failure(voice.error().code, voice.error().message);
    }
    ambient_voice_ = voice.value();
    ++stats_.ambient_starts;
    return core::Status::ok();
}

} // namespace heartstead::game
