#include "engine/save/chunk_delta_journal_benchmark.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>

namespace heartstead::save::benchmark {

namespace {

using BenchmarkClock = std::chrono::steady_clock;

constexpr std::size_t maximum_benchmark_record_count = 1'000'000U;
constexpr std::size_t maximum_benchmark_payload_bytes = 16U * 1024U * 1024U;
constexpr std::size_t maximum_benchmark_table_bytes = 256U * 1024U * 1024U;
constexpr std::uint64_t maximum_benchmark_journal_entries = 65'536U;
constexpr world::ChunkCoord benchmark_base_coord{2'000'000, 0, -2'000'000};

[[nodiscard]] double elapsed_milliseconds(BenchmarkClock::time_point begin,
                                          BenchmarkClock::time_point end) noexcept {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

[[nodiscard]] world::ChunkCoord coord_for_index(std::size_t index) noexcept {
    return {benchmark_base_coord.x + static_cast<std::int64_t>(index), benchmark_base_coord.y,
            benchmark_base_coord.z};
}

[[nodiscard]] std::string payload_for_marker(std::size_t payload_bytes, std::uint64_t marker) {
    std::string payload(payload_bytes, 'x');
    for (std::size_t index = 0; index < payload.size(); ++index) {
        const auto value = marker * 131U + static_cast<std::uint64_t>(index) * 17U;
        payload[index] = static_cast<char>('!' + value % 90U);
    }
    return payload;
}

[[nodiscard]] double percentile(const std::vector<double>& sorted, double fraction) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto position = fraction * static_cast<double>(sorted.size() - 1U);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const auto weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

[[nodiscard]] std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const auto character : value) {
        switch (character) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

void write_json_string(std::ostream& output, std::string_view value) {
    output << '"' << json_escape(value) << '"';
}

void write_runtime_metadata(std::ostream& output, const profiling::RuntimeMetadata& runtime) {
    output << "  \"runtime\": {\n";
    const auto string_field = [&output](std::string_view name, std::string_view value,
                                        bool trailing_comma = true) {
        output << "    \"" << name << "\": ";
        write_json_string(output, value);
        output << (trailing_comma ? ",\n" : "\n");
    };
    string_field("engine_version", runtime.engine_version);
    string_field("git_commit", runtime.git_commit);
    string_field("build_configuration", runtime.build_configuration);
    string_field("compiler", runtime.compiler);
    string_field("platform", runtime.platform);
    string_field("architecture", runtime.architecture);
    string_field("operating_system", runtime.operating_system);
    string_field("cpu_model", runtime.cpu_model);
    output << "    \"logical_cpu_count\": " << runtime.logical_cpu_count << ",\n"
           << "    \"git_dirty\": " << (runtime.git_dirty ? "true" : "false") << ",\n"
           << "    \"tracy_enabled\": " << (runtime.tracy_enabled ? "true" : "false") << "\n  },\n";
}

[[nodiscard]] core::Status write_text_file(const std::filesystem::path& path,
                                           std::string_view text) {
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return core::Status::failure("chunk_delta_journal_benchmark.create_directory_failed",
                                         "failed to create benchmark output directory: " +
                                             error.message());
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return core::Status::failure("chunk_delta_journal_benchmark.open_output_failed",
                                     "failed to open benchmark output: " + path.string());
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        return core::Status::failure("chunk_delta_journal_benchmark.write_output_failed",
                                     "failed to write benchmark output: " + path.string());
    }
    return core::Status::ok();
}

[[nodiscard]] core::Result<std::filesystem::path>
create_fixture_root(const std::filesystem::path& configured_parent) {
    std::error_code error;
    auto parent = configured_parent;
    if (parent.empty()) {
        parent = std::filesystem::temp_directory_path(error);
        if (error) {
            return core::Result<std::filesystem::path>::failure(
                "chunk_delta_journal_benchmark.temp_directory_failed", error.message());
        }
    }
    std::filesystem::create_directories(parent, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            "chunk_delta_journal_benchmark.fixture_parent_failed",
            "failed to create fixture parent " + parent.string() + ": " + error.message());
    }

