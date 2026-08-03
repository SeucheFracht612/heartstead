#pragma once

#include "game/application/application_state.hpp"
#include "game/runtime/runtime_session.hpp"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>

namespace heartstead::game {

enum class ProcessMemoryDetail {
    // Suitable for live diagnostics. Linux uses the inexpensive, asynchronously maintained statm
    // RSS value and does not walk the process page tables.
    approximate,
    // Suitable for sparse benchmark samples. Linux replaces RSS and adds PSS/private bytes from
    // smaps_rollup, whose page-table walk is deliberately kept off the per-frame F3 path.
    precise,
};

struct ProcessResourceSample {
    std::optional<std::uint64_t> resident_memory_bytes;
    std::optional<std::uint64_t> proportional_set_size_bytes;
    std::optional<std::uint64_t> private_resident_memory_bytes;
    std::optional<std::size_t> thread_count;
    std::optional<std::size_t> open_file_count;
    bool precise_memory_accounting = false;
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
    std::size_t pending_chunk_load_operations = 0;
    std::size_t reserved_chunk_load_working_bytes = 0;
    double last_chunk_load_worker_ms = 0.0;
    double maximum_chunk_load_pipeline_latency_ms = 0.0;
    std::uint64_t maximum_chunk_load_publication_us = 0;
    bool predictive_streaming_enabled = false;
    std::size_t desired_streaming_chunks = 0;
    std::size_t active_speculative_streaming_chunks = 0;
    std::size_t deferred_streaming_evictions = 0;
    std::size_t projected_streaming_overage = 0;
    double streaming_prediction_accuracy = 0.0;
    double streaming_timely_coverage = 0.0;
    std::size_t pending_save_operations = 0;
    std::size_t reserved_save_working_bytes = 0;
    std::size_t pending_save_checkpoints = 0;
    std::size_t in_flight_save_checkpoints = 0;
    std::uint64_t save_checkpoint_retry_attempts = 0;
    std::uint64_t completed_save_checkpoints = 0;
    std::uint64_t exhausted_save_checkpoints = 0;
    std::uint64_t terminal_save_checkpoint_failures = 0;
    double last_save_owner_handoff_ms = 0.0;
    double maximum_save_owner_handoff_ms = 0.0;
    double last_save_durable_acceptance_ms = 0.0;
    double last_save_worker_ms = 0.0;
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
    // Whole-process CPU time normalized against all logical processors. A value is unavailable
    // until two refresh boundaries have been observed.
    std::optional<double> process_cpu_usage_percent;
};

struct PerformanceOverlaySnapshot {
    FrameRateSample frame_rate;
    double cpu_frame_milliseconds = 0.0;
    bool gpu_timing_valid = false;
    double gpu_frame_milliseconds = 0.0;
    std::optional<std::uint64_t> process_resident_memory_bytes;
    std::uint64_t gpu_mesh_memory_bytes = 0;
    std::uint32_t resident_chunks = 0;
    std::uint32_t visible_chunks = 0;
    // Heartstead currently exposes conservative distance/frustum-culling totals rather than a
    // distinct chunk-HZB result. The compact player-facing label remains "OCCLUDED".
    std::uint32_t occluded_chunks = 0;
};

struct PerformanceOverlayLayout {
    float panel_width = 0.0F;
    float panel_height = 0.0F;
    float margin = 0.0F;
    float horizontal_padding = 0.0F;
    float title_offset_y = 0.0F;
    float body_offset_y = 0.0F;
    float title_size = 0.0F;
    float body_size = 0.0F;
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
    std::optional<std::clock_t> last_process_cpu_clock_;
};

[[nodiscard]] ProcessResourceSample sample_process_resources(
    ProcessMemoryDetail memory_detail = ProcessMemoryDetail::approximate) noexcept;
[[nodiscard]] std::string format_runtime_diagnostics(const RuntimeDiagnosticsSnapshot& snapshot);
[[nodiscard]] std::string format_frame_rate(FrameRateSample sample);
[[nodiscard]] std::string
format_performance_overlay(const PerformanceOverlaySnapshot& snapshot);
[[nodiscard]] PerformanceOverlayLayout performance_overlay_layout(float ui_scale) noexcept;

} // namespace heartstead::game
