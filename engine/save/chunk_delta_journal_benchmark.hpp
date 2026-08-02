#pragma once

#include "engine/core/result.hpp"
#include "engine/profiling/runtime_metadata.hpp"
#include "engine/save/save_database.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace heartstead::save::benchmark {

struct ChunkDeltaJournalBenchmarkConfig {
    std::size_t base_record_count = 16'384;
    std::size_t payload_bytes = 88;
    std::uint32_t warmup_append_count = 8;
    std::uint32_t append_repetitions = 128;
    std::uint32_t open_warmup_repetitions = 2;
    std::uint32_t open_repetitions = 9;
    std::filesystem::path fixture_parent;
    bool enforce_gates = false;
    double maximum_initial_writer_open_ms = 250.0;
    double maximum_append_p95_ms = 25.0;
    double maximum_writer_open_p95_ms = 250.0;
    double maximum_reader_open_p95_ms = 250.0;
    double maximum_checkpoint_ms = 75'000.0;
    double maximum_post_checkpoint_reader_open_ms = 250.0;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkDeltaJournalAppendSample {
    std::uint32_t repetition = 0;
    world::ChunkCoord coord;
    std::uint64_t sequence = 0;
    std::size_t payload_bytes = 0;
    std::size_t encoded_bytes = 0;
    std::size_t journal_entry_count_after = 0;
    std::size_t journal_bytes_after = 0;
    double append_ms = 0.0;
};

struct ChunkDeltaJournalFixtureMetadata {
    bool used = false;
    std::filesystem::path ephemeral_root;
    std::string active_generation;
    std::size_t base_record_count = 0;
    std::uint64_t encoded_payload_bytes = 0;
    double generation_write_ms = 0.0;
    bool removed_after_run = false;
};

struct ChunkDeltaJournalCheckpointMeasurement {
    bool compacted = false;
    std::size_t merged_entry_count = 0;
    std::size_t removed_entry_count = 0;
    double elapsed_ms = 0.0;
};

struct ChunkDeltaJournalBenchmarkViolation {
    std::string metric;
    double actual = 0.0;
    double limit = 0.0;
};

struct ChunkDeltaJournalBenchmarkGateEvaluation {
    bool evaluated = false;
    bool passed = true;
    std::vector<ChunkDeltaJournalBenchmarkViolation> violations;
};

struct ChunkDeltaJournalBenchmarkSummary {
    std::size_t append_sample_count = 0;
    double median_append_ms = 0.0;
    double p95_append_ms = 0.0;
    double p99_append_ms = 0.0;
    double maximum_append_ms = 0.0;
    double p95_writer_open_ms = 0.0;
    double p95_reader_open_ms = 0.0;
    double checkpoint_ms = 0.0;
    ChunkDeltaJournalBenchmarkGateEvaluation gates;
};

struct ChunkDeltaJournalBenchmarkReport {
    static constexpr std::uint32_t schema_version = 1;

    ChunkDeltaJournalBenchmarkConfig config;
    profiling::RuntimeMetadata runtime;
    ChunkDeltaJournalFixtureMetadata fixture;
    double initial_writer_open_ms = 0.0;
    std::vector<ChunkDeltaJournalAppendSample> append_samples;
    std::vector<double> writer_open_samples_ms;
    std::vector<double> reader_open_samples_ms;
    std::size_t effective_record_count_before_checkpoint = 0;
    std::size_t journal_entry_count_before_checkpoint = 0;
    std::size_t journal_bytes_before_checkpoint = 0;
    ChunkDeltaJournalCheckpointMeasurement checkpoint;
    double post_checkpoint_reader_open_ms = 0.0;
    std::size_t verified_updated_coordinate_count = 0;
    std::size_t final_effective_record_count = 0;
    std::size_t final_journal_entry_count = 0;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] ChunkDeltaJournalBenchmarkSummary summary() const;
    [[nodiscard]] bool gates_passed() const;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] core::Status write_json(const std::filesystem::path& path) const;
};

[[nodiscard]] core::Result<ChunkDeltaJournalBenchmarkReport>
run_chunk_delta_journal_benchmark(const ChunkDeltaJournalBenchmarkConfig& config);

} // namespace heartstead::save::benchmark
