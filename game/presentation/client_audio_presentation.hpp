#pragma once

#include "engine/audio/audio_system.hpp"
#include "engine/core/result.hpp"
#include "engine/movement/player_camera.hpp"
#include "engine/movement/player_controller.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace heartstead::game {

struct ClientAudioPresentationConfig {
    std::string default_footstep_event_id = "base:audio/earth_footstep";
    std::string ambient_event_id = "base:audio/homestead_ambient";
    double footstep_stride_meters = 1.65;
    double minimum_footstep_speed = 0.6;
    std::uint32_t maximum_footsteps_per_update = 4;

    [[nodiscard]] core::Status validate() const;
};

struct ClientAudioPresentationStats {
    std::uint64_t emitted_footsteps = 0;
    std::uint64_t dropped_footsteps = 0;
    std::uint64_t surface_footsteps = 0;
    std::uint64_t default_footsteps = 0;
    std::uint64_t ambient_starts = 0;
};

class ClientAudioPresentation {
  public:
    explicit ClientAudioPresentation(ClientAudioPresentationConfig config = {});

    [[nodiscard]] core::Status initialize(audio::IAudioSystem& audio);
    [[nodiscard]] core::Status update(audio::IAudioSystem& audio,
                                      const movement::PlayerControllerState& player,
                                      const movement::PlayerCameraFrame& camera,
                                      const world::ChunkDatabase& chunks,
                                      const world::VoxelPalette& palette, float delta_seconds);
    [[nodiscard]] core::Status shutdown(audio::IAudioSystem& audio);
    [[nodiscard]] const ClientAudioPresentationStats& stats() const noexcept;

  private:
    [[nodiscard]] core::Status ensure_ambient(audio::IAudioSystem& audio);

    ClientAudioPresentationConfig config_{};
    core::PrototypeId default_footstep_event_id_;
    core::PrototypeId ambient_event_id_;
    std::optional<audio::AudioVoiceId> ambient_voice_;
    double walked_distance_meters_ = 0.0;
    ClientAudioPresentationStats stats_{};
    bool initialized_ = false;
};

} // namespace heartstead::game