    static std::atomic_uint64_t next_fixture{1};
    const auto nonce = static_cast<std::uint64_t>(BenchmarkClock::now().time_since_epoch().count());
    for (std::uint64_t attempt = 0; attempt < 64; ++attempt) {
        const auto sequence = next_fixture.fetch_add(1, std::memory_order_relaxed);
        const auto candidate = parent / ("heartstead_chunk_delta_journal_" + std::to_string(nonce) +
                                         "_" + std::to_string(sequence));
        const bool created = std::filesystem::create_directory(candidate, error);
        if (error) {
            return core::Result<std::filesystem::path>::failure(
                "chunk_delta_journal_benchmark.fixture_create_failed",
                "failed to create fixture " + candidate.string() + ": " + error.message());
        }
        if (created) {
            return core::Result<std::filesystem::path>::success(candidate);
        }
    }
    return core::Result<std::filesystem::path>::failure(
        "chunk_delta_journal_benchmark.fixture_create_failed",
        "could not allocate a unique physical fixture directory");
}

class ChunkDeltaJournalFixture {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<ChunkDeltaJournalFixture>>
    create(const ChunkDeltaJournalBenchmarkConfig& config) {
        auto root = create_fixture_root(config.fixture_parent);
        if (!root) {
            return core::Result<std::unique_ptr<ChunkDeltaJournalFixture>>::failure(
                root.error().code, root.error().message);
        }
        auto fixture = std::unique_ptr<ChunkDeltaJournalFixture>(
            new ChunkDeltaJournalFixture(std::move(root).value()));

        SaveSnapshot snapshot;
        snapshot.metadata.game_version = "chunk-delta-journal-benchmark";
        snapshot.metadata.world_seed = 0x48535443444AULL;
        snapshot.chunk_edits.reserve(config.base_record_count);
        for (std::size_t index = 0; index < config.base_record_count; ++index) {
            auto payload = payload_for_marker(config.payload_bytes, index);
            fixture->metadata_.encoded_payload_bytes += payload.size();
            snapshot.chunk_edits.push_back({coord_for_index(index), std::move(payload)});
        }

        const auto write_started_at = BenchmarkClock::now();
        const auto status = fixture->database_.write_snapshot(snapshot);
        fixture->metadata_.generation_write_ms =
            elapsed_milliseconds(write_started_at, BenchmarkClock::now());
        if (!status) {
            return core::Result<std::unique_ptr<ChunkDeltaJournalFixture>>::failure(
                status.error().code, status.error().message);
        }
        auto stats = fixture->database_.stats();
        if (!stats) {
            return core::Result<std::unique_ptr<ChunkDeltaJournalFixture>>::failure(
                stats.error().code, stats.error().message);
        }
        if (stats.value().active_generation.empty() ||
            stats.value().chunk_delta_count != config.base_record_count ||
            stats.value().chunk_delta_bytes != fixture->metadata_.encoded_payload_bytes) {
            return core::Result<std::unique_ptr<ChunkDeltaJournalFixture>>::failure(
                "chunk_delta_journal_benchmark.fixture_incomplete",
                "physical fixture did not commit every base chunk delta");
        }

        fixture->metadata_.used = true;
        fixture->metadata_.ephemeral_root = fixture->root_;
        fixture->metadata_.active_generation = stats.value().active_generation;
        fixture->metadata_.base_record_count = stats.value().chunk_delta_count;
        return core::Result<std::unique_ptr<ChunkDeltaJournalFixture>>::success(std::move(fixture));
    }

    ChunkDeltaJournalFixture(const ChunkDeltaJournalFixture&) = delete;
    ChunkDeltaJournalFixture& operator=(const ChunkDeltaJournalFixture&) = delete;

    ~ChunkDeltaJournalFixture() {
        static_cast<void>(remove());
    }

    [[nodiscard]] core::Status remove() noexcept {
        if (metadata_.removed_after_run) {
            return core::Status::ok();
        }
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        if (error) {
            return core::Status::failure("chunk_delta_journal_benchmark.fixture_cleanup_failed",
                                         "failed to remove fixture " + root_.string() + ": " +
                                             error.message());
        }
        metadata_.removed_after_run = true;
        return core::Status::ok();
    }

