#include "engine/audio/audio_mixer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace heartstead::audio {

namespace {

constexpr float pi = 3.14159265358979323846F;

[[nodiscard]] bool non_negative_finite(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F;
}

[[nodiscard]] math::Vec3f normalize_or(math::Vec3f value, math::Vec3f fallback) noexcept {
    const auto length = static_cast<float>(math::length(value));
    return std::isfinite(length) && length > 0.000001F ? value / length : fallback;
}

[[nodiscard]] float smooth_attenuation(float distance, float minimum, float maximum) noexcept {
    if (distance <= minimum) {
        return 1.0F;
    }
    if (distance >= maximum) {
        return 0.0F;
    }
    const auto t = (distance - minimum) / (maximum - minimum);
    return 1.0F - (t * t * (3.0F - 2.0F * t));
}

[[nodiscard]] float cone_attenuation(const SoundEventDefinition& event,
                                     const AudioEmitterState& emitter,
                                     math::Vec3f emitter_to_listener) noexcept {
    if (event.cone_outer_angle_degrees >= 360.0F) {
        return 1.0F;
    }
    const auto forward = normalize_or(emitter.forward, {0.0F, 0.0F, -1.0F});
    const auto direction = normalize_or(emitter_to_listener, forward);
    const auto cosine = std::clamp(math::dot(forward, direction), -1.0F, 1.0F);
    const auto angle = std::acos(cosine) * (180.0F / pi);
    const auto inner_half = event.cone_inner_angle_degrees * 0.5F;
    const auto outer_half = event.cone_outer_angle_degrees * 0.5F;
    if (angle <= inner_half) {
        return 1.0F;
    }
    if (angle >= outer_half || outer_half <= inner_half) {
        return event.cone_outer_gain;
    }
    const auto t = (angle - inner_half) / (outer_half - inner_half);
    return 1.0F + ((event.cone_outer_gain - 1.0F) * t);
}

} // namespace

core::Status AudioMixerConfig::validate() const {
    if (maximum_voices == 0) {
        return core::Status::failure("audio_mixer.invalid_voice_limit",
                                     "audio mixer voice limit must be non-zero");
    }
    if (!std::isfinite(gain_ramp_seconds) || gain_ramp_seconds < 0.0F) {
        return core::Status::failure(
            "audio_mixer.invalid_gain_ramp",
            "audio mixer gain ramp duration must be finite and non-negative");
    }
    return core::Status::ok();
}

AudioMixer::AudioMixer(const SoundEventRegistry& events, AudioMixerConfig config)
    : events_(&events), config_(config) {
    stats_.maximum_voices = config.maximum_voices;
    stats_.master_gain = 1.0F;
}

core::Result<AudioVoiceId> AudioMixer::play(const AudioPlayRequest& request) {
    const auto* event = events_->find(request.event_id);
    if (event == nullptr) {
        ++stats_.rejected_voices;
        return core::Result<AudioVoiceId>::failure("audio.event_missing",
                                                   "audio event prototype is not registered");
    }
    if (!non_negative_finite(request.gain) || !std::isfinite(request.pitch) ||
        request.pitch <= 0.0F) {
        ++stats_.rejected_voices;
        return core::Result<AudioVoiceId>::failure(
            "audio.invalid_play_request",
            "audio play gain must be non-negative and pitch must be positive and finite");
    }
    if (event->spatialized && !request.emitter.has_value()) {
        ++stats_.rejected_voices;
        return core::Result<AudioVoiceId>::failure("audio.emitter_required",
                                                   "spatial audio events require an emitter");
    }
    if (request.emitter.has_value() &&
        (!request.emitter->position.is_valid() || !request.emitter->velocity.is_finite() ||
         !request.emitter->forward.is_finite())) {
        ++stats_.rejected_voices;
        return core::Result<AudioVoiceId>::failure("audio.invalid_emitter",
                                                   "audio emitter state is invalid");
    }

    const auto steal = voice_to_steal(*event);
    const auto event_count = static_cast<std::uint32_t>(std::ranges::count_if(
        voices_, [event](const Voice& voice) { return voice.event == event; }));
    if (voices_.size() >= config_.maximum_voices || event_count >= event->maximum_instances) {
        if (!steal.has_value() || voices_[*steal].event->priority > event->priority) {
            ++stats_.rejected_voices;
            return core::Result<AudioVoiceId>::failure(
                "audio.voice_limit", "audio voice was rejected by instance/priority policy");
        }
        erase_voice(*steal, true);
    }
    if (next_voice_id_ == 0 || next_sequence_ == 0) {
        ++stats_.rejected_voices;
        return core::Result<AudioVoiceId>::failure("audio.id_exhausted",
                                                   "audio voice id space is exhausted");
    }

    Voice voice;
    voice.id = AudioVoiceId::from_value(next_voice_id_++);
    voice.event = event;
    voice.emitter = request.emitter;
    voice.request_gain = request.gain;
    voice.pitch = request.pitch;
    voice.sequence = next_sequence_++;
    voice.spatial = spatial_parameters(voice);
    by_id_.emplace(voice.id.value(), voices_.size());
    voices_.push_back(std::move(voice));
    ++stats_.played_voices;
    stats_.active_voices = static_cast<std::uint32_t>(voices_.size());
    return core::Result<AudioVoiceId>::success(voices_.back().id);
}

