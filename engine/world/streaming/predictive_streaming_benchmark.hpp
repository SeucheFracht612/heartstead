#pragma once

#include "engine/core/result.hpp"
#include "engine/profiling/runtime_metadata.hpp"
#include "engine/world/streaming/predictive_chunk_streaming_controller.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::world::benchmark {

enum class PredictiveStreamingPhase : std::uint8_t {
    steady_travel,
    reversal,
    teleport,
    post_teleport,
    bounded_soak,
};

[[nodiscard]] std::string_view
predictive_streaming_phase_name(PredictiveStreamingPhase phase) noexcept;

struct PredictiveStreamingBenchmarkConfig {
    std::uint64_t seed = 0x5052454645544348ULL;
    std::uint32_t steady_steps = 20;
    std::uint32_t reversal_steps = 8;
    std::uint32_t post_teleport_steps = 6;
    std::uint32_t soak_steps = 32;
    std::int64_t teleport_distance_chunks = 256;
    std::uint64_t movement_interval_us = 20'000;
    std::uint64_t owner_update_interval_us = 1'000;
    std::uint64_t settle_timeout_ms = 10'000;
    bool exercise_cancellation_probe = true;
    PredictiveChunkStreamingPolicy policy;
    ChunkLoadSchedulerConfig scheduler;
    bool enforce_gates = false;
    double maximum_predictive_hole_p95_ms = 250.0;
    double minimum_predictive_immediate_hit_rate = 0.50;
    double minimum_prediction_accuracy = 0.25;
    double maximum_prediction_waste_ratio = 0.75;
    double minimum_cancellation_completion_ratio = 0.75;
    double maximum_predictive_hole_rate_ratio_vs_baseline = 1.0;
    double maximum_soak_memory_slope_chunks_per_step = 0.05;
    std::uint64_t maximum_owner_publication_us = 500;

    PredictiveStreamingBenchmarkConfig();

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] std::size_t movement_step_count() const noexcept;
};

struct PredictiveStreamingStepSample {
    bool prediction_enabled = false;
    PredictiveStreamingPhase phase = PredictiveStreamingPhase::steady_travel;
    std::uint32_t ordinal = 0;
    ChunkCoord coord;
    ChunkStreamMemoryPressure pressure = ChunkStreamMemoryPressure::nominal;
    bool required_resident_at_start = false;
    bool required_resident_before_deadline = false;
    std::uint64_t visible_hole_us = 0;
    std::size_t resident_chunk_count = 0;
    std::size_t pending_load_count = 0;
    std::size_t active_speculative_count = 0;
    std::uint64_t cumulative_evicted_chunks = 0;
};

struct PredictiveStreamingBenchmarkTrial {
    bool prediction_enabled = false;
    std::uint64_t elapsed_us = 0;
    std::uint64_t movement_steps = 0;
    std::uint64_t immediate_required_hits = 0;
    std::uint64_t steps_with_visible_holes = 0;
    double immediate_hit_rate = 0.0;
    double p95_visible_hole_ms = 0.0;
    double maximum_visible_hole_ms = 0.0;
    std::size_t maximum_resident_chunk_count = 0;
    std::size_t final_resident_chunk_count = 0;
    double soak_memory_slope_chunks_per_step = 0.0;
    std::uint64_t evicted_chunks = 0;
    std::uint64_t deferred_required_loads = 0;
    std::uint64_t maximum_owner_publication_us = 0;
    std::uint64_t scheduler_submitted_requests = 0;
    std::uint64_t scheduler_published_requests = 0;
    std::uint64_t scheduler_cancelled_requests = 0;
    std::uint64_t scheduler_failed_requests = 0;
    std::uint64_t scheduler_stale_requests = 0;
    std::uint64_t scheduler_rejected_requests = 0;
    std::uint64_t scheduler_duplicate_requests = 0;
    std::size_t final_pending_load_count = 0;
    std::size_t final_reserved_working_bytes = 0;
    PredictiveChunkStreamingStats policy_stats;
    std::vector<PredictiveStreamingStepSample> raw_steps;
};

struct PredictiveStreamingBenchmarkViolation {
    std::string metric;
    double actual = 0.0;
    double limit = 0.0;
};

struct PredictiveStreamingBenchmarkGateEvaluation {
    bool evaluated = false;
    bool passed = true;
    std::vector<PredictiveStreamingBenchmarkViolation> violations;
};

struct PredictiveStreamingBenchmarkReport {
    static constexpr std::uint32_t schema_version = 1;

    PredictiveStreamingBenchmarkConfig config;
    profiling::RuntimeMetadata runtime;
    PredictiveStreamingBenchmarkTrial baseline;
    PredictiveStreamingBenchmarkTrial predictive;
    PredictiveStreamingBenchmarkGateEvaluation gates;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] bool gates_passed() const noexcept;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] core::Status write_json(const std::filesystem::path& path) const;
};

[[nodiscard]] core::Result<PredictiveStreamingBenchmarkReport>
run_predictive_streaming_benchmark(const PredictiveStreamingBenchmarkConfig& config);

} // namespace heartstead::world::benchmark
