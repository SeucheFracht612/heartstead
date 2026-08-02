#include "engine/save/chunk_delta_journal_benchmark.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

namespace benchmark = heartstead::save::benchmark;

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("heartstead_chunk_delta_journal_benchmark_test_" + std::to_string(nonce) +
                     "_" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
        }
        assert(false && "could not create chunk delta journal benchmark test directory");
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

void test_small_physical_benchmark_retains_timings_and_verifies_checkpoint() {
    TemporaryDirectory temporary;
    benchmark::ChunkDeltaJournalBenchmarkConfig config;
    config.base_record_count = 8;
    config.payload_bytes = 32;
    config.warmup_append_count = 2;
    config.append_repetitions = 5;
    config.open_warmup_repetitions = 0;
    config.open_repetitions = 2;
    config.fixture_parent = temporary.path();
    config.enforce_gates = true;
    config.maximum_initial_writer_open_ms = 1'000'000.0;
    config.maximum_append_p95_ms = 1'000'000.0;
    config.maximum_writer_open_p95_ms = 1'000'000.0;
    config.maximum_reader_open_p95_ms = 1'000'000.0;
    config.maximum_checkpoint_ms = 1'000'000.0;
    config.maximum_post_checkpoint_reader_open_ms = 1'000'000.0;

    auto report = benchmark::run_chunk_delta_journal_benchmark(config);
    assert(report);
    assert(report.value().validate());
    assert(report.value().gates_passed());
    assert(report.value().fixture.used);
    assert(report.value().fixture.base_record_count == 8);
    assert(report.value().fixture.encoded_payload_bytes == 256);
    assert(report.value().fixture.generation_write_ms > 0.0);
    assert(report.value().fixture.removed_after_run);
    assert(!std::filesystem::exists(report.value().fixture.ephemeral_root));
    assert(report.value().append_samples.size() == 5);
    assert(report.value().append_samples.front().sequence == 3);
    assert(report.value().append_samples.back().sequence == 7);
    assert(report.value().writer_open_samples_ms.size() == 2);
    assert(report.value().reader_open_samples_ms.size() == 2);
    assert(report.value().effective_record_count_before_checkpoint == 8);
    assert(report.value().journal_entry_count_before_checkpoint == 7);
    assert(report.value().journal_bytes_before_checkpoint > 7 * config.payload_bytes);
    assert(report.value().checkpoint.compacted);
    assert(report.value().checkpoint.merged_entry_count == 7);
    assert(report.value().checkpoint.removed_entry_count == 7);
    assert(report.value().verified_updated_coordinate_count == 7);
    assert(report.value().final_effective_record_count == 8);
    assert(report.value().final_journal_entry_count == 0);

    const auto summary = report.value().summary();
    assert(summary.append_sample_count == 5);
    assert(std::isfinite(summary.p95_append_ms));
    assert(std::isfinite(summary.p95_writer_open_ms));
    assert(std::isfinite(summary.p95_reader_open_ms));
    assert(summary.checkpoint_ms > 0.0);
    assert(summary.gates.evaluated);
    assert(summary.gates.passed);

    auto failed_gate = report.value();
    failed_gate.config.maximum_append_p95_ms = 1e-12;
    assert(!failed_gate.gates_passed());

    const auto json = report.value().to_json();
    assert(json.contains("\"schema_version\": 1"));
    assert(json.contains("\"benchmark\": \"chunk_delta_journal\""));
    assert(json.contains("\"append_samples\""));
    assert(json.contains("\"journal_entry_count_before_checkpoint\": 7"));
    assert(json.contains("\"removed_after_run\": true"));

    const auto output_path = temporary.path() / "report.json";
    assert(report.value().write_json(output_path));
    std::ifstream input(output_path, std::ios::binary);
    const std::string persisted{std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{}};
    assert(persisted == json);
}

void test_invalid_configs_fail_closed() {
    benchmark::ChunkDeltaJournalBenchmarkConfig config;
    config.base_record_count = 0;
    assert(!config.validate());
    config.base_record_count = 1;
    config.payload_bytes = 0;
    assert(!config.validate());
    config.payload_bytes = 1;
    config.append_repetitions = 0;
    assert(!config.validate());
    config.append_repetitions = 1;
    config.open_repetitions = 0;
    assert(!config.validate());
    config.open_repetitions = 1;
    config.maximum_checkpoint_ms = 0.0;
    assert(!config.validate());
    assert(!benchmark::run_chunk_delta_journal_benchmark(config));
}

} // namespace

int main() {
    test_small_physical_benchmark_retains_timings_and_verifies_checkpoint();
    test_invalid_configs_fail_closed();
    return 0;
}