    [[nodiscard]] FileSaveDatabase& database() noexcept {
        return database_;
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

    [[nodiscard]] const ChunkDeltaJournalFixtureMetadata& metadata() const noexcept {
        return metadata_;
    }

  private:
    explicit ChunkDeltaJournalFixture(std::filesystem::path root)
        : root_(std::move(root)), database_(root_) {}

    std::filesystem::path root_;
    FileSaveDatabase database_;
    ChunkDeltaJournalFixtureMetadata metadata_;
};

[[nodiscard]] bool valid_timing(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

} // namespace

core::Status ChunkDeltaJournalBenchmarkConfig::validate() const {
    if (base_record_count == 0 || base_record_count > maximum_benchmark_record_count) {
        return core::Status::failure("chunk_delta_journal_benchmark.invalid_base_record_count",
                                     "base record count must be between one and one million");
    }
    if (payload_bytes == 0 || payload_bytes > maximum_benchmark_payload_bytes ||
        base_record_count > maximum_benchmark_table_bytes / payload_bytes) {
        return core::Status::failure(
            "chunk_delta_journal_benchmark.invalid_payload_size",
            "benchmark payloads must be non-empty, individually bounded, and at most 256 MiB in "
            "aggregate");
    }
    const auto total_append_count = static_cast<std::uint64_t>(warmup_append_count) +
                                    static_cast<std::uint64_t>(append_repetitions);
    if (append_repetitions == 0 || total_append_count > maximum_benchmark_journal_entries) {
        return core::Status::failure(
            "chunk_delta_journal_benchmark.invalid_append_count",
            "retained appends must be non-empty and fit the chunk journal entry budget");
    }
    if (open_repetitions == 0 || open_repetitions > 100 || open_warmup_repetitions > 100) {
        return core::Status::failure("chunk_delta_journal_benchmark.invalid_open_repetitions",
                                     "open repetitions must be 1..100 and warmups 0..100");
    }
    if (!std::isfinite(maximum_initial_writer_open_ms) || maximum_initial_writer_open_ms <= 0.0 ||
        !std::isfinite(maximum_append_p95_ms) || maximum_append_p95_ms <= 0.0 ||
        !std::isfinite(maximum_writer_open_p95_ms) || maximum_writer_open_p95_ms <= 0.0 ||
        !std::isfinite(maximum_reader_open_p95_ms) || maximum_reader_open_p95_ms <= 0.0 ||
        !std::isfinite(maximum_checkpoint_ms) || maximum_checkpoint_ms <= 0.0 ||
        !std::isfinite(maximum_post_checkpoint_reader_open_ms) ||
        maximum_post_checkpoint_reader_open_ms <= 0.0) {
        return core::Status::failure("chunk_delta_journal_benchmark.invalid_gates",
                                     "chunk delta persistence gates must be finite and positive");
    }
    return core::Status::ok();
}

core::Status ChunkDeltaJournalBenchmarkReport::validate() const {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    if (!fixture.used || fixture.ephemeral_root.empty() || fixture.active_generation.empty() ||
        fixture.base_record_count != config.base_record_count ||
        fixture.encoded_payload_bytes != config.base_record_count * config.payload_bytes ||
        !valid_timing(fixture.generation_write_ms) || fixture.generation_write_ms == 0.0 ||
        !fixture.removed_after_run) {
        return core::Status::failure(
            "chunk_delta_journal_benchmark.invalid_fixture",
            "chunk delta journal report has incomplete physical-fixture metadata");
    }
    if (!valid_timing(initial_writer_open_ms) ||
        append_samples.size() != config.append_repetitions ||
        writer_open_samples_ms.size() != config.open_repetitions ||
        reader_open_samples_ms.size() != config.open_repetitions) {
        return core::Status::failure(
            "chunk_delta_journal_benchmark.incomplete_samples",
            "chunk delta journal report does not contain every retained timing sample");
    }

    std::size_t previous_journal_bytes = 0;
    for (std::size_t index = 0; index < append_samples.size(); ++index) {
        const auto& sample = append_samples[index];
        const auto absolute_index = static_cast<std::uint64_t>(config.warmup_append_count) + index;
        const auto expected_sequence = absolute_index + 1U;
        const auto expected_coord =
            coord_for_index(static_cast<std::size_t>(absolute_index % config.base_record_count));
        if (sample.repetition != index || sample.coord != expected_coord ||
            sample.sequence != expected_sequence || sample.payload_bytes != config.payload_bytes ||
            sample.encoded_bytes <= sample.payload_bytes ||
            sample.journal_entry_count_after != expected_sequence ||
            sample.journal_bytes_after <= previous_journal_bytes ||
            !valid_timing(sample.append_ms)) {
            return core::Status::failure(
                "chunk_delta_journal_benchmark.invalid_append_sample",
                "chunk delta journal report contains an invalid append timing sample");
        }
        previous_journal_bytes = sample.journal_bytes_after;
    }
    if (!std::ranges::all_of(writer_open_samples_ms, valid_timing) ||
        !std::ranges::all_of(reader_open_samples_ms, valid_timing)) {
        return core::Status::failure(
            "chunk_delta_journal_benchmark.invalid_open_sample",
            "chunk delta journal report contains an invalid reader or writer open sample");
    }

    const auto total_entries = static_cast<std::size_t>(config.warmup_append_count) +
                               static_cast<std::size_t>(config.append_repetitions);
    const auto expected_verified = std::min(config.base_record_count, total_entries);
    if (effective_record_count_before_checkpoint != config.base_record_count ||
        journal_entry_count_before_checkpoint != total_entries ||
        journal_bytes_before_checkpoint == 0 || !checkpoint.compacted ||
        checkpoint.merged_entry_count != total_entries ||
        checkpoint.removed_entry_count != total_entries || !valid_timing(checkpoint.elapsed_ms) ||
        checkpoint.elapsed_ms == 0.0 || !valid_timing(post_checkpoint_reader_open_ms) ||
        verified_updated_coordinate_count != expected_verified ||
        final_effective_record_count != config.base_record_count ||
        final_journal_entry_count != 0) {
        return core::Status::failure(
            "chunk_delta_journal_benchmark.invalid_checkpoint",
            "chunk delta journal report did not checkpoint and verify the exact effective table");
    }
    return core::Status::ok();
}

ChunkDeltaJournalBenchmarkSummary ChunkDeltaJournalBenchmarkReport::summary() const {
    ChunkDeltaJournalBenchmarkSummary result;
    std::vector<double> append_ms;
    append_ms.reserve(append_samples.size());
    for (const auto& sample : append_samples) {
        append_ms.push_back(sample.append_ms);
    }
    std::ranges::sort(append_ms);
    auto writer_open_ms = writer_open_samples_ms;
    auto reader_open_ms = reader_open_samples_ms;
    std::ranges::sort(writer_open_ms);
    std::ranges::sort(reader_open_ms);

    result.append_sample_count = append_ms.size();
    result.median_append_ms = percentile(append_ms, 0.50);
    result.p95_append_ms = percentile(append_ms, 0.95);
    result.p99_append_ms = percentile(append_ms, 0.99);
    result.maximum_append_ms = append_ms.empty() ? 0.0 : append_ms.back();
    result.p95_writer_open_ms = percentile(writer_open_ms, 0.95);
    result.p95_reader_open_ms = percentile(reader_open_ms, 0.95);
    result.checkpoint_ms = checkpoint.elapsed_ms;

    result.gates.evaluated = config.enforce_gates;
    if (config.enforce_gates) {
        const auto check = [&result](std::string metric, double actual, double limit) {
            if (!std::isfinite(actual) || actual > limit) {
                result.gates.violations.push_back({std::move(metric), actual, limit});
            }
        };
        check("initial_writer_open_ms", initial_writer_open_ms,
              config.maximum_initial_writer_open_ms);
        check("p95_append_ms", result.p95_append_ms, config.maximum_append_p95_ms);
        check("p95_writer_open_ms", result.p95_writer_open_ms, config.maximum_writer_open_p95_ms);
        check("p95_reader_open_ms", result.p95_reader_open_ms, config.maximum_reader_open_p95_ms);
        check("checkpoint_ms", result.checkpoint_ms, config.maximum_checkpoint_ms);
        check("post_checkpoint_reader_open_ms", post_checkpoint_reader_open_ms,
              config.maximum_post_checkpoint_reader_open_ms);
        result.gates.passed = result.gates.violations.empty();
    }
    return result;
}

bool ChunkDeltaJournalBenchmarkReport::gates_passed() const {
    return summary().gates.passed;
}

std::string ChunkDeltaJournalBenchmarkReport::to_json() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "{\n"
           << "  \"schema_version\": " << schema_version << ",\n"
           << "  \"benchmark\": \"chunk_delta_journal\",\n"
           << "  \"config\": {\n"
           << "    \"base_record_count\": " << config.base_record_count << ",\n"
           << "    \"payload_bytes\": " << config.payload_bytes << ",\n"
           << "    \"warmup_append_count\": " << config.warmup_append_count << ",\n"
           << "    \"append_repetitions\": " << config.append_repetitions << ",\n"
           << "    \"open_warmup_repetitions\": " << config.open_warmup_repetitions << ",\n"
           << "    \"open_repetitions\": " << config.open_repetitions << ",\n"
           << "    \"fixture_parent\": ";
    write_json_string(output, config.fixture_parent.string());
    output << ",\n"
           << "    \"enforce_gates\": " << (config.enforce_gates ? "true" : "false") << ",\n"
           << "    \"maximum_initial_writer_open_ms\": " << config.maximum_initial_writer_open_ms
           << ",\n"
           << "    \"maximum_append_p95_ms\": " << config.maximum_append_p95_ms << ",\n"
           << "    \"maximum_writer_open_p95_ms\": " << config.maximum_writer_open_p95_ms << ",\n"
           << "    \"maximum_reader_open_p95_ms\": " << config.maximum_reader_open_p95_ms << ",\n"
           << "    \"maximum_checkpoint_ms\": " << config.maximum_checkpoint_ms << ",\n"
           << "    \"maximum_post_checkpoint_reader_open_ms\": "
           << config.maximum_post_checkpoint_reader_open_ms << "\n"
           << "  },\n";
    write_runtime_metadata(output, runtime);

