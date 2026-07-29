#pragma once

#include "engine/audio/audio_system.hpp"
#include "engine/core/result.hpp"
#include "engine/renderer/particles/particle_system.hpp"
#include "engine/world/voxel_change.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_set>

namespace heartstead::game {

struct VoxelInteractionPresentationConfig {
    std::string fallback_break_particle = "base:particles/block_break_puff";
    std::string fallback_break_sound = "base:audio/interaction_fallback";
    std::string fallback_place_sound = "base:audio/interaction_fallback";
    std::uint32_t break_particle_count = 18;

    [[nodiscard]] core::Status validate() const;
};

struct VoxelInteractionPresentationStats {
    std::uint64_t presented_removals = 0;
    std::uint64_t presented_placements = 0;
    std::uint64_t emitted_particles = 0;
    std::uint64_t emitted_sounds = 0;
    std::uint64_t dropped_sounds = 0;
    std::uint64_t fallback_uses = 0;
    std::uint64_t fallback_diagnostics = 0;
};

class VoxelInteractionPresentation {
  public:
    explicit VoxelInteractionPresentation(VoxelInteractionPresentationConfig config = {});

    [[nodiscard]] core::Status present(std::span<const world::VoxelChangeRecord> accepted_edits,
                                       const world::VoxelPalette& palette,
                                       renderer::CpuParticleSystem& particles,
                                       audio::IAudioSystem& audio);
    [[nodiscard]] const VoxelInteractionPresentationStats& stats() const noexcept;

  private:
    [[nodiscard]] core::Status play_sound(audio::IAudioSystem& audio,
                                          const core::PrototypeId& event_id,
                                          const world::WorldPosition& position);
    void report_fallback(const world::VoxelDefinition* definition, std::uint16_t voxel_type,
                         std::string_view role, const core::PrototypeId& fallback);

    VoxelInteractionPresentationConfig config_;
    core::PrototypeId fallback_break_particle_;
    core::PrototypeId fallback_break_sound_;
    core::PrototypeId fallback_place_sound_;
    std::unordered_set<std::string> reported_fallbacks_;
    std::uint64_t particle_seed_ = 1;
    VoxelInteractionPresentationStats stats_{};
};

} // namespace heartstead::game
