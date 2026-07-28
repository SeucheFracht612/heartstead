#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/renderer_stats.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer::benchmark {

struct BenchmarkRunMetadata {
    std::string scene;
    std::uint64_t seed = 0;
    std::string backend = "headless";
    std::string mesher = "greedy";
    std::uint32_t initial_width = 0;
    std::uint32_t initial_height = 0;
    std::uint32_t chunk_radius = 0;
    std::uint64_t warmup_frames = 0;
    std::uint64_t measured_frames = 0;
    std::uint32_t frame_cap = 0;
    bool validation_requested = false;
};

struct BenchmarkSummary {
    std::string scene;
    std::uint64_t seed = 0;
    std::size_t sample_count = 0;
    std::size_t gpu_sample_count = 0;
    std::size_t gpu_upload_sample_count = 0;

    double median_frame_ms = 0.0;
    double p95_frame_ms = 0.0;
    double p99_frame_ms = 0.0;
    double one_percent_low_fps = 0.0;
    double point_one_percent_low_fps = 0.0;
    double maximum_frame_ms = 0.0;

    double mean_cpu_frame_ms = 0.0;
    double mean_gpu_frame_ms = 0.0;
    double mean_gpu_upload_ms = 0.0;
    double mean_render_extraction_ms = 0.0;
    double mean_chunk_synchronization_ms = 0.0;
    double mean_culling_ms = 0.0;
    double mean_draw_list_ms = 0.0;
    double mean_command_build_ms = 0.0;
    double mean_command_recording_ms = 0.0;
    double mean_chunk_snapshot_ms = 0.0;
    double mean_meshing_ms = 0.0;
    double mean_upload_preparation_ms = 0.0;
    double mean_upload_ms = 0.0;
    double mean_gpu_wait_ms = 0.0;
    double mean_gpu_opaque_terrain_ms = 0.0;
    double mean_gpu_alpha_tested_terrain_ms = 0.0;
    double mean_gpu_transparent_terrain_ms = 0.0;
    double mean_gpu_transfer_ms = 0.0;
    double mean_gpu_final_copy_ms = 0.0;
    double median_voxel_relight_solve_ms = 0.0;
    double p95_voxel_relight_solve_ms = 0.0;
    double median_voxel_relight_apply_ms = 0.0;
    double p95_voxel_relight_apply_ms = 0.0;
    std::uint64_t maximum_voxel_relight_backlog_cells = 0;
    std::uint64_t maximum_voxel_relight_visited_cells = 0;
    std::uint64_t total_voxel_relight_changed_chunks = 0;
    std::uint64_t final_voxel_relight_stale_results = 0;
    std::uint64_t final_voxel_relight_apply_budget_overruns = 0;
    double median_voxel_fluid_snapshot_ms = 0.0;
    double p95_voxel_fluid_snapshot_ms = 0.0;
    double median_voxel_fluid_simulation_ms = 0.0;
    double p95_voxel_fluid_simulation_ms = 0.0;
    double median_voxel_fluid_apply_ms = 0.0;
    double p95_voxel_fluid_apply_ms = 0.0;
    std::uint64_t maximum_voxel_fluid_active_cells = 0;
    std::uint64_t maximum_voxel_fluid_processed_cells = 0;
    std::uint64_t total_voxel_fluid_changed_chunks = 0;
    std::uint64_t final_voxel_fluid_budget_exhaustions = 0;
    std::uint64_t final_voxel_fluid_apply_budget_overruns = 0;
    std::uint64_t total_uploaded_bytes = 0;
    RendererStats slowest_frame{};
};

class BenchmarkRecorder {
  public:
    BenchmarkRecorder(std::string scene, std::uint64_t seed);
    explicit BenchmarkRecorder(BenchmarkRunMetadata metadata);

    void record(RendererStats stats);
    void clear() noexcept;

    [[nodiscard]] const std::vector<RendererStats>& samples() const noexcept;
    [[nodiscard]] const BenchmarkRunMetadata& metadata() const noexcept;
    [[nodiscard]] BenchmarkSummary summarize() const;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] std::string to_csv() const;
    [[nodiscard]] core::Status write_json(const std::filesystem::path& path) const;
    [[nodiscard]] core::Status write_csv(const std::filesystem::path& path) const;

  private:
    BenchmarkRunMetadata metadata_;
    std::vector<RendererStats> samples_;
};

[[nodiscard]] std::string format_benchmark_summary(const BenchmarkSummary& summary);

} // namespace heartstead::renderer::benchmark