    output << "  \"fixture\": {\"used\": " << (fixture.used ? "true" : "false")
           << ", \"ephemeral_root\": ";
    write_json_string(output, fixture.ephemeral_root.string());
    output << ", \"active_generation\": ";
    write_json_string(output, fixture.active_generation);
    output << ", \"base_record_count\": " << fixture.base_record_count
           << ", \"encoded_payload_bytes\": " << fixture.encoded_payload_bytes
           << ", \"generation_write_ms\": " << fixture.generation_write_ms
           << ", \"removed_after_run\": " << (fixture.removed_after_run ? "true" : "false")
           << "},\n";

    output << "  \"initial_writer_open_ms\": " << initial_writer_open_ms << ",\n"
           << "  \"append_samples\": [\n";
    for (std::size_t index = 0; index < append_samples.size(); ++index) {
        const auto& sample = append_samples[index];
        output << "    {\"repetition\": " << sample.repetition << ", \"coord\": [" << sample.coord.x
               << ", " << sample.coord.y << ", " << sample.coord.z
               << "], \"sequence\": " << sample.sequence
               << ", \"payload_bytes\": " << sample.payload_bytes
               << ", \"encoded_bytes\": " << sample.encoded_bytes
               << ", \"journal_entry_count_after\": " << sample.journal_entry_count_after
               << ", \"journal_bytes_after\": " << sample.journal_bytes_after
               << ", \"append_ms\": " << sample.append_ms << '}'
               << (index + 1U == append_samples.size() ? "\n" : ",\n");
    }
    output << "  ],\n"
           << "  \"writer_open_samples_ms\": [";
    for (std::size_t index = 0; index < writer_open_samples_ms.size(); ++index) {
        output << (index == 0 ? "" : ", ") << writer_open_samples_ms[index];
    }
    output << "],\n"
           << "  \"reader_open_samples_ms\": [";
    for (std::size_t index = 0; index < reader_open_samples_ms.size(); ++index) {
        output << (index == 0 ? "" : ", ") << reader_open_samples_ms[index];
    }
    output << "],\n"
           << "  \"effective_record_count_before_checkpoint\": "
           << effective_record_count_before_checkpoint << ",\n"
           << "  \"journal_entry_count_before_checkpoint\": "
           << journal_entry_count_before_checkpoint << ",\n"
           << "  \"journal_bytes_before_checkpoint\": " << journal_bytes_before_checkpoint << ",\n"
           << "  \"checkpoint\": {\"compacted\": " << (checkpoint.compacted ? "true" : "false")
           << ", \"merged_entry_count\": " << checkpoint.merged_entry_count
           << ", \"removed_entry_count\": " << checkpoint.removed_entry_count
           << ", \"elapsed_ms\": " << checkpoint.elapsed_ms << "},\n"
           << "  \"post_checkpoint_reader_open_ms\": " << post_checkpoint_reader_open_ms << ",\n"
           << "  \"verified_updated_coordinate_count\": " << verified_updated_coordinate_count
           << ",\n"
           << "  \"final_effective_record_count\": " << final_effective_record_count << ",\n"
           << "  \"final_journal_entry_count\": " << final_journal_entry_count << ",\n";

