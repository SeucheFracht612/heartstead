#include "engine/world/streaming/chunk_streaming_benchmark.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace benchmark = heartstead::world::benchmark;

void test_small_benchmark_retains_open_loop_latency_and_bounds() {
    benchmark::ChunkStreamingBenchmarkConfig config;
    config.radius_chunks = 1;
    config.unrelated_history_edit_count = 1'024;
    config.warmup_repetitions = 0;
    config.repetitions = 2;
    config.update_interval_us = 50;
    config.timeout_ms = 5'000;
    config.scheduler.worker_count = 2;
    config.scheduler.max_concurrent_requests = 4;
    config.scheduler.max_completed_results = 4;
    config.scheduler.reservation_bytes_per_request = 1U * 1024U * 1024U;
    config.scheduler.max_reserved_working_bytes = 4U * 1024U * 1024U;
    config.scheduler.max_publications_per_update = 2;

    auto report = benchmark::run_chunk_streaming_benchmark(config);
    assert(report);
    assert(report.value().validate());
    assert(report.value().runs.size() == 6);
    assert(report.value().raw_samples.size() == 30);
    const auto summaries = report.value().summaries();
    assert(summaries.size() == 3);
    assert(std::ranges::all_of(summaries, [](const auto& summary) {
        return summary.run_count == 2 && summary.sample_count == 10 &&
               summary.p95_interest_to_publication_ms > 0.0 &&
               summary.p95_scheduler_pipeline_ms > 0.0 && summary.p95_generation_ms > 0.0 &&
               summary.p95_prepare_ms > 0.0 && summary.p95_worker_ms > 0.0 &&
               summary.mean_chunks_per_second > 0.0 &&
               summary.reserved_working_bytes_high_water > 0;
    }));
    assert(summaries[0].workload == benchmark::ChunkStreamingWorkload::near_load);
    assert(summaries[0].total_cancelled_requests == 0);
    assert(summaries[1].workload == benchmark::ChunkStreamingWorkload::teleport_recovery);
    assert(summaries[1].total_cancelled_requests > 0);
    assert(summaries[2].workload == benchmark::ChunkStreamingWorkload::saved_delta_publication);
    assert(summaries[2].total_saved_delta_publications == 10);
    assert(summaries[2].p95_disk_read_ms > 0.0);
    assert(summaries[2].p95_decode_ms > 0.0);
    assert(summaries[2].maximum_edit_log_cache_rebuilds_during_publication == 0);
    assert(std::ranges::all_of(report.value().runs, [&](const auto& run) {
        return run.desired_chunks == 5 && run.published_requests == 5 && run.failed_requests == 0 &&
               run.stale_requests == 0 && run.rejected_requests == 0 &&
               run.off_interest_publications == 0 && run.final_reserved_working_bytes == 0 &&
               run.reserved_working_bytes_high_water <= config.scheduler.max_reserved_working_bytes;
    }));
    assert(std::ranges::all_of(report.value().runs, [&](const auto& run) {
        if (run.workload != benchmark::ChunkStreamingWorkload::saved_delta_publication) {
            return run.saved_delta_publications == 0 && run.initial_edit_count == 0 &&
                   run.final_edit_count == 0;
        }
        return run.saved_delta_publications == 5 && run.initial_edit_count == 1'029 &&
               run.final_edit_count == 1'029 && run.edit_log_cache_rebuilds_during_publication == 0;
    }));

    auto gated = report.value();
    gated.config.enforce_gates = true;
    gated.config.maximum_near_p95_ms = 1'000'000.0;
    gated.config.maximum_teleport_p95_ms = 1'000'000.0;
    gated.config.maximum_saved_delta_p95_ms = 1'000'000.0;
    gated.config.maximum_owner_publication_us = 1'000'000;
    assert(gated.gates_passed());
    gated.config.maximum_near_p95_ms = 0.000'000'001;
    gated.config.maximum_teleport_p95_ms = 0.000'000'001;
    gated.config.maximum_saved_delta_p95_ms = 0.000'000'001;
    assert(!gated.gates_passed());

    const auto json = report.value().to_json();
    assert(json.contains("\"schema_version\": 2"));
    assert(json.contains("\"benchmark\": \"chunk_streaming\""));
    assert(json.contains("\"interest_to_publication_us\""));
    assert(json.contains("\"p95_interest_to_publication_ms\""));
    assert(json.contains("\"admission_deferred_updates\""));
    assert(json.contains("\"off_interest_publications\": 0"));
    assert(json.contains("\"source\": \"generated_with_saved_delta\""));
    assert(json.contains("\"unrelated_history_edit_count\": 1024"));
    assert(json.contains("\"edit_log_cache_rebuilds_during_publication\": 0"));

    const std::filesystem::path output_path{"chunk_streaming_benchmark_test_output.json"};
    std::error_code error;
    std::filesystem::remove(output_path, error);
    assert(report.value().write_json(output_path));
    std::ifstream input(output_path, std::ios::binary);
    const std::string persisted{std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{}};
    assert(persisted == json);
    input.close();
    assert(std::filesystem::remove(output_path));
}

void test_invalid_configs_fail_closed() {
    benchmark::ChunkStreamingBenchmarkConfig config;
    config.workloads.clear();
    assert(!config.validate());
    config.workloads = {benchmark::ChunkStreamingWorkload::near_load,
                        benchmark::ChunkStreamingWorkload::near_load};
    assert(!config.validate());
    config.workloads = {benchmark::ChunkStreamingWorkload::near_load};
    config.radius_chunks = 0;
    assert(!config.validate());
    config.radius_chunks = 1;
    config.unrelated_history_edit_count = 0;
    assert(!config.validate());
    config.unrelated_history_edit_count = 1;
    config.repetitions = 0;
    assert(!config.validate());
    config.repetitions = 1;
    config.scheduler.worker_count = 0;
    assert(!config.validate());
    assert(!benchmark::run_chunk_streaming_benchmark(config));
    assert(benchmark::chunk_streaming_workload_name(
               static_cast<benchmark::ChunkStreamingWorkload>(255)) == "unknown");
    assert(benchmark::chunk_streaming_workload_name(
               benchmark::ChunkStreamingWorkload::saved_delta_publication) ==
           "saved_delta_publication");
}

} // namespace

int main() {
    test_small_benchmark_retains_open_loop_latency_and_bounds();
    test_invalid_configs_fail_closed();
    return 0;
}
