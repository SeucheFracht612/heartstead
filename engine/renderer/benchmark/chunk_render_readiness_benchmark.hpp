#pragma once

#include "engine/core/result.hpp"
#include "engine/profiling/runtime_metadata.hpp"
#include "engine/renderer/chunks/chunk_render_system.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/world/streaming/chunk_load_scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer::benchmark {

struct ChunkRenderReadinessBenchmarkConfig {
    rhi::RenderBackend render_backend = rhi::RenderBackend::headless;
    std::uint64_t seed = 0x48535452454E4445ULL;
    std::uint16_t horizontal_radius_chunks = 2;
    std::uint32_t warmup_repetitions = 2;
    std::uint32_t repetitions = 9;
    std::uint64_t update_interval_us = 16'667;
    std::uint64_t timeout_ms = 5'000;
    world::ChunkLoadSchedulerConfig loading;
    ChunkRenderConfig rendering;
    bool enforce_gates = false;
    double maximum_draw_eligibility_p95_ms = 250.0;
    double maximum_upload_preparation_ms = 0.5;
    double maximum_synchronous_gpu_wait_ms = 0.0;
    double maximum_mesh_builds_per_publication = 2.5;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkRenderReadinessDeviceMetadata {
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

struct ChunkRenderReadinessBenchmarkSample {
    std::uint32_t repetition = 0;
    std::uint32_t ordinal = 0;
    world::ChunkCoord coord;
    std::uint64_t request_id = 0;
    std::uint64_t load_generation = 0;
    std::uint64_t mesh_request_revision = 0;
    world::ChunkStreamLoadSource source = world::ChunkStreamLoadSource::generated;
    std::size_t saved_edit_count = 0;
    std::uint64_t interest_to_publication_us = 0;
    std::uint64_t publication_to_mesh_resident_us = 0;
    std::uint64_t interest_to_mesh_resident_us = 0;
    std::uint64_t mesh_resident_to_draw_eligibility_us = 0;
    std::uint64_t interest_to_draw_eligibility_us = 0;
    double scheduler_pipeline_ms = 0.0;
    std::uint64_t first_draw_owner_update = 0;
    std::uint32_t draw_command_count = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    std::size_t resident_bytes = 0;
};

struct ChunkRenderReadinessBenchmarkRun {
    std::uint32_t repetition = 0;
    std::size_t desired_chunks = 0;
    std::uint64_t owner_updates = 0;
    std::uint64_t elapsed_us = 0;
    std::uint64_t submitted_requests = 0;
    std::uint64_t published_requests = 0;
    std::uint64_t draw_eligible_chunks = 0;
    std::uint64_t cancelled_requests = 0;
    std::uint64_t stale_requests = 0;
    std::uint64_t failed_requests = 0;
    std::uint64_t rejected_requests = 0;
    std::uint64_t admission_deferred_updates = 0;
    std::uint64_t load_item_budget_exhaustions = 0;
    std::uint64_t load_time_budget_exhaustions = 0;
    std::uint64_t maximum_publication_time_us = 0;
    std::size_t reserved_working_bytes_high_water = 0;
    std::size_t final_reserved_working_bytes = 0;
    std::uint64_t scheduled_mesh_jobs = 0;
    std::uint64_t completed_mesh_jobs = 0;
    std::uint64_t built_meshes = 0;
    std::uint64_t published_meshes = 0;
    std::uint64_t stale_mesh_results = 0;
    std::uint64_t cancelled_mesh_results = 0;
    std::uint64_t failed_mesh_results = 0;
    std::uint64_t failed_uploads = 0;
    std::uint64_t coalesced_mesh_invalidations = 0;
    std::uint64_t abandoned_mesh_invalidations = 0;
    std::uint64_t uploaded_chunks = 0;
    std::uint64_t uploaded_bytes = 0;
    double mesh_builds_per_publication = 0.0;
    std::size_t pending_meshes = 0;
    std::size_t in_flight_meshes = 0;
    std::size_t pending_uploads = 0;
    std::size_t pending_edit_responses = 0;
    std::size_t current_mesh_stages = 0;
    std::size_t exact_resident_meshes = 0;
    std::size_t drawable_resident_meshes = 0;
    std::size_t peak_gpu_resident_bytes = 0;
    std::size_t maximum_snapshot_cells_per_update = 0;
    std::size_t maximum_uploaded_bytes_per_update = 0;
    double maximum_owner_update_ms = 0.0;
    double maximum_render_synchronize_ms = 0.0;
    double maximum_draw_list_build_ms = 0.0;
    double maximum_chunk_snapshot_ms = 0.0;
    double maximum_meshing_ms = 0.0;
    double maximum_upload_preparation_ms = 0.0;
    double maximum_upload_ms = 0.0;
    double maximum_gpu_wait_ms = 0.0;
    std::size_t baseline_live_render_resources = 0;
    std::size_t final_live_render_resources = 0;
};

struct ChunkRenderReadinessBenchmarkViolation {
    std::string metric;
    double actual = 0.0;
    double limit = 0.0;
};

struct ChunkRenderReadinessBenchmarkGateEvaluation {
    bool evaluated = false;
    bool passed = true;
    std::vector<ChunkRenderReadinessBenchmarkViolation> violations;
};

struct ChunkRenderReadinessBenchmarkSummary {
    std::size_t run_count = 0;
    std::size_t sample_count = 0;
    double median_interest_to_publication_ms = 0.0;
    double p95_interest_to_publication_ms = 0.0;
    double p99_interest_to_publication_ms = 0.0;
    double median_publication_to_mesh_resident_ms = 0.0;
    double p95_publication_to_mesh_resident_ms = 0.0;
    double p99_publication_to_mesh_resident_ms = 0.0;
    double median_interest_to_mesh_resident_ms = 0.0;
    double p95_interest_to_mesh_resident_ms = 0.0;
    double p99_interest_to_mesh_resident_ms = 0.0;
    double median_interest_to_draw_eligibility_ms = 0.0;
    double p95_interest_to_draw_eligibility_ms = 0.0;
    double p99_interest_to_draw_eligibility_ms = 0.0;
    double maximum_interest_to_draw_eligibility_ms = 0.0;
    double mean_chunks_per_second = 0.0;
    std::uint64_t maximum_publication_time_us = 0;
    std::size_t reserved_working_bytes_high_water = 0;
    std::size_t peak_gpu_resident_bytes = 0;
    std::size_t maximum_snapshot_cells_per_update = 0;
    std::size_t maximum_uploaded_bytes_per_update = 0;
    double maximum_mesh_builds_per_publication = 0.0;
    double maximum_owner_update_ms = 0.0;
    double maximum_render_synchronize_ms = 0.0;
    double maximum_draw_list_build_ms = 0.0;
    double maximum_chunk_snapshot_ms = 0.0;
    double maximum_meshing_ms = 0.0;
    double maximum_upload_preparation_ms = 0.0;
    double maximum_upload_ms = 0.0;
    double maximum_gpu_wait_ms = 0.0;
    std::uint64_t total_stale_mesh_results = 0;
    std::uint64_t total_cancelled_mesh_results = 0;
    ChunkRenderReadinessBenchmarkGateEvaluation gates;
};

struct ChunkRenderReadinessBenchmarkReport {
    static constexpr std::uint32_t schema_version = 1;
    static constexpr std::string_view measurement_endpoint =
        "first_exact_current_chunk_draw_command";

    ChunkRenderReadinessBenchmarkConfig config;
    profiling::RuntimeMetadata runtime;
    ChunkRenderReadinessDeviceMetadata device;
    std::vector<ChunkRenderReadinessBenchmarkRun> runs;
    std::vector<ChunkRenderReadinessBenchmarkSample> raw_samples;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] ChunkRenderReadinessBenchmarkSummary summary() const;
    [[nodiscard]] bool gates_passed() const;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] core::Status write_json(const std::filesystem::path& path) const;
};

[[nodiscard]] core::Result<ChunkRenderReadinessBenchmarkReport>
run_chunk_render_readiness_benchmark(const ChunkRenderReadinessBenchmarkConfig& config);

} // namespace heartstead::renderer::benchmark