    const auto summary_value = summary();
    output << "  \"summary\": {\"append_sample_count\": " << summary_value.append_sample_count
           << ", \"median_append_ms\": " << summary_value.median_append_ms
           << ", \"p95_append_ms\": " << summary_value.p95_append_ms
           << ", \"p99_append_ms\": " << summary_value.p99_append_ms
           << ", \"maximum_append_ms\": " << summary_value.maximum_append_ms
           << ", \"p95_writer_open_ms\": " << summary_value.p95_writer_open_ms
           << ", \"p95_reader_open_ms\": " << summary_value.p95_reader_open_ms
           << ", \"checkpoint_ms\": " << summary_value.checkpoint_ms
           << ", \"gates\": {\"evaluated\": " << (summary_value.gates.evaluated ? "true" : "false")
           << ", \"passed\": " << (summary_value.gates.passed ? "true" : "false")
           << ", \"violations\": [";
    for (std::size_t index = 0; index < summary_value.gates.violations.size(); ++index) {
        const auto& violation = summary_value.gates.violations[index];
        output << (index == 0 ? "" : ", ") << "{\"metric\": ";
        write_json_string(output, violation.metric);
        output << ", \"actual\": " << violation.actual << ", \"limit\": " << violation.limit << '}';
    }
    output << "]}}\n}\n";
    return output.str();
}

core::Status ChunkDeltaJournalBenchmarkReport::write_json(const std::filesystem::path& path) const {
    auto status = validate();
    if (!status) {
        return status;
    }
    return write_text_file(path, to_json());
}

core::Result<ChunkDeltaJournalBenchmarkReport>
run_chunk_delta_journal_benchmark(const ChunkDeltaJournalBenchmarkConfig& config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(status.error().code,
                                                                       status.error().message);
    }

