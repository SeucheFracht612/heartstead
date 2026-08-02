#pragma once

#include "engine/core/result.hpp"
#include "engine/profiling/runtime_metadata.hpp"
#include "engine/renderer/chunks/chunk_render_system.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/terrain/far_terrain_renderer.hpp"
#include "engine/world/coords/world_position.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer::benchmark {

[[nodiscard]] ChunkRenderConfig terrain_edit_transition_near_defaults() noexcept;
[[nodiscard]] FarTerrainRendererConfig terrain_edit_transition_far_defaults() noexcept;

struct TerrainEditTransitionBenchmarkConfig {
    rhi::RenderBackend render_backend = rhi::RenderBackend::headless;
    std::uint16_t world_radius_chunks = 4;
    std::uint32_t warmup_repetitions = 2;
    std::uint32_t repetitions = 9;
    std::uint64_t update_interval_us = 16'667;
    std::uint64_t timeout_ms = 5'000;
    ChunkRenderConfig near_rendering = terrain_edit_transition_near_defaults();
    FarTerrainRendererConfig far_rendering = terrain_edit_transition_far_defaults();
    bool enforce_gates = false;
    double maximum_near_draw_p95_ms = 50.0;
    double maximum_mid_convergence_p95_ms = 250.0;
    double maximum_far_convergence_p95_ms = 500.0;
    double maximum_full_convergence_p95_ms = 500.0;
    double maximum_owner_update_ms = 8.0;
    double maximum_upload_preparation_ms = 0.5;
    double maximum_synchronous_gpu_wait_ms = 0.0;

    [[nodiscard]] core::Status validate() const;
};

struct TerrainEditTransitionDeviceMetadata {
    std::string backend;
    std::string device_name;
    std::string driver_name;
    std::string driver_info;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
    bool headless = true;
};

struct TerrainEditTransitionBenchmarkSample {
    std::uint32_t repetition = 0;
    world::ChunkCoord coord;
    world::VoxelCoord voxel;
    std::uint64_t target_content_revision = 0;
    std::uint64_t target_surface_revision = 0;
    std::uint64_t owner_updates = 0;
    std::uint64_t near_draw_current_us = 0;
    std::uint64_t first_mid_publication_us = 0;
    std::uint64_t mid_convergence_us = 0;
    std::uint64_t first_far_publication_us = 0;
    std::uint64_t far_convergence_us = 0;
    std::uint64_t full_convergence_us = 0;
    double instrumented_near_edit_to_visible_ms = 0.0;
    double maximum_owner_update_ms = 0.0;
    double maximum_near_snapshot_ms = 0.0;
    double maximum_near_meshing_ms = 0.0;
    double maximum_near_upload_preparation_ms = 0.0;
    double maximum_near_upload_ms = 0.0;
    double maximum_synchronous_gpu_wait_ms = 0.0;
    double maximum_far_worker_meshing_ms = 0.0;
    std::size_t maximum_near_pending_meshes = 0;
    std::size_t maximum_near_in_flight_meshes = 0;
    std::size_t maximum_near_pending_uploads = 0;
    std::size_t maximum_far_ready_meshes = 0;
    std::size_t maximum_far_in_flight_meshes = 0;
    std::size_t maximum_far_completed_mailbox = 0;
    std::size_t maximum_far_pipeline_occupancy = 0;
    std::size_t minimum_near_resident_chunks = 0;
    std::size_t minimum_far_resident_patches = 0;
    std::size_t minimum_near_draw_commands = 0;
    std::size_t minimum_far_draw_commands = 0;
    std::size_t maximum_near_uploaded_bytes_per_update = 0;
    std::size_t maximum_far_uploaded_bytes_per_update = 0;
    std::uint64_t invalidated_far_patches = 0;
    std::uint64_t rebuilt_mid_patches = 0;
    std::uint64_t rebuilt_far_patches = 0;
    std::uint64_t near_completed_mesh_jobs = 0;
    std::uint64_t near_built_meshes = 0;
    std::uint64_t near_published_meshes = 0;
    std::uint64_t near_stale_mesh_results = 0;
    std::uint64_t near_cancelled_mesh_results = 0;
    std::uint64_t far_submitted_mesh_jobs = 0;
    std::uint64_t far_completed_mesh_jobs = 0;
    std::uint64_t far_cancelled_mesh_jobs = 0;
    std::uint64_t far_stale_results = 0;
};