core::Status AudioMixer::stop(AudioVoiceId voice) {
    const auto found = by_id_.find(voice.value());
    if (!voice.is_valid() || found == by_id_.end()) {
        return core::Status::failure("audio.voice_missing", "audio voice is not active");
    }
    erase_voice(found->second, false);
    return core::Status::ok();
}

core::Status AudioMixer::mark_finished(AudioVoiceId voice) {
    return stop(voice);
}

core::Status AudioMixer::set_emitter(AudioVoiceId voice, AudioEmitterState emitter) {
    if (!emitter.position.is_valid() || !emitter.velocity.is_finite() ||
        !emitter.forward.is_finite()) {
        return core::Status::failure("audio.invalid_emitter", "audio emitter state is invalid");
    }
    const auto found = by_id_.find(voice.value());
    if (!voice.is_valid() || found == by_id_.end()) {
        return core::Status::failure("audio.voice_missing", "audio voice is not active");
    }
    voices_[found->second].emitter = emitter;
    voices_[found->second].spatial = spatial_parameters(voices_[found->second]);
    return core::Status::ok();
}

core::Status AudioMixer::set_listener(AudioListenerState listener) {
    auto status = validate_listener(listener);
    if (!status) {
        return status;
    }
    listener.forward = normalize_or(listener.forward, {0.0F, 0.0F, -1.0F});
    listener.up = normalize_or(listener.up, {0.0F, 1.0F, 0.0F});
    listener_ = listener;
    for (auto& voice : voices_) {
        voice.spatial = spatial_parameters(voice);
    }
    return core::Status::ok();
}

core::Status AudioMixer::set_bus_gain(AudioBus bus, float gain) {
    if (!non_negative_finite(gain)) {
        return core::Status::failure("audio.invalid_bus_gain",
                                     "audio bus gain must be finite and non-negative");
    }
    target_bus_gains_[bus_index(bus)] = gain;
    return core::Status::ok();
}

float AudioMixer::bus_gain(AudioBus bus) const noexcept {
    return bus_gains_[bus_index(bus)];
}

float AudioMixer::target_bus_gain(AudioBus bus) const noexcept {
    return target_bus_gains_[bus_index(bus)];
}

core::Status AudioMixer::advance(float delta_seconds) {
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0F) {
        return core::Status::failure("audio.invalid_delta",
                                     "audio delta time must be finite and non-negative");
    }
    const auto amount = config_.gain_ramp_seconds <= 0.0F
                            ? 1.0F
                            : std::clamp(delta_seconds / config_.gain_ramp_seconds, 0.0F, 1.0F);
    for (std::size_t index = 0; index < bus_gains_.size(); ++index) {
        bus_gains_[index] += (target_bus_gains_[index] - bus_gains_[index]) * amount;
    }
    stats_.master_gain = bus_gains_[bus_index(AudioBus::master)];
    for (auto& voice : voices_) {
        voice.spatial = spatial_parameters(voice);
    }
    return core::Status::ok();
}

bool AudioMixer::is_active(AudioVoiceId voice) const noexcept {
    return voice.is_valid() && by_id_.contains(voice.value());
}

std::optional<AudioVoiceSnapshot> AudioMixer::snapshot(AudioVoiceId voice) const {
    const auto found = by_id_.find(voice.value());
    if (!voice.is_valid() || found == by_id_.end()) {
        return std::nullopt;
    }
    const auto& value = voices_[found->second];
    return AudioVoiceSnapshot{value.id,
                              value.event->prototype_id,
                              value.event->bus,
                              value.spatial,
                              value.pitch,
                              value.event->priority,
                              value.event->looping,
                              value.event->streaming,
                              value.event->spatialized};
}

