#include "engine/audio/audio_system.hpp"
#include "engine/audio/sound_event.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/core/logging.hpp"
#include "engine/renderer/particles/particle_system.hpp"
#include "game/presentation/voxel_interaction_presentation.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace heartstead;

[[nodiscard]] core::PrototypeId id(std::string_view value) {
    return core::PrototypeId::parse(value).value();
}

[[nodiscard]] audio::SoundEventDefinition sound(std::string_view value) {
    audio::SoundEventDefinition result;
    result.prototype_id = id(value);
    result.asset_id = "test:sounds/feedback.tone";
    result.spatialized = true;
    return result;
}

[[nodiscard]] renderer::ParticlePrototype particle(std::string_view value) {
    renderer::ParticlePrototype result;
    result.id = id(value);
    result.lifetime_min_seconds = 1.0F;
    result.lifetime_max_seconds = 1.0F;
    result.speed_max = 0.0F;
    result.gravity = 0.0F;
    return result;
}

[[nodiscard]] world::VoxelDefinition voxel(std::uint16_t type, std::string_view value) {
    world::VoxelDefinition result;
    result.type = type;
    result.prototype_id = id(value);
    result.display_name = std::string(value);
    result.terrain_material = "test";
    result.mining_tool = "hand";
    return result;
}

void test_only_accepted_edits_emit_data_driven_feedback() {
    const auto specific_particle = id("test:particles/specific");
    const auto fallback_particle = id("test:particles/fallback");
    const auto break_sound = id("test:audio/break");
    const auto place_sound = id("test:audio/place");
    const auto fallback_sound = id("test:audio/fallback");

    audio::SoundEventRegistry events;
    assert(events.add(sound(break_sound.value())));
    assert(events.add(sound(place_sound.value())));
    assert(events.add(sound(fallback_sound.value())));
    audio::AudioSystemDesc audio_desc;
    audio_desc.events = &events;
    auto audio_system = audio::create_audio_system(audio_desc);
    assert(audio_system);

    const std::vector particle_prototypes{particle(specific_particle.value()),
                                          particle(fallback_particle.value())};
    renderer::ParticleSystemConfig particle_config;
    particle_config.maximum_particles = 128;
    particle_config.maximum_emitters = 1;
    particle_config.maximum_queued_events = 8;
    particle_config.maximum_spawns_per_update = 128;
    auto particles = renderer::CpuParticleSystem::create(particle_config, particle_prototypes);
    assert(particles);

    world::VoxelPalette palette;
    auto configured = voxel(1, "test:voxels/configured");
    configured.interaction.break_particle = specific_particle;
    configured.interaction.break_sound = break_sound;
    configured.interaction.place_sound = place_sound;
    assert(palette.add(configured));
    assert(palette.add(voxel(2, "test:voxels/legacy")));

    game::VoxelInteractionPresentationConfig config;
    config.fallback_break_particle = fallback_particle.value();
    config.fallback_break_sound = fallback_sound.value();
    config.fallback_place_sound = fallback_sound.value();
    game::VoxelInteractionPresentation presentation(config);

    const world::VoxelCell configured_cell{1, 0, 0, 0};
    const world::VoxelCell legacy_cell{2, 0, 0, 0};
    const world::VoxelCell air{};
    const std::vector<world::VoxelChangeRecord> accepted{
        {{1, 2, 3}, configured_cell, air, {}, 1},
        {{2, 2, 3}, air, configured_cell, {}, 1},
    };
    assert(presentation.present(accepted, palette, particles.value(), *audio_system.value()));
    assert(particles.value().stats().queued_events == 1);
    assert(audio_system.value()->stats().played_voices == 2);
    assert(presentation.stats().presented_removals == 1);
    assert(presentation.stats().presented_placements == 1);
    assert(presentation.stats().fallback_uses == 0);

    const std::vector<world::VoxelChangeRecord> rejected;
    assert(presentation.present(rejected, palette, particles.value(), *audio_system.value()));
    assert(particles.value().stats().queued_events == 1);
    assert(audio_system.value()->stats().played_voices == 2);

    const std::vector<world::VoxelChangeRecord> legacy_removal{
        {{3, 2, 3}, legacy_cell, air, {}, 1},
    };
    std::vector<std::string> warnings;
    core::set_log_sink([&](core::LogLevel level, std::string_view message) {
        if (level == core::LogLevel::warning) {
            warnings.emplace_back(message);
        }
    });
    assert(presentation.present(legacy_removal, palette, particles.value(), *audio_system.value()));
    assert(particles.value().stats().queued_events == 2);
    assert(audio_system.value()->stats().played_voices == 3);
    assert(presentation.stats().fallback_uses == 2);
    assert(presentation.stats().fallback_diagnostics == 2);
    assert(presentation.present(legacy_removal, palette, particles.value(), *audio_system.value()));
    core::reset_log_sink();
    assert(warnings.size() == 2);
    assert(std::ranges::all_of(warnings, [](const std::string& warning) {
        return warning.find("test:voxels/legacy") != std::string::npos;
    }));
    assert(presentation.stats().fallback_uses == 4);
    assert(presentation.stats().fallback_diagnostics == 2);
    assert(particles.value().update(1.0F / 60.0F));
    assert(particles.value().stats().active_particles == 54);
}

void test_foundation_voxels_resolve_feedback_resources() {
    const auto report =
        content::ContentValidation::validate(std::filesystem::path(HEARTSTEAD_TEST_SOURCE_DIR));
    assert(!report.has_errors());
    for (const auto* definition : report.voxel_palette.definitions()) {
        assert(definition->interaction.break_particle.has_value());
        assert(definition->interaction.break_sound.has_value());
        assert(definition->interaction.place_sound.has_value());
        assert(report.sound_events.find(*definition->interaction.break_sound) != nullptr);
        assert(report.sound_events.find(*definition->interaction.place_sound) != nullptr);
        if (!definition->collision_bounds.empty()) {
            assert(definition->interaction.footstep_sound.has_value());
            assert(report.sound_events.find(*definition->interaction.footstep_sound) != nullptr);
        }
    }
}

} // namespace

int main() {
    test_only_accepted_edits_emit_data_driven_feedback();
    test_foundation_voxels_resolve_feedback_resources();
    return 0;
}
