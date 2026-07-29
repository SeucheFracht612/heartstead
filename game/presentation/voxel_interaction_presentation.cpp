#include "game/presentation/voxel_interaction_presentation.hpp"

#include <utility>

namespace heartstead::game {

core::Status VoxelInteractionPresentationConfig::validate() const {
    if (!core::PrototypeId::parse(fallback_break_particle).has_value() ||
        !core::PrototypeId::parse(fallback_break_sound).has_value() ||
        !core::PrototypeId::parse(fallback_place_sound).has_value()) {
        return core::Status::failure(
            "voxel_interaction_presentation.invalid_fallback",
            "voxel interaction fallback resources must be valid prototype ids");
    }
    if (break_particle_count == 0) {
        return core::Status::failure(
            "voxel_interaction_presentation.invalid_particle_count",
            "voxel interaction break feedback must emit at least one particle");
    }
    return core::Status::ok();
}

VoxelInteractionPresentation::VoxelInteractionPresentation(
    VoxelInteractionPresentationConfig config)
    : config_(std::move(config)),
      fallback_break_particle_(
          core::PrototypeId::parse(config_.fallback_break_particle).value_or(core::PrototypeId{})),
      fallback_break_sound_(
          core::PrototypeId::parse(config_.fallback_break_sound).value_or(core::PrototypeId{})),
      fallback_place_sound_(
          core::PrototypeId::parse(config_.fallback_place_sound).value_or(core::PrototypeId{})) {}

core::Status VoxelInteractionPresentation::present(
    std::span<const world::VoxelChangeRecord> accepted_edits,
    const world::VoxelPalette& palette, renderer::CpuParticleSystem& particles,
    audio::IAudioSystem& audio) {
    auto status = config_.validate();
    if (!status) {
        return status;
    }

    for (const auto& edit : accepted_edits) {
        const auto removed = !edit.previous.is_air() && edit.current.is_air();
        const auto placed = edit.previous.is_air() && !edit.current.is_air();
        if (!removed && !placed) {
            continue;
        }

        auto position = world::WorldPosition::from_anchor(edit.position, {0.5, 0.5, 0.5});
        if (!position) {
            return core::Status::failure(position.error().code, position.error().message);
        }

        const auto cell = removed ? edit.previous : edit.current;
        const auto* definition = palette.find_by_type(cell.type);
        if (removed) {
            const auto uses_particle_fallback =
                definition == nullptr || !definition->interaction.break_particle.has_value();
            const auto uses_sound_fallback =
                definition == nullptr || !definition->interaction.break_sound.has_value();
            const auto& particle_id =
                uses_particle_fallback ? fallback_break_particle_
                                       : *definition->interaction.break_particle;
            const auto& sound_id =
                uses_sound_fallback ? fallback_break_sound_ : *definition->interaction.break_sound;
            status = particles.queue_event(
                {particle_id, position.value(), {0.0F, 1.0F, 0.0F}, {},
                 config_.break_particle_count, particle_seed_++});
            if (!status) {
                return status;
            }
            stats_.emitted_particles += config_.break_particle_count;
            stats_.fallback_uses +=
                static_cast<std::uint64_t>(uses_particle_fallback) +
                static_cast<std::uint64_t>(uses_sound_fallback);
            status = play_sound(audio, sound_id, position.value());
            if (!status) {
                return status;
            }
            ++stats_.presented_removals;
            continue;
        }

        const auto uses_sound_fallback =
            definition == nullptr || !definition->interaction.place_sound.has_value();
        const auto& sound_id =
            uses_sound_fallback ? fallback_place_sound_ : *definition->interaction.place_sound;
        stats_.fallback_uses += static_cast<std::uint64_t>(uses_sound_fallback);
        status = play_sound(audio, sound_id, position.value());
        if (!status) {
            return status;
        }
        ++stats_.presented_placements;
    }
    return core::Status::ok();
}

const VoxelInteractionPresentationStats&
VoxelInteractionPresentation::stats() const noexcept {
    return stats_;
}

core::Status VoxelInteractionPresentation::play_sound(
    audio::IAudioSystem& audio, const core::PrototypeId& event_id,
    const world::WorldPosition& position) {
    audio::AudioEmitterState emitter;
    emitter.position = position;
    auto voice = audio.play({event_id, emitter, 1.0F, 1.0F});
    if (!voice) {
        if (voice.error().code == "audio.voice_limit") {
            ++stats_.dropped_sounds;
            return core::Status::ok();
        }
        return core::Status::failure(voice.error().code, voice.error().message);
    }
    ++stats_.emitted_sounds;
    return core::Status::ok();
}

} // namespace heartstead::game
