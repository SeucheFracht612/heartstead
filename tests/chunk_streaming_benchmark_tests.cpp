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
    assert(!report.value().save_under_streaming.executed);
    assert(report.value().save_under_streaming.raw_samples.empty());
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
    assert(json.contains("\"schema_version\": 4"));
    assert(json.contains("\"benchmark\": \"chunk_streaming\""));
    assert(json.contains("\"interest_to_publication_us\""));
    assert(json.contains("\"p95_interest_to_publication_ms\""));
    assert(json.contains("\"admission_deferred_updates\""));
    assert(json.contains("\"off_interest_publications\": 0"));
    assert(json.contains("\"source\": \"generated_with_saved_delta\""));
    assert(json.contains("\"unrelated_history_edit_count\": 1024"));
    assert(json.contains("\"edit_log_cache_rebuilds_during_publication\": 0"));
    assert(json.contains("\"physical_fixture\": {\"used\": false"));
    assert(json.contains("\"save_under_streaming\": {"));
    assert(json.contains("\"executed\": false"));

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

void test_file_backed_workloads_report_cache_state_and_scale() {
    benchmark::ChunkStreamingBenchmarkConfig config;
    config.workloads = {benchmark::ChunkStreamingWorkload::file_delta_warm};
#if defined(__linux__)
    config.workloads.push_back(benchmark::ChunkStreamingWorkload::file_delta_drop_cache_advised);
#endif
    config.radius_chunks = 1;
    config.unrelated_history_edit_count = 64;
    config.physical_saved_delta_record_count = 32;
    config.warmup_repetitions = 0;
    config.repetitions = 1;
    config.update_interval_us = 50;
    config.timeout_ms = 5'000;
    config.run_save_under_streaming = true;
    config.save_timeout_ms = 5'000;
    config.scheduler.worker_count = 2;
    config.scheduler.max_concurrent_requests = 4;
    config.scheduler.max_completed_results = 4;
    config.scheduler.reservation_bytes_per_request = 1U * 1024U * 1024U;
    config.scheduler.max_reserved_working_bytes = 4U * 1024U * 1024U;
    config.scheduler.max_publications_per_update = 2;
    config.enforce_gates = true;
    config.maximum_file_delta_p95_ms = 1'000'000.0;
    config.maximum_file_delta_disk_read_p95_ms = 1'000'000.0;
    config.maximum_file_delta_reader_open_p95_ms = 1'000'000.0;
    config.maximum_owner_publication_us = 1'000'000;
    config.maximum_save_under_streaming_p95_ms = 1'000'000.0;
    config.maximum_save_submission_ms = 1'000'000.0;
    config.maximum_save_durable_acceptance_ms = 1'000'000.0;
    config.maximum_save_compaction_ms = 1'000'000.0;

    auto report = benchmark::run_chunk_streaming_benchmark(config);
    assert(report);
    assert(report.value().validate());
    assert(report.value().gates_passed());
    assert(report.value().physical_fixture.used);
    assert(report.value().physical_fixture.record_count == 32);
    assert(report.value().physical_fixture.encoded_payload_bytes > 0);
    assert(report.value().physical_fixture.setup_ms > 0.0);
    assert(report.value().physical_fixture.removed_after_run);
    assert(!std::filesystem::exists(report.value().physical_fixture.ephemeral_root));
    assert(report.value().runs.size() == config.workloads.size());
    assert(report.value().raw_samples.size() == config.workloads.size() * 5U);

    const auto& save = report.value().save_under_streaming;
    assert(save.executed);
    assert(!save.pinned_generation.empty());
    assert(!save.published_generation.empty());
    assert(save.pinned_generation != save.published_generation);
    assert(save.physical_indexed_delta_count == 32);
    assert(save.desired_chunks == 5);
    assert(save.submitted_requests == 5);
    assert(save.published_requests == 5);
    assert(save.failed_requests == 0);
    assert(save.stale_requests == 0);
    assert(save.rejected_requests == 0);
    assert(save.saved_delta_publications == 5);
    assert(save.final_reserved_working_bytes == 0);
    assert(save.delta_reader_open_ms >= 0.0);
    assert(save.snapshot_clone_ms >= 0.0);
    assert(save.save_submission_ms >= 0.0);
    assert(save.save_durable_acceptance_ms > 0.0);
    assert(save.save_durable_operation_ms > 0.0);
    assert(save.save_durable_acceptance_ms >= save.save_durable_operation_ms);
    assert(save.save_compaction_ms > 0.0);
    assert(save.save_total_worker_ms >= save.save_compaction_ms);
    assert(save.p95_interest_to_publication_ms > 0.0);
    assert(save.p95_disk_read_ms > 0.0);
    assert(save.save_durably_accepted);
    assert(save.save_compacted);
    assert(save.destructive_maintenance_busy_while_pinned);
    assert(save.reader_gap_pruned_stale_generation);
    assert(save.saved_delta_source_rotations == 2);
    assert(save.raw_samples.size() == 5);
    assert(save.gates.evaluated);
    assert(save.gates.passed);

    const auto summaries = report.value().summaries();
    assert(summaries.size() == config.workloads.size());
    for (const auto& summary : summaries) {
        assert(summary.run_count == 1);
        assert(summary.sample_count == 5);
        assert(summary.total_saved_delta_publications == 5);
        assert(summary.p95_disk_read_ms > 0.0);
        assert(summary.p95_delta_reader_open_ms > 0.0);
    }
    const auto& warm_run = report.value().runs.front();
    assert(warm_run.workload == benchmark::ChunkStreamingWorkload::file_delta_warm);
    assert(warm_run.physical_indexed_delta_count == 32);
    assert(warm_run.cache_preloaded_payload_count == 5);
    assert(!warm_run.cache_advice_supported);
    assert(warm_run.cache_advice_attempted_file_count == 0);
    assert(warm_run.cache_advice_accepted_file_count == 0);
#if defined(__linux__)
    const auto& advised_run = report.value().runs.back();
    assert(advised_run.workload ==
           benchmark::ChunkStreamingWorkload::file_delta_drop_cache_advised);
    assert(advised_run.physical_indexed_delta_count == 32);
    assert(advised_run.cache_preloaded_payload_count == 0);
    assert(advised_run.cache_advice_supported);
    assert(advised_run.cache_advice_attempted_file_count == 7);
    assert(advised_run.cache_advice_accepted_file_count == 7);
#endif

    const auto json = report.value().to_json();
    assert(json.contains("\"physical_saved_delta_record_count\": 32"));
    assert(json.contains("\"file_delta_warm\""));
    assert(json.contains("\"physical_indexed_delta_count\": 32"));
    assert(json.contains("\"delta_reader_open_ms\""));
    assert(json.contains("\"cache_preloaded_payload_count\": 5"));
    assert(json.contains("\"removed_after_run\": true"));
    assert(json.contains("\"run_save_under_streaming\": true"));
    assert(json.contains("\"destructive_maintenance_busy_while_pinned\": true"));
    assert(json.contains("\"reader_gap_pruned_stale_generation\": true"));
    assert(json.contains("\"saved_delta_source_rotations\": 2"));

    auto failed_gate = report.value();
    failed_gate.config.maximum_file_delta_disk_read_p95_ms = 0.000'000'001;
    assert(!failed_gate.gates_passed());
    failed_gate = report.value();
    failed_gate.save_under_streaming.gates.passed = false;
    failed_gate.save_under_streaming.gates.violations.push_back(
        {"save_submission_ms", failed_gate.save_under_streaming.save_submission_ms, 0.0});
    assert(!failed_gate.gates_passed());
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
    config.physical_saved_delta_record_count = 0;
    assert(!config.validate());
    config.physical_saved_delta_record_count = 5;
    config.repetitions = 0;
    assert(!config.validate());
    config.repetitions = 1;
    config.save_timeout_ms = 0;
    assert(!config.validate());
    config.save_timeout_ms = 5'000;
    config.maximum_save_submission_ms = 0.0;
    assert(!config.validate());
    config.maximum_save_submission_ms = 0.25;
    config.scheduler.worker_count = 0;
    assert(!config.validate());
    assert(!benchmark::run_chunk_streaming_benchmark(config));
    assert(benchmark::chunk_streaming_workload_name(
               static_cast<benchmark::ChunkStreamingWorkload>(255)) == "unknown");
    assert(benchmark::chunk_streaming_workload_name(
               benchmark::ChunkStreamingWorkload::saved_delta_publication) ==
           "saved_delta_publication");
    assert(benchmark::chunk_streaming_workload_name(
               benchmark::ChunkStreamingWorkload::file_delta_warm) == "file_delta_warm");
    assert(benchmark::chunk_streaming_workload_name(
               benchmark::ChunkStreamingWorkload::file_delta_drop_cache_advised) ==
           "file_delta_drop_cache_advised");
}

} // namespace

int main() {
    test_small_benchmark_retains_open_loop_latency_and_bounds();
    test_file_backed_workloads_report_cache_state_and_scale();
    test_invalid_configs_fail_closed();
    return 0;
}
