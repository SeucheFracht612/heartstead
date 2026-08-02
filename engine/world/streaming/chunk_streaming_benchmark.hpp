#pragma once

#include "engine/core/result.hpp"
#include "engine/profiling/runtime_metadata.hpp"
#include "engine/world/streaming/chunk_load_scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::world::benchmark {

enum class ChunkStreamingWorkload : std::uint8_t {
    near_load,
    teleport_recovery,
    saved_delta_publication,
    file_delta_warm,
    file_delta_drop_cache_advised,
};

[[nodiscard]] std::string_view
chunk_streaming_workload_name(ChunkStreamingWorkload workload) noexcept;

struct ChunkStreamingBenchmarkConfig {
    std::vector<ChunkStreamingWorkload> workloads;
    std::uint64_t seed = 0x4853545245414DULL;
    std::uint16_t radius_chunks = 4;
    std::size_t unrelated_history_edit_count = 16'384;
    std::size_t physical_saved_delta_record_count = 16'384;
    std::filesystem::path physical_fixture_parent;
    std::uint32_t warmup_repetitions = 2;
    std::uint32_t repetitions = 9;
    std::uint64_t update_interval_us = 1'000;
    std::uint64_t timeout_ms = 10'000;
    ChunkLoadSchedulerConfig scheduler;
    bool enforce_gates = false;
    double maximum_near_p95_ms = 250.0;
    double maximum_teleport_p95_ms = 1'000.0;
    double maximum_saved_delta_p95_ms = 250.0;
    double maximum_file_delta_p95_ms = 250.0;
    double maximum_file_delta_disk_read_p95_ms = 25.0;
    double maximum_file_delta_reader_open_p95_ms = 100.0;
    std::uint64_t maximum_owner_publication_us = 500;

    ChunkStreamingBenchmarkConfig();

    [[nodiscard]] core::Status validate() const;
};

struct ChunkStreamingBenchmarkSample {
    ChunkStreamingWorkload workload = ChunkStreamingWorkload::near_load;
    std::uint32_t repetition = 0;
    std::uint32_t ordinal = 0;
    ChunkCoord coord;
    std::uint64_t request_id = 0;
    ChunkStreamLoadSource source = ChunkStreamLoadSource::generated;
    std::size_t saved_edit_count = 0;
    std::uint64_t interest_to_publication_us = 0;
    double scheduler_pipeline_ms = 0.0;
    double disk_read_ms = 0.0;
    double decode_ms = 0.0;
    double generation_ms = 0.0;
    double prepare_ms = 0.0;
    double worker_ms = 0.0;
};

struct ChunkStreamingBenchmarkRun {
    ChunkStreamingWorkload workload = ChunkStreamingWorkload::near_load;
    std::uint32_t repetition = 0;
    std::uint64_t desired_chunks = 0;
    std::uint64_t obsolete_requests = 0;
    std::uint64_t submitted_requests = 0;
    std::uint64_t published_requests = 0;
    std::uint64_t cancelled_requests = 0;
    std::uint64_t stale_requests = 0;
    std::uint64_t failed_requests = 0;
    std::uint64_t rejected_requests = 0;
    std::uint64_t off_interest_publications = 0;
    std::uint64_t saved_delta_publications = 0;
    std::uint64_t admission_deferred_updates = 0;
    std::uint64_t item_budget_exhaustions = 0;
    std::uint64_t time_budget_exhaustions = 0;
    std::uint64_t elapsed_us = 0;
    std::uint64_t maximum_publication_time_us = 0;
    std::size_t reserved_working_bytes_high_water = 0;
    std::size_t final_reserved_working_bytes = 0;
    std::size_t initial_edit_count = 0;
    std::size_t final_edit_count = 0;
    std::uint64_t edit_log_cache_rebuilds_during_publication = 0;
    std::size_t physical_indexed_delta_count = 0;
    double delta_reader_open_ms = 0.0;
    std::size_t cache_preloaded_payload_count = 0;
    bool cache_advice_supported = false;
    std::size_t cache_advice_attempted_file_count = 0;
    std::size_t cache_advice_accepted_file_count = 0;
};

struct ChunkStreamingBenchmarkViolation {
    std::string metric;
    double actual = 0.0;
    double limit = 0.0;
};

struct ChunkStreamingBenchmarkGateEvaluation {
    bool evaluated = false;
    bool passed = true;
    std::vector<ChunkStreamingBenchmarkViolation> violations;
};

struct ChunkStreamingBenchmarkSummary {
    ChunkStreamingWorkload workload = ChunkStreamingWorkload::near_load;
    std::size_t run_count = 0;
    std::size_t sample_count = 0;
    double median_interest_to_publication_ms = 0.0;
    double p95_interest_to_publication_ms = 0.0;
    double p99_interest_to_publication_ms = 0.0;
    double maximum_interest_to_publication_ms = 0.0;
    double median_scheduler_pipeline_ms = 0.0;
    double p95_scheduler_pipeline_ms = 0.0;
    double p95_disk_read_ms = 0.0;
    double p95_decode_ms = 0.0;
    double p95_generation_ms = 0.0;
    double p95_prepare_ms = 0.0;
    double p95_worker_ms = 0.0;
    double p95_delta_reader_open_ms = 0.0;
    double mean_chunks_per_second = 0.0;
    std::uint64_t total_cancelled_requests = 0;
    std::uint64_t total_saved_delta_publications = 0;
    std::uint64_t total_admission_deferred_updates = 0;
    std::uint64_t maximum_edit_log_cache_rebuilds_during_publication = 0;
    std::uint64_t maximum_publication_time_us = 0;
    std::size_t reserved_working_bytes_high_water = 0;
    std::size_t total_cache_preloaded_payload_count = 0;
    std::size_t total_cache_advice_attempted_file_count = 0;
    std::size_t total_cache_advice_accepted_file_count = 0;
    ChunkStreamingBenchmarkGateEvaluation gates;
};

struct ChunkStreamingPhysicalFixtureMetadata {
    bool used = false;
    std::filesystem::path ephemeral_root;
    std::string active_generation;
    std::size_t record_count = 0;
    std::uint64_t encoded_payload_bytes = 0;
    double setup_ms = 0.0;
    bool removed_after_run = false;
};

struct ChunkStreamingBenchmarkReport {
    static constexpr std::uint32_t schema_version = 3;

    ChunkStreamingBenchmarkConfig config;
    profiling::RuntimeMetadata runtime;
    ChunkStreamingPhysicalFixtureMetadata physical_fixture;
    std::vector<ChunkStreamingBenchmarkRun> runs;
    std::vector<ChunkStreamingBenchmarkSample> raw_samples;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] std::vector<ChunkStreamingBenchmarkSummary> summaries() const;
    [[nodiscard]] bool gates_passed() const;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] core::Status write_json(const std::filesystem::path& path) const;
};

[[nodiscard]] core::Result<ChunkStreamingBenchmarkReport>
run_chunk_streaming_benchmark(const ChunkStreamingBenchmarkConfig& config);

} // namespace heartstead::world::benchmark