    ChunkDeltaJournalBenchmarkReport report;
    report.config = config;
    report.runtime = profiling::query_runtime_metadata();

    auto created_fixture = ChunkDeltaJournalFixture::create(config);
    if (!created_fixture) {
        return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(
            created_fixture.error().code, created_fixture.error().message);
    }
    auto fixture = std::move(created_fixture).value();
    report.fixture = fixture->metadata();
    auto& database = fixture->database();
    std::map<world::ChunkCoord, std::string> expected_updates;

    {
        const auto open_started_at = BenchmarkClock::now();
        auto opened_writer = database.open_chunk_delta_writer();
        report.initial_writer_open_ms =
            elapsed_milliseconds(open_started_at, BenchmarkClock::now());
        if (!opened_writer) {
            return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(
                opened_writer.error().code, opened_writer.error().message);
        }
        auto writer = std::move(opened_writer).value();
        const auto total_appends = static_cast<std::uint64_t>(config.warmup_append_count) +
                                   static_cast<std::uint64_t>(config.append_repetitions);
        report.append_samples.reserve(config.append_repetitions);
        for (std::uint64_t index = 0; index < total_appends; ++index) {
            const auto coord =
                coord_for_index(static_cast<std::size_t>(index % config.base_record_count));
            auto payload =
                payload_for_marker(config.payload_bytes, config.base_record_count + index);
            const auto append_started_at = BenchmarkClock::now();
            auto receipt = writer.write_chunk_delta({coord, payload});
            const auto append_ms = elapsed_milliseconds(append_started_at, BenchmarkClock::now());
            if (!receipt) {
                return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(
                    receipt.error().code, receipt.error().message);
            }
            expected_updates.insert_or_assign(coord, std::move(payload));
            if (index >= config.warmup_append_count) {
                report.append_samples.push_back(
                    {static_cast<std::uint32_t>(index - config.warmup_append_count), coord,
                     receipt.value().sequence, config.payload_bytes, receipt.value().encoded_bytes,
                     writer.stats().journal_entry_count, writer.stats().journal_bytes, append_ms});
            }
        }
        report.effective_record_count_before_checkpoint =
            writer.stats().effective_chunk_delta_count;
        report.journal_entry_count_before_checkpoint = writer.stats().journal_entry_count;
        report.journal_bytes_before_checkpoint = writer.stats().journal_bytes;
    }

