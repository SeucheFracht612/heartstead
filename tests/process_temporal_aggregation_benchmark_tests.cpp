#include "engine/processes/process_temporal_aggregation_benchmark.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

namespace {

namespace benchmark = heartstead::processes::benchmark;

[[nodiscard]] benchmark::ProcessTemporalAggregationBenchmarkConfig small_config() {
    benchmark::ProcessTemporalAggregationBenchmarkConfig config;
    config.process_count = 256;
    config.simulation_ticks = 64;
    config.burst_process_count = 128;
    config.stalled_process_count = 8;
    config.stress_tick = 32;
    config.warmup_repetitions = 0;
    config.repetitions = 2;
    config.temporal.maximum_admissions_per_tick = 128;
    config.temporal.maximum_events_per_tick = 64;
    config.temporal.maximum_tracked_processes = 256;
    config.temporal.stalled_reevaluation_interval_ticks = 8;
    config.temporal.maximum_catch_up_ticks_per_event = 64;
    config.temporal.maximum_catch_up_ticks_per_tick = 256;
    config.maximum_event_backlog_ticks = 2;
    config.maximum_temporal_p99_tick_ms = 0.0;
    config.minimum_median_speedup = 0.0;
    config.minimum_resolver_call_reduction_ratio = 0.0;
    return config;
}

void test_paired_benchmark_retains_parity_pressure_and_raw_ticks() {
    const auto config = small_config();
    auto report = benchmark::run_process_temporal_aggregation_benchmark(config);
    if (!report) {
        std::cerr << report.error().code << ": " << report.error().message << '\n';
    }
    assert(report);
    assert(report.value().validate());
    assert(report.value().acceptance_passed());
    assert(report.value().repetitions.size() == config.repetitions);
    assert(report.value().temporal_timing.sample_count ==
           static_cast<std::size_t>(config.repetitions) * config.simulation_ticks);
    assert(report.value().dense_timing.sample_count == report.value().temporal_timing.sample_count);
    assert(report.value().raw_samples.size() >= report.value().temporal_timing.sample_count);
    assert(report.value().minimum_resolver_call_reduction_ratio > 0.0);

    const auto expected_dense_calls =
        static_cast<std::uint64_t>(config.process_count) * config.simulation_ticks;
    const auto checksum = report.value().repetitions.front().temporal_state_checksum;
    for (const auto& repetition : report.value().repetitions) {
        assert(repetition.admission_tick_count == 2);
        assert(repetition.dense_resolver_call_count == expected_dense_calls);
        assert(repetition.temporal_resolver_call_count < repetition.dense_resolver_call_count);
        assert(repetition.maximum_event_backlog_ticks > 0);
        assert(repetition.maximum_event_backlog_ticks <= config.maximum_event_backlog_ticks);
        assert(repetition.maximum_event_lateness_ticks > 0);
        assert(repetition.maximum_event_lateness_ticks <= config.maximum_event_backlog_ticks);
        assert(repetition.maximum_deferred_lateness_ticks > 0);
        assert(repetition.budget_violation_count == 0);
        assert(repetition.parity_mismatch_count == 0);
        assert(repetition.timestamp_mismatch_count > 0);
        assert(repetition.unexpected_outcome_count == 0);
        assert(repetition.temporal_state_checksum == repetition.dense_state_checksum);
        assert(repetition.temporal_state_checksum == checksum);
    }

    const auto normal_samples = static_cast<std::size_t>(std::ranges::count_if(
        report.value().raw_samples, [](const auto& sample) { return sample.drain_pass == 0; }));
    const auto pressured_samples =
        std::ranges::count_if(report.value().raw_samples, [](const auto& sample) {
            return sample.drain_pass == 0 && sample.temporal_event_budget_exhausted;
        });
    assert(normal_samples == report.value().temporal_timing.sample_count);
    assert(pressured_samples > 0);
    for (const auto& sample : report.value().raw_samples) {
        assert(sample.temporal_elapsed_nanoseconds > 0);
        assert(sample.temporal_admission_count == 0);
        assert(sample.temporal_dispatched_event_count <= config.temporal.maximum_events_per_tick);
        assert(sample.temporal_stale_event_count == 0);
        assert(sample.temporal_retired_event_count == 0);
        assert(!sample.temporal_counters_saturated);
        if (sample.drain_pass == 0) {
            assert(sample.dense_elapsed_nanoseconds > 0);
            assert(sample.dense_resolver_call_count == config.process_count);
        } else {
            assert(sample.world_tick == config.simulation_ticks);
            assert(sample.dense_elapsed_nanoseconds == 0);
            assert(sample.dense_resolver_call_count == 0);
        }
    }

    const auto json = report.value().to_json();
    assert(json.find("\"schema_version\": 1") != std::string::npos);
    assert(json.find("\"benchmark\": \"process_temporal_aggregation\"") != std::string::npos);
    assert(json.find("\"dense_reference_semantic_parity_mismatches\"") != std::string::npos);
    assert(json.find("\"maximum_event_lateness_ticks\"") != std::string::npos);
    assert(json.find("\"timestamp_mismatch_count\"") != std::string::npos);
    assert(json.find("\"raw_samples\"") != std::string::npos);

    const auto output_path = std::filesystem::temp_directory_path() /
                             "heartstead_process_temporal_aggregation_benchmark_test.json";
    std::error_code remove_error;
    std::filesystem::remove(output_path, remove_error);
    auto write_status = report.value().write_json(output_path);
    assert(write_status);
    std::ifstream input(output_path, std::ios::binary);
    const std::string persisted{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
    assert(input.good() || input.eof());
    assert(persisted == json);
    std::filesystem::remove(output_path, remove_error);

    auto inconsistent = report.value();
    inconsistent.acceptance.front().passed = !inconsistent.acceptance.front().passed;
    const auto inconsistent_status = inconsistent.validate();
    assert(!inconsistent_status);
    assert(inconsistent_status.error().code == "process_temporal_benchmark.invalid_acceptance");
}

void test_invalid_workloads_fail_closed() {
    auto config = small_config();
    config.process_count = 0;
    auto status = config.validate();
    assert(!status);
    assert(status.error().code == "process_temporal_benchmark.invalid_process_count");

    config = small_config();
    config.burst_process_count = 250;
    config.stalled_process_count = 7;
    status = config.validate();
    assert(!status);
    assert(status.error().code == "process_temporal_benchmark.invalid_process_mix");

    config = small_config();
    config.temporal.maximum_tracked_processes = config.process_count - 1U;
    status = config.validate();
    assert(!status);
    assert(status.error().code == "process_temporal_benchmark.insufficient_tracking_budget");

    config = small_config();
    config.repetitions = 0;
    status = config.validate();
    assert(!status);
    assert(status.error().code == "process_temporal_benchmark.invalid_repetitions");

    config = small_config();
    config.maximum_temporal_p99_tick_ms = std::numeric_limits<double>::quiet_NaN();
    status = config.validate();
    assert(!status);
    assert(status.error().code == "process_temporal_benchmark.invalid_acceptance_gate");

    config = {};
    config.process_count = 1'000'000;
    config.simulation_ticks = 1;
    config.burst_process_count = 0;
    config.stalled_process_count = 0;
    config.stress_tick = 1;
    config.repetitions = 100;
    config.warmup_repetitions = 0;
    config.temporal.maximum_events_per_tick = 1;
    config.temporal.maximum_tracked_processes = config.process_count;
    status = config.validate();
    assert(!status);
    assert(status.error().code == "process_temporal_benchmark.excessive_sample_count");

    config = {};
    config.process_count = 1'000'000;
    config.simulation_ticks = 1'001;
    config.burst_process_count = 0;
    config.stalled_process_count = 0;
    config.stress_tick = 1;
    config.repetitions = 1;
    config.warmup_repetitions = 1;
    config.temporal.maximum_tracked_processes = config.process_count;
    status = config.validate();
    assert(!status);
    assert(status.error().code == "process_temporal_benchmark.excessive_dense_work");
}

} // namespace

int main() {
    test_paired_benchmark_retains_parity_pressure_and_raw_ticks();
    test_invalid_workloads_fail_closed();
    return 0;
}
