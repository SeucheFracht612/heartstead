#pragma once

#include "engine/core/result.hpp"
#include "engine/processes/process_temporal_aggregation.hpp"
#include "engine/profiling/runtime_metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace heartstead::processes::benchmark {

struct ProcessTemporalAggregationBenchmarkConfig {
    std::uint32_t process_count = 65'536;
    simulation::WorldTick simulation_ticks = 600;
    std::uint32_t burst_process_count = 2'048;
    std::uint32_t stalled_process_count = 256;
    simulation::WorldTick stress_tick = 300;
    std::uint64_t seed = 0x48535450524f4345ULL;
    std::uint32_t warmup_repetitions = 1;
    std::uint32_t repetitions = 5;
    ProcessTemporalAggregationConfig temporal{
        .maximum_admissions_per_tick = 4'096,
        .maximum_events_per_tick = 1'024,
        .maximum_tracked_processes = 65'536,
        .stalled_reevaluation_interval_ticks = 20,
        .maximum_catch_up_ticks_per_event = 1'200,
        .maximum_catch_up_ticks_per_tick = 4'800,
    };
    std::uint32_t maximum_event_backlog_ticks = 2;
    double maximum_temporal_p99_tick_ms = 5.0;
    double minimum_median_speedup = 5.0;
    double minimum_resolver_call_reduction_ratio = 0.95;

    [[nodiscard]] core::Status validate() const;
};

struct ProcessTemporalAggregationBenchmarkSample {
    std::uint32_t repetition = 0;
    simulation::WorldTick world_tick = 0;
    std::uint32_t drain_pass = 0;
    std::uint64_t temporal_elapsed_nanoseconds = 0;
    std::uint64_t dense_elapsed_nanoseconds = 0;
    std::uint32_t temporal_resolver_call_count = 0;
    std::uint32_t dense_resolver_call_count = 0;
    std::uint32_t temporal_admission_count = 0;
    std::uint32_t temporal_dispatched_event_count = 0;
    std::uint32_t temporal_evaluated_process_count = 0;
    std::uint32_t temporal_changed_process_count = 0;
    std::uint32_t temporal_completed_process_count = 0;
    std::uint32_t temporal_stale_event_count = 0;
    std::uint32_t temporal_retired_event_count = 0;
    std::size_t temporal_active_event_count = 0;
    std::size_t temporal_unadmitted_process_count = 0;
    simulation::WorldTick temporal_evaluated_delta_ticks = 0;
    simulation::WorldTick temporal_catch_up_delta_ticks = 0;
    simulation::WorldTick temporal_maximum_lateness_ticks = 0;
    simulation::WorldTick temporal_oldest_deferred_lateness_ticks = 0;
    bool temporal_event_budget_exhausted = false;
    bool temporal_catch_up_budget_exhausted = false;
    bool temporal_counters_saturated = false;
};

struct ProcessTemporalAggregationTimingSummary {
    std::size_t sample_count = 0;
    double minimum_ms = 0.0;
    double median_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double maximum_ms = 0.0;
    double mean_ms = 0.0;
    double standard_deviation_ms = 0.0;
    double coefficient_of_variation = 0.0;
};

struct ProcessTemporalAggregationBenchmarkRepetition {
    std::uint32_t repetition = 0;
    std::uint32_t admission_tick_count = 0;
    std::uint64_t admission_elapsed_nanoseconds = 0;
    ProcessTemporalAggregationTimingSummary temporal_timing;
    ProcessTemporalAggregationTimingSummary dense_timing;
    double median_speedup = 0.0;
    double resolver_call_reduction_ratio = 0.0;
    std::uint64_t temporal_resolver_call_count = 0;
    std::uint64_t dense_resolver_call_count = 0;
    std::uint32_t maximum_event_backlog_ticks = 0;
    simulation::WorldTick maximum_event_lateness_ticks = 0;
    std::size_t maximum_active_event_count = 0;
    std::size_t maximum_unadmitted_process_count = 0;
    simulation::WorldTick maximum_deferred_lateness_ticks = 0;
    std::uint64_t budget_violation_count = 0;
    std::uint64_t parity_mismatch_count = 0;
    std::uint64_t timestamp_mismatch_count = 0;
    std::uint64_t unexpected_outcome_count = 0;
    std::uint64_t temporal_state_checksum = 0;
    std::uint64_t dense_state_checksum = 0;
};

struct ProcessTemporalAggregationAcceptanceCheck {
    std::string name;
    std::string comparison;
    double measured = 0.0;
    double limit = 0.0;
    bool enabled = true;
    bool passed = false;
};

struct ProcessTemporalAggregationBenchmarkReport {
    static constexpr std::uint32_t schema_version = 1;

    ProcessTemporalAggregationBenchmarkConfig config;
    profiling::RuntimeMetadata runtime;
    std::vector<ProcessTemporalAggregationBenchmarkSample> raw_samples;
    std::vector<ProcessTemporalAggregationBenchmarkRepetition> repetitions;
    ProcessTemporalAggregationTimingSummary temporal_timing;
    ProcessTemporalAggregationTimingSummary dense_timing;
    double median_speedup = 0.0;
    double minimum_resolver_call_reduction_ratio = 0.0;
    std::vector<ProcessTemporalAggregationAcceptanceCheck> acceptance;

    [[nodiscard]] bool acceptance_passed() const noexcept;
    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] core::Status write_json(const std::filesystem::path& path) const;
};

[[nodiscard]] core::Result<ProcessTemporalAggregationBenchmarkReport>
run_process_temporal_aggregation_benchmark(const ProcessTemporalAggregationBenchmarkConfig& config);

} // namespace heartstead::processes::benchmark