std::vector<AudioVoiceSnapshot> AudioMixer::snapshots() const {
    std::vector<AudioVoiceSnapshot> result;
    result.reserve(voices_.size());
    for (const auto& voice : voices_) {
        result.push_back(AudioVoiceSnapshot{voice.id, voice.event->prototype_id, voice.event->bus,
                                            voice.spatial, voice.pitch, voice.event->priority,
                                            voice.event->looping, voice.event->streaming,
                                            voice.event->spatialized});
    }
    return result;
}

const AudioSystemStats& AudioMixer::stats() const noexcept {
    return stats_;
}

const AudioListenerState& AudioMixer::listener() const noexcept {
    return listener_;
}

core::Status AudioMixer::validate_listener(const AudioListenerState& listener) const {
    if (!listener.position.is_valid() || !listener.velocity.is_finite() ||
        !listener.forward.is_finite() || !listener.up.is_finite() ||
        math::length_squared(listener.forward) <= 0.000001F ||
        math::length_squared(listener.up) <= 0.000001F ||
        math::length_squared(math::cross(listener.forward, listener.up)) <= 0.000001F) {
        return core::Status::failure(
            "audio.invalid_listener",
            "audio listener requires a valid position and finite non-collinear orientation");
    }
    return core::Status::ok();
}

std::optional<std::size_t> AudioMixer::voice_to_steal(const SoundEventDefinition& event) const {
    std::optional<std::size_t> same_event;
    std::optional<std::size_t> global;
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        const auto better_candidate = [this, index](std::optional<std::size_t> current) {
            return !current.has_value() ||
                   voices_[index].event->priority < voices_[*current].event->priority ||
                   (voices_[index].event->priority == voices_[*current].event->priority &&
                    voices_[index].sequence < voices_[*current].sequence);
        };
        if (better_candidate(global)) {
            global = index;
        }
        if (voices_[index].event == &event && better_candidate(same_event)) {
            same_event = index;
        }
    }
    const auto event_count = static_cast<std::uint32_t>(std::ranges::count_if(
        voices_, [&event](const Voice& voice) { return voice.event == &event; }));
    return event_count >= event.maximum_instances ? same_event : global;
}

AudioSpatialParameters AudioMixer::spatial_parameters(const Voice& voice) const {
    const auto master = bus_gains_[bus_index(AudioBus::master)];
    const auto bus =
        voice.event->bus == AudioBus::master ? 1.0F : bus_gains_[bus_index(voice.event->bus)];
    auto gain = master * bus * voice.event->gain * voice.request_gain;
    AudioSpatialParameters result;
    if (voice.event->spatialized && voice.emitter.has_value()) {
        const auto emitter_relative =
            voice.emitter->position.relative_to(listener_.position.anchor);
        const auto listener_local = listener_.position.local_offset;
        const math::Vec3f delta{
            static_cast<float>(emitter_relative.x - listener_local.x),
            static_cast<float>(emitter_relative.y - listener_local.y),
            static_cast<float>(emitter_relative.z - listener_local.z),
        };
        result.distance = static_cast<float>(math::length(delta));
        gain *= smooth_attenuation(result.distance, voice.event->minimum_distance,
                                   voice.event->maximum_distance);
        gain *= cone_attenuation(*voice.event, *voice.emitter, delta * -1.0F);
        const auto right =
            normalize_or(math::cross(listener_.forward, listener_.up), {1.0F, 0.0F, 0.0F});
        const auto horizontal = math::Vec3f{delta.x, 0.0F, delta.z};
        result.pan = math::length_squared(horizontal) <= 0.000001F
                         ? 0.0F
                         : std::clamp(math::dot(normalize_or(horizontal, {}), right), -1.0F, 1.0F);
    }
    result.gain = gain;
    result.left_gain = gain * std::sqrt(0.5F * (1.0F - result.pan));
    result.right_gain = gain * std::sqrt(0.5F * (1.0F + result.pan));
    return result;
}

std::size_t AudioMixer::bus_index(AudioBus bus) const noexcept {
    return static_cast<std::size_t>(bus);
}

void AudioMixer::erase_voice(std::size_t index, bool stolen) {
    const auto erased_id = voices_[index].id.value();
    const auto last = voices_.size() - 1;
    if (index != last) {
        voices_[index] = std::move(voices_[last]);
        by_id_[voices_[index].id.value()] = index;
    }
    voices_.pop_back();
    by_id_.erase(erased_id);
    ++stats_.stopped_voices;
    if (stolen) {
        ++stats_.stolen_voices;
    }
    stats_.active_voices = static_cast<std::uint32_t>(voices_.size());
}

} // namespace heartstead::audio
