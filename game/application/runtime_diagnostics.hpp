#pragma once

#include "game/application/application_state.hpp"
#include "game/runtime/runtime_session.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace heartstead::game {

struct ProcessResourceSample {
    std::optional<std::uint64_t> resident_memory_bytes;
    std::optional<std::size_t> thread_count;
    std::optional<std::size_t> open_file_count;
};

struct RuntimeDiagnosticsSnapshot {
    ApplicationState application_state = ApplicationState::boot;
    std::optional<RuntimeSessionState> session_state;
    std::optional<SessionMode> session_mode;
    std::optional<SessionConnectionState> connection_state;
    std::optional<SessionStartupPhase> loading_phase;
    std::string active_world;
    std::string save_destination;
    std::uint64_t session_generation = 0;
    std::uint64_t authoritative_tick = 0;
    std::uint64_t fixed_step_tick = 0;
    std::uint64_t dropped_tick_time_us = 0;
    double interpolation_alpha = 0.0;
    std::size_t active_jobs = 0;
    std::size_t pending_loading_operations = 0;
    std::size_t active_network_connections = 0;
    std::size_t registered_session_callbacks = 0;
    std::size_t world_entities = 0;
    std::size_t physics_objects = 0;
    std::size_t presentation_objects = 0;
    std::size_t render_objects = 0;
    std::size_t audio_emitters = 0;
    std::size_t asset_references = 0;
    std::uint64_t resident_gpu_bytes = 0;
    std::optional<std::uint64_t> device_gpu_usage_bytes;
    std::optional<std::uint64_t> device_gpu_budget_bytes;
    ProcessResourceSample process;
};

struct FrameRateSample {
    double frames_per_second = 0.0;
    double frame_time_milliseconds = 0.0;
};

class FrameRateCounter {
  public:
    void record_frame(std::uint64_t delta_microseconds) noexcept;
    void reset() noexcept;
    [[nodiscard]] FrameRateSample sample() const noexcept;

  private:
    static constexpr std::uint64_t refresh_interval_microseconds = 250'000;
    std::uint64_t accumulated_microseconds_ = 0;
    std::uint64_t accumulated_frames_ = 0;
    FrameRateSample sample_{};
};

[[nodiscard]] ProcessResourceSample sample_process_resources() noexcept;
[[nodiscard]] std::string format_runtime_diagnostics(const RuntimeDiagnosticsSnapshot& snapshot);
[[nodiscard]] std::string format_frame_rate(FrameRateSample sample);

} // namespace heartstead::game
