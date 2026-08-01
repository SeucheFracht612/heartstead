#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/renderer_stats.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer::benchmark {

enum class BenchmarkBudgetProfile : std::uint8_t {
    none,
    compatibility,
    minimum,
    mainstream,
    high_end,
};

struct BenchmarkBudget {
    double frame_interval_ms = 0.0;
    double maximum_p95_frame_ms = 0.0;
    double maximum_p99_frame_ms = 0.0;
    double maximum_frame_ms = 0.0;
    double maximum_mean_gpu_ms = 0.0;
    std::uint64_t maximum_upload_bytes_per_frame = 0;
};

struct BenchmarkBudgetViolation {
    std::string metric;
    double actual = 0.0;
    double limit = 0.0;
};

struct BenchmarkBudgetEvaluation {
    BenchmarkBudgetProfile profile = BenchmarkBudgetProfile::none;
    std::optional<BenchmarkBudget> limits;
    bool evaluated = false;
    bool passed = true;
    bool gpu_evaluated = false;
    std::vector<BenchmarkBudgetViolation> violations;
};

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
    BenchmarkBudgetProfile budget_profile = BenchmarkBudgetProfile::none;
    std::string engine_version;
    std::string git_commit;
    std::string build_configuration;
    std::string compiler;
    std::string platform;
    std::string architecture;
    std::string operating_system;
    std::string cpu_model;
    std::uint32_t logical_cpu_count = 0;
    bool git_dirty = false;
    bool tracy_enabled = false;
    std::string gpu_name;
    std::string gpu_driver;
    std::string gpu_driver_info;
    std::uint32_t gpu_vendor_id = 0;
    std::uint32_t gpu_device_id = 0;
    std::uint32_t graphics_api_version = 0;
    std::uint32_t graphics_driver_version = 0;
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
    double median_particle_update_ms = 0.0;
    double p95_particle_update_ms = 0.0;
    double median_particle_presentation_ms = 0.0;
    double p95_particle_presentation_ms = 0.0;
    std::uint32_t maximum_particle_active = 0;
    std::uint32_t maximum_particle_material_groups = 0;
    std::uint64_t final_particle_dropped = 0;
    std::uint64_t total_uploaded_bytes = 0;
    std::uint64_t maximum_uploaded_bytes = 0;
    BenchmarkBudgetEvaluation budget;
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
[[nodiscard]] std::string_view
benchmark_budget_profile_name(BenchmarkBudgetProfile profile) noexcept;
[[nodiscard]] std::optional<BenchmarkBudgetProfile>
parse_benchmark_budget_profile(std::string_view name) noexcept;
[[nodiscard]] std::optional<BenchmarkBudget>
benchmark_budget(BenchmarkBudgetProfile profile) noexcept;
[[nodiscard]] BenchmarkBudgetEvaluation evaluate_benchmark_budget(const BenchmarkSummary& summary,
                                                                  BenchmarkBudgetProfile profile);

} // namespace heartstead::renderer::benchmark