    const auto total_open_passes = config.open_warmup_repetitions + config.open_repetitions;
    report.writer_open_samples_ms.reserve(config.open_repetitions);
    for (std::uint32_t pass = 0; pass < total_open_passes; ++pass) {
        const auto open_started_at = BenchmarkClock::now();
        auto writer = database.open_chunk_delta_writer();
        const auto open_ms = elapsed_milliseconds(open_started_at, BenchmarkClock::now());
        if (!writer) {
            return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(writer.error().code,
                                                                           writer.error().message);
        }
        if (writer.value().stats().effective_chunk_delta_count != config.base_record_count ||
            writer.value().stats().journal_entry_count !=
                report.journal_entry_count_before_checkpoint) {
            return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(
                "chunk_delta_journal_benchmark.writer_open_mismatch",
                "retained writer reopen did not recover the exact journal state");
        }
        if (pass >= config.open_warmup_repetitions) {
            report.writer_open_samples_ms.push_back(open_ms);
        }
    }

    report.reader_open_samples_ms.reserve(config.open_repetitions);
    for (std::uint32_t pass = 0; pass < total_open_passes; ++pass) {
        const auto open_started_at = BenchmarkClock::now();
        auto reader = database.open_chunk_delta_reader();
        const auto open_ms = elapsed_milliseconds(open_started_at, BenchmarkClock::now());
        if (!reader) {
            return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(reader.error().code,
                                                                           reader.error().message);
        }
        if (reader.value().stats().indexed_chunk_delta_count != config.base_record_count ||
            reader.value().stats().journal_entry_count !=
                report.journal_entry_count_before_checkpoint) {
            return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(
                "chunk_delta_journal_benchmark.reader_open_mismatch",
                "reader reopen did not pin the exact journal end mark");
        }
        if (pass >= config.open_warmup_repetitions) {
            report.reader_open_samples_ms.push_back(open_ms);
        }
    }

    const auto checkpoint_started_at = BenchmarkClock::now();
    auto checkpoint = database.compact_chunk_delta_journal();
    report.checkpoint.elapsed_ms =
        elapsed_milliseconds(checkpoint_started_at, BenchmarkClock::now());
    if (!checkpoint) {
        return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(checkpoint.error().code,
                                                                       checkpoint.error().message);
    }
    report.checkpoint.compacted = checkpoint.value().compacted;
    report.checkpoint.merged_entry_count = checkpoint.value().merged_entry_count;
    report.checkpoint.removed_entry_count = checkpoint.value().removed_entry_count;

    FileSaveDatabase restarted(fixture->root());
    const auto post_checkpoint_open_started_at = BenchmarkClock::now();
    auto post_checkpoint_reader = restarted.open_chunk_delta_reader();
    report.post_checkpoint_reader_open_ms =
        elapsed_milliseconds(post_checkpoint_open_started_at, BenchmarkClock::now());
    if (!post_checkpoint_reader) {
        return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(
            post_checkpoint_reader.error().code, post_checkpoint_reader.error().message);
    }
    if (post_checkpoint_reader.value().stats().indexed_chunk_delta_count !=
            config.base_record_count ||
        post_checkpoint_reader.value().stats().journal_entry_count != 0) {
        return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(
            "chunk_delta_journal_benchmark.post_checkpoint_mismatch",
            "post-checkpoint reader did not select the compacted base table");
    }
    for (const auto& [coord, expected_payload] : expected_updates) {
        auto record = post_checkpoint_reader.value().read_chunk_delta(coord);
        if (!record || !record.value().has_value() ||
            record.value()->encoded_edit_delta != expected_payload) {
            return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(
                record ? "chunk_delta_journal_benchmark.verification_mismatch"
                       : record.error().code,
                record ? "checkpoint did not preserve the latest accepted chunk delta"
                       : record.error().message);
        }
        ++report.verified_updated_coordinate_count;
    }

    auto final_stats = restarted.stats();
    if (!final_stats) {
        return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(final_stats.error().code,
                                                                       final_stats.error().message);
    }
    report.final_effective_record_count = final_stats.value().chunk_delta_count;
    report.final_journal_entry_count = final_stats.value().chunk_delta_journal_entry_count;

    status = fixture->remove();
    if (!status) {
        return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(status.error().code,
                                                                       status.error().message);
    }
    report.fixture = fixture->metadata();
    status = report.validate();
    if (!status) {
        return core::Result<ChunkDeltaJournalBenchmarkReport>::failure(status.error().code,
                                                                       status.error().message);
    }
    return core::Result<ChunkDeltaJournalBenchmarkReport>::success(std::move(report));
}

} // namespace heartstead::save::benchmark
