#pragma once

#include "engine/core/result.hpp"
#include "engine/profiling/runtime_metadata.hpp"
#include "engine/renderer/renderer_stats.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace heartstead::game {

struct GameApplicationModeTimings {
    double state_update_ms = 0.0;
    double camera_world_ms = 0.0;
    double audio_ms = 0.0;
    double ui_ms = 0.0;
};

struct GameApplicationBenchmarkConfig {
    std::uint64_t warmup_frames = 120;
    std::uint64_t measured_frames = 600;
    std::uint64_t maximum_startup_frames = 5'000;

    [[nodiscard]] core::Status validate() const;
};

struct GameApplicationBenchmarkSample {
    std::uint64_t frame_index = 0;
    std::uint64_t delta_microseconds = 0;
    double event_pump_ms = 0.0;
    double mode_update_ms = 0.0;
    double render_ms = 0.0;
    double total_frame_ms = 0.0;
    GameApplicationModeTimings mode;
    std::optional<renderer::RendererStats> renderer;
};

struct GameApplicationBenchmarkDevice {
    std::string backend;
    std::string device_name;
    std::string driver_name;
    std::string driver_info;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
};

struct GameApplicationBenchmarkReport {
    std::uint64_t ready_frame_index = 0;
    std::uint64_t warmup_frames = 0;
    bool completed = false;
    bool validation_requested = false;
    std::string present_mode;
    std::string quality_preset;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    GameApplicationBenchmarkDevice device;
    std::vector<GameApplicationBenchmarkSample> samples;
};

struct GameApplicationBenchmarkSummary {
    std::size_t sample_count = 0;
    std::size_t gpu_sample_count = 0;
    double median_frame_ms = 0.0;
    double p95_frame_ms = 0.0;
    double p99_frame_ms = 0.0;
    double maximum_frame_ms = 0.0;
    double one_percent_low_fps = 0.0;
    double point_one_percent_low_fps = 0.0;
    double mean_event_pump_ms = 0.0;
    double mean_mode_update_ms = 0.0;
    double p95_mode_update_ms = 0.0;
    double mean_state_update_ms = 0.0;
    double mean_camera_world_ms = 0.0;
    double mean_audio_ms = 0.0;
    double mean_ui_ms = 0.0;
    double mean_render_ms = 0.0;
    double p95_render_ms = 0.0;
    double mean_renderer_cpu_ms = 0.0;
    double mean_gpu_ms = 0.0;
    double mean_chunk_synchronization_ms = 0.0;
    double mean_render_extraction_ms = 0.0;
    double mean_culling_ms = 0.0;
    double mean_draw_list_ms = 0.0;
    double mean_command_build_ms = 0.0;
    double mean_command_recording_ms = 0.0;
    double mean_gpu_wait_ms = 0.0;
    double mean_gpu_opaque_terrain_ms = 0.0;
    double mean_gpu_alpha_tested_terrain_ms = 0.0;
    double mean_gpu_transparent_terrain_ms = 0.0;
    double mean_gpu_transfer_ms = 0.0;
    double mean_gpu_final_copy_ms = 0.0;
    std::uint64_t slowest_frame_index = 0;
};

struct GameApplicationBenchmarkMetadata {
    std::string workload;
    profiling::RuntimeMetadata runtime;
};

[[nodiscard]] GameApplicationBenchmarkSummary
summarize_application_benchmark(const GameApplicationBenchmarkReport& report);
[[nodiscard]] std::string
format_application_benchmark_summary(const GameApplicationBenchmarkSummary& summary);
[[nodiscard]] std::string
application_benchmark_json(const GameApplicationBenchmarkMetadata& metadata,
                           const GameApplicationBenchmarkReport& report);
[[nodiscard]] core::Status
write_application_benchmark_json(const std::filesystem::path& path,
                                 const GameApplicationBenchmarkMetadata& metadata,
                                 const GameApplicationBenchmarkReport& report);

} // namespace heartstead::game