struct TerrainEditTransitionBenchmarkRun {
    std::uint32_t repetition = 0;
    std::size_t loaded_chunks = 0;
    std::size_t initial_near_resident_chunks = 0;
    std::size_t initial_far_resident_patches = 0;
    std::size_t initial_near_draw_commands = 0;
    std::size_t initial_far_draw_commands = 0;
    std::uint64_t initial_owner_updates = 0;
    std::uint64_t initial_settlement_us = 0;
    std::uint64_t supersession_owner_updates = 0;
    std::uint64_t supersession_convergence_us = 0;
    std::uint64_t supersession_near_coalesced_invalidations = 0;
    std::uint64_t supersession_near_obsolete_results = 0;
    std::uint64_t supersession_far_coalesced_invalidations = 0;
    std::uint64_t supersession_far_stale_results = 0;
    std::size_t supersession_minimum_near_draw_commands = 0;
    std::size_t supersession_minimum_far_draw_commands = 0;
    std::size_t supersession_minimum_near_resident_chunks = 0;
    std::size_t supersession_minimum_far_resident_patches = 0;
    std::size_t baseline_live_render_resources = 0;
    std::size_t final_live_render_resources = 0;
};

struct TerrainEditTransitionBenchmarkViolation {
    std::string metric;
    double actual = 0.0;
    double limit = 0.0;
};

struct TerrainEditTransitionBenchmarkGateEvaluation {
    bool evaluated = false;
    bool passed = true;
    std::vector<TerrainEditTransitionBenchmarkViolation> violations;
};

struct TerrainEditTransitionBenchmarkSummary {
    std::size_t run_count = 0;
    std::size_t sample_count = 0;
    double median_near_draw_ms = 0.0;
    double p95_near_draw_ms = 0.0;
    double p99_near_draw_ms = 0.0;
    double median_mid_convergence_ms = 0.0;
    double p95_mid_convergence_ms = 0.0;
    double p99_mid_convergence_ms = 0.0;
    double median_far_convergence_ms = 0.0;
    double p95_far_convergence_ms = 0.0;
    double p99_far_convergence_ms = 0.0;
    double median_full_convergence_ms = 0.0;
    double p95_full_convergence_ms = 0.0;
    double p99_full_convergence_ms = 0.0;
    double maximum_owner_update_ms = 0.0;
    double maximum_upload_preparation_ms = 0.0;
    double maximum_synchronous_gpu_wait_ms = 0.0;
    std::size_t maximum_far_pipeline_occupancy = 0;
    std::uint64_t total_invalidated_far_patches = 0;
    std::uint64_t total_rebuilt_mid_patches = 0;
    std::uint64_t total_rebuilt_far_patches = 0;
    std::uint64_t total_near_stale_or_cancelled_results = 0;
    std::uint64_t total_far_stale_or_cancelled_results = 0;
    TerrainEditTransitionBenchmarkGateEvaluation gates;
};

struct TerrainEditTransitionBenchmarkReport {
    static constexpr std::uint32_t schema_version = 1;
    static constexpr std::string_view near_measurement_endpoint =
        "first_exact_current_chunk_draw_command";
    static constexpr std::string_view mid_measurement_endpoint =
        "all_invalidated_mid_patches_current";
    static constexpr std::string_view far_measurement_endpoint =
        "all_invalidated_far_patches_current";

    TerrainEditTransitionBenchmarkConfig config;
    profiling::RuntimeMetadata runtime;
    TerrainEditTransitionDeviceMetadata device;
    std::vector<TerrainEditTransitionBenchmarkRun> runs;
    std::vector<TerrainEditTransitionBenchmarkSample> raw_samples;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] TerrainEditTransitionBenchmarkSummary summary() const;
    [[nodiscard]] bool gates_passed() const;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] core::Status write_json(const std::filesystem::path& path) const;
};

[[nodiscard]] core::Result<TerrainEditTransitionBenchmarkReport>
run_terrain_edit_transition_benchmark(const TerrainEditTransitionBenchmarkConfig& config);

} // namespace heartstead::renderer::benchmark
