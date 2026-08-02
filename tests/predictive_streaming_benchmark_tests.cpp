#include "engine/world/streaming/predictive_streaming_benchmark.hpp"

#include <cassert>
#include <iostream>
#include <string>

namespace {

namespace benchmark = heartstead::world::benchmark;

void test_small_paired_benchmark_retains_policy_and_memory_evidence() {
    benchmark::PredictiveStreamingBenchmarkConfig config;
    config.steady_steps = 4;
    config.reversal_steps = 2;
    config.post_teleport_steps = 2;
    config.soak_steps = 4;
    config.movement_interval_us = 20'000;
    config.owner_update_interval_us = 1'000;
    config.policy.max_speculative_submissions_per_update = 2;
    config.policy.max_active_speculative_requests = 3;
    config.policy.nominal_resident_chunk_budget = 8;
    config.policy.elevated_resident_chunk_budget = 4;
    config.policy.critical_resident_chunk_budget = 2;
    config.scheduler.worker_count = 2;
    config.scheduler.max_concurrent_requests = 4;
    config.scheduler.max_completed_results = 4;
    config.scheduler.max_reserved_working_bytes =
        config.scheduler.reservation_bytes_per_request * config.scheduler.max_concurrent_requests;
    config.enforce_gates = false;

    auto report = benchmark::run_predictive_streaming_benchmark(config);
    if (!report) {
        std::cerr << report.error().code << ": " << report.error().message << '\n';
    }
    assert(report);
    assert(report.value().validate());
    assert(report.value().baseline.movement_steps == config.movement_step_count());
    assert(report.value().predictive.movement_steps == config.movement_step_count());
    assert(report.value().baseline.raw_steps.size() == config.movement_step_count());
    assert(report.value().predictive.raw_steps.size() == config.movement_step_count());
    assert(report.value().baseline.policy_stats.speculative_submissions == 0);
    assert(report.value().predictive.policy_stats.speculative_submissions > 0);
    assert(report.value().predictive.policy_stats.demand_transitions > 0);
    assert(report.value().predictive.evicted_chunks > 0);
    assert(report.value().predictive.final_pending_load_count == 0);
    assert(report.value().predictive.final_reserved_working_bytes == 0);
    assert(report.value().predictive.final_resident_chunk_count <=
           config.policy.critical_resident_chunk_budget);
    assert(report.value().gates.evaluated);

    const auto json = report.value().to_json();
    assert(json.find("\"schema_version\": 1") != std::string::npos);
    assert(json.find("\"benchmark\": \"predictive_streaming\"") != std::string::npos);
    assert(json.find("\"baseline\"") != std::string::npos);
    assert(json.find("\"predictive\"") != std::string::npos);
    assert(json.find("\"prediction_accuracy\"") != std::string::npos);
    assert(json.find("\"max_evictions_per_update\": 4") != std::string::npos);
    assert(json.find("\"soak_memory_slope_chunks_per_step\"") != std::string::npos);
}

void test_invalid_cadence_fails_closed() {
    benchmark::PredictiveStreamingBenchmarkConfig config;
    config.owner_update_interval_us = config.movement_interval_us + 1;
    const auto status = config.validate();
    assert(!status);
    assert(status.error().code == "predictive_streaming_benchmark.invalid_workload");
}

} // namespace

int main() {
    test_small_paired_benchmark_retains_policy_and_memory_evidence();
    test_invalid_cadence_fails_closed();
    return 0;
}
