#pragma once

#include "engine/core/result.hpp"
#include "engine/physics/chunk_collision_system.hpp"
#include "engine/profiling/runtime_metadata.hpp"
#include "engine/world/lighting/chunk_light_system.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace heartstead::world::benchmark {

struct VoxelResponseBenchmarkConfig {
    physics::PhysicsBackend physics_backend = physics::PhysicsBackend::headless;
    std::uint16_t horizontal_radius_chunks = 1;
    std::uint32_t warmup_repetitions = 2;
    std::uint32_t repetitions = 9;
    std::uint64_t update_interval_us = 16'667;
    std::uint64_t timeout_ms = 5'000;
    physics::ChunkCollisionSystemConfig collision;
    ChunkLightSystemConfig lighting;
    bool enforce_gates = false;
    double maximum_collision_p95_ms = 100.0;
    double maximum_relight_p95_ms = 250.0;

    [[nodiscard]] core::Status validate() const;
};

struct VoxelResponseBenchmarkSample {
    std::uint32_t repetition = 0;
    ChunkCoord coord;
    VoxelCoord voxel;
    bool became_solid = false;
    std::uint64_t owner_updates = 0;
    double collision_response_ms = 0.0;
    double relight_convergence_ms = 0.0;
    double maximum_collision_cooking_ms = 0.0;
    double maximum_collision_apply_ms = 0.0;
    double maximum_relight_solve_ms = 0.0;
    double maximum_relight_apply_ms = 0.0;
    double maximum_owner_update_ms = 0.0;
    std::uint64_t snapshot_cells_copied = 0;
    std::uint64_t relight_changed_chunks = 0;
    std::uint64_t relight_changed_cells = 0;
    std::uint64_t collision_stale_results = 0;
    std::uint64_t relight_stale_snapshots = 0;
    std::uint64_t relight_stale_results = 0;
    std::uint64_t relight_apply_budget_overruns = 0;
};

struct VoxelResponseBenchmarkRun {
    std::size_t chunk_count = 0;
    std::uint32_t warmup_edits = 0;
    std::uint32_t measured_edits = 0;
    std::uint64_t owner_updates = 0;
    std::uint64_t elapsed_us = 0;
    std::uint64_t collision_response_completions = 0;
    std::uint64_t relight_response_completions = 0;
    std::uint64_t collision_coalesced_invalidations = 0;
    std::uint64_t relight_coalesced_invalidations = 0;
    std::uint64_t collision_abandoned_invalidations = 0;
    std::uint64_t relight_abandoned_invalidations = 0;
    std::uint64_t collision_stale_results = 0;
    std::uint64_t relight_stale_snapshots = 0;
    std::uint64_t relight_stale_results = 0;
    std::uint64_t collision_failed_results = 0;
    std::uint64_t relight_failed_results = 0;
    std::uint64_t relight_apply_budget_overruns = 0;
    std::size_t pending_collision_responses = 0;
    std::size_t pending_relight_responses = 0;
    std::size_t current_collision_stages = 0;
    std::size_t current_lighting_stages = 0;
};

struct VoxelResponseBenchmarkViolation {
    std::string metric;
    double actual = 0.0;
    double limit = 0.0;
};

struct VoxelResponseBenchmarkGateEvaluation {
    bool evaluated = false;
    bool passed = true;
    std::vector<VoxelResponseBenchmarkViolation> violations;
};

struct VoxelResponseBenchmarkSummary {
    std::size_t sample_count = 0;
    double median_collision_response_ms = 0.0;
    double p95_collision_response_ms = 0.0;
    double p99_collision_response_ms = 0.0;
    double maximum_collision_response_ms = 0.0;
    double median_relight_convergence_ms = 0.0;
    double p95_relight_convergence_ms = 0.0;
    double p99_relight_convergence_ms = 0.0;
    double maximum_relight_convergence_ms = 0.0;
    double mean_owner_updates_per_edit = 0.0;
    double maximum_collision_cooking_ms = 0.0;
    double maximum_collision_apply_ms = 0.0;
    double maximum_relight_solve_ms = 0.0;
    double maximum_relight_apply_ms = 0.0;
    double maximum_owner_update_ms = 0.0;
    std::uint64_t total_snapshot_cells_copied = 0;
    VoxelResponseBenchmarkGateEvaluation gates;
};

struct VoxelResponseBenchmarkReport {
    static constexpr std::uint32_t schema_version = 1;

    VoxelResponseBenchmarkConfig config;
    profiling::RuntimeMetadata runtime;
    VoxelResponseBenchmarkRun run;
    std::vector<VoxelResponseBenchmarkSample> raw_samples;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] VoxelResponseBenchmarkSummary summary() const;
    [[nodiscard]] bool gates_passed() const;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] core::Status write_json(const std::filesystem::path& path) const;
};

[[nodiscard]] core::Result<VoxelResponseBenchmarkReport>
run_voxel_response_benchmark(const VoxelResponseBenchmarkConfig& config);

} // namespace heartstead::world::benchmark
