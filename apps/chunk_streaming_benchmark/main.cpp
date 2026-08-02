#include "engine/core/process_entry.hpp"
#include "engine/world/streaming/chunk_streaming_benchmark.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace benchmark = heartstead::world::benchmark;
namespace core = heartstead::core;

struct Options {
    benchmark::ChunkStreamingBenchmarkConfig benchmark;
    std::filesystem::path output;
    bool help = false;
};

template <typename Value> [[nodiscard]] std::optional<Value> parse_number(std::string_view text) {
    Value result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::optional<benchmark::ChunkStreamingWorkload>
parse_workload(std::string_view name) noexcept {
    if (name == "near_load") {
        return benchmark::ChunkStreamingWorkload::near_load;
    }
    if (name == "teleport_recovery") {
        return benchmark::ChunkStreamingWorkload::teleport_recovery;
    }
    if (name == "saved_delta_publication") {
        return benchmark::ChunkStreamingWorkload::saved_delta_publication;
    }
    if (name == "file_delta_warm") {
        return benchmark::ChunkStreamingWorkload::file_delta_warm;
    }
    if (name == "file_delta_drop_cache_advised") {
        return benchmark::ChunkStreamingWorkload::file_delta_drop_cache_advised;
    }
    return std::nullopt;
}

[[nodiscard]] core::Result<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const auto argument = std::string_view(argv[index]);
        const auto next = [&]() -> core::Result<std::string_view> {
            if (index + 1 >= argc) {
                return core::Result<std::string_view>::failure(
                    "chunk_streaming_benchmark.missing_value",
                    std::string(argument) + " requires a value");
            }
            return core::Result<std::string_view>::success(argv[++index]);
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--enforce-gates") {
            options.benchmark.enforce_gates = true;
        } else if (argument == "--save-under-streaming") {
            options.benchmark.run_save_under_streaming = true;
        } else if (argument == "--workload") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            if (value.value() == "all") {
                options.benchmark.workloads = {
                    benchmark::ChunkStreamingWorkload::near_load,
                    benchmark::ChunkStreamingWorkload::teleport_recovery,
                    benchmark::ChunkStreamingWorkload::saved_delta_publication,
                    benchmark::ChunkStreamingWorkload::file_delta_warm,
                };
#if defined(__linux__)
                options.benchmark.workloads.push_back(
                    benchmark::ChunkStreamingWorkload::file_delta_drop_cache_advised);
#endif
            } else {
                const auto parsed = parse_workload(value.value());
                if (!parsed) {
                    return core::Result<Options>::failure(
                        "chunk_streaming_benchmark.invalid_workload",
                        "--workload must be near_load, teleport_recovery, "
                        "saved_delta_publication, file_delta_warm, "
                        "file_delta_drop_cache_advised, or all");
                }
                options.benchmark.workloads = {*parsed};
            }
        } else if (argument == "--seed" || argument == "--update-us" ||
                   argument == "--timeout-ms" || argument == "--save-timeout-ms" ||
                   argument == "--owner-publication-us") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint64_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("chunk_streaming_benchmark.invalid_integer",
                                                      std::string(argument) +
                                                          " must be an unsigned integer");
            }
            if (argument == "--seed") {
                options.benchmark.seed = *parsed;
            } else if (argument == "--update-us") {
                options.benchmark.update_interval_us = *parsed;
            } else if (argument == "--timeout-ms") {
                options.benchmark.timeout_ms = *parsed;
            } else if (argument == "--save-timeout-ms") {
                options.benchmark.save_timeout_ms = *parsed;
            } else {
                options.benchmark.maximum_owner_publication_us = *parsed;
                options.benchmark.scheduler.max_publication_time_us = *parsed;
            }
        } else if (argument == "--radius" || argument == "--warmup" ||
                   argument == "--repetitions" || argument == "--workers" ||
                   argument == "--publications" || argument == "--history-edits" ||
                   argument == "--physical-records") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint32_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("chunk_streaming_benchmark.invalid_count",
                                                      std::string(argument) +
                                                          " must be an unsigned integer");
            }
            if (argument == "--radius") {
                if (*parsed > std::numeric_limits<std::uint16_t>::max()) {
                    return core::Result<Options>::failure(
                        "chunk_streaming_benchmark.invalid_radius", "--radius is out of range");
                }
                options.benchmark.radius_chunks = static_cast<std::uint16_t>(*parsed);
            } else if (argument == "--warmup") {
                options.benchmark.warmup_repetitions = *parsed;
            } else if (argument == "--repetitions") {
                options.benchmark.repetitions = *parsed;
            } else if (argument == "--workers") {
                options.benchmark.scheduler.worker_count = *parsed;
            } else if (argument == "--history-edits") {
                options.benchmark.unrelated_history_edit_count = *parsed;
            } else if (argument == "--physical-records") {
                options.benchmark.physical_saved_delta_record_count = *parsed;
            } else {
                options.benchmark.scheduler.max_publications_per_update = *parsed;
            }
        } else if (argument == "--near-p95-ms" || argument == "--teleport-p95-ms" ||
                   argument == "--saved-delta-p95-ms" || argument == "--file-delta-p95-ms" ||
                   argument == "--file-disk-read-p95-ms" ||
                   argument == "--file-reader-open-p95-ms" ||
                   argument == "--save-streaming-p95-ms" || argument == "--save-submission-ms" ||
                   argument == "--save-durable-ms" || argument == "--save-compaction-ms") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<double>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("chunk_streaming_benchmark.invalid_latency",
                                                      std::string(argument) +
                                                          " must be a finite positive number");
            }
            if (argument == "--near-p95-ms") {
                options.benchmark.maximum_near_p95_ms = *parsed;
            } else if (argument == "--teleport-p95-ms") {
                options.benchmark.maximum_teleport_p95_ms = *parsed;
            } else if (argument == "--saved-delta-p95-ms") {
                options.benchmark.maximum_saved_delta_p95_ms = *parsed;
            } else if (argument == "--file-delta-p95-ms") {
                options.benchmark.maximum_file_delta_p95_ms = *parsed;
            } else if (argument == "--file-disk-read-p95-ms") {
                options.benchmark.maximum_file_delta_disk_read_p95_ms = *parsed;
            } else if (argument == "--file-reader-open-p95-ms") {
                options.benchmark.maximum_file_delta_reader_open_p95_ms = *parsed;
            } else if (argument == "--save-streaming-p95-ms") {
                options.benchmark.maximum_save_under_streaming_p95_ms = *parsed;
            } else if (argument == "--save-submission-ms") {
                options.benchmark.maximum_save_submission_ms = *parsed;
            } else if (argument == "--save-durable-ms") {
                options.benchmark.maximum_save_durable_acceptance_ms = *parsed;
            } else {
                options.benchmark.maximum_save_compaction_ms = *parsed;
            }
        } else if (argument == "--physical-fixture-parent") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.benchmark.physical_fixture_parent = value.value();
        } else if (argument == "--output") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.output = value.value();
        } else {
            return core::Result<Options>::failure("chunk_streaming_benchmark.unknown_option",
                                                  "unknown chunk streaming benchmark option: " +
                                                      std::string(argument));
        }
    }
    auto status = options.benchmark.validate();
    if (!status) {
        return core::Result<Options>::failure(status.error().code, status.error().message);
    }
    return core::Result<Options>::success(std::move(options));
}

void print_usage(std::ostream& output) {
    output << "usage: heartstead_chunk_streaming_benchmark [options]\n"
              "  --workload NAME          near_load, teleport_recovery, saved_delta_publication,\n"
              "                           file_delta_warm, "
              "file_delta_drop_cache_advised (Linux), or all supported\n"
              "  --seed N                 Deterministic terrain seed\n"
              "  --radius N               Circular required-ring radius (default 4)\n"
              "  --history-edits N        Unrelated retained edits for saved-delta work "
              "(default 16384)\n"
              "  --physical-records N     Records in the file-backed delta table (default 16384)\n"
              "  --physical-fixture-parent PATH\n"
              "                           Parent volume for the ephemeral physical fixture\n"
              "  --warmup N               Unmeasured workload runs (default 2)\n"
              "  --repetitions N          Retained workload runs (default 9)\n"
              "  --update-us N            Owner publication cadence (default 1000)\n"
              "  --timeout-ms N           Per-run fail-closed timeout (default 10000)\n"
              "  --save-under-streaming   Run one physical load ring while publishing a save\n"
              "  --save-timeout-ms N      Save phase fail-closed timeout (default 90000)\n"
              "  --workers N              Chunk-load worker count (default 2)\n"
              "  --publications N         Maximum results per owner update (default 2)\n"
              "  --near-p95-ms N          Near-ring P95 gate (default 250)\n"
              "  --teleport-p95-ms N      Teleport-ring P95 gate (default 1000)\n"
              "  --saved-delta-p95-ms N   Saved-delta P95 gate (default 250)\n"
              "  --file-delta-p95-ms N    File-backed saved-delta P95 gate (default 250)\n"
              "  --file-disk-read-p95-ms N\n"
              "                           File payload-read P95 gate (default 25)\n"
              "  --file-reader-open-p95-ms N\n"
              "                           File index-open P95 gate (default 100)\n"
              "  --owner-publication-us N Owner update gate and scheduler budget (default 500)\n"
              "  --save-streaming-p95-ms N\n"
              "                           Concurrent physical-load P95 gate (default 250)\n"
              "  --save-submission-ms N   Owner save handoff gate (default 0.25)\n"
              "  --save-durable-ms N      Stable-storage acceptance gate (default 5000)\n"
              "  --save-compaction-ms N   Full generation publication gate (default 75000)\n"
              "  --enforce-gates          Return failure when a latency gate is exceeded\n"
              "  --output PATH            Write JSON; otherwise write JSON to stdout\n"
              "\nPerformance measurements require an optimized build. All target chunks become "
              "interesting at once, so admission delay remains in the raw wall-clock samples.\n";
}

[[nodiscard]] int run(const Options& options) {
#if !defined(NDEBUG)
    static_cast<void>(options);
    std::cerr << "chunk_streaming_benchmark.unoptimized_build: rebuild with default-release or "
                 "an optimized profiling configuration\n";
    return 2;
#else
    auto report = benchmark::run_chunk_streaming_benchmark(options.benchmark);
    if (!report) {
        std::cerr << report.error().code << ": " << report.error().message << '\n';
        return 1;
    }
    if (options.output.empty()) {
        std::cout << report.value().to_json();
    } else {
        auto status = report.value().write_json(options.output);
        if (!status) {
            std::cerr << status.error().code << ": " << status.error().message << '\n';
            return 1;
        }
        std::cout << "wrote " << report.value().raw_samples.size() << " raw chunk samples to "
                  << options.output << '\n';
        for (const auto& summary : report.value().summaries()) {
            std::cout << benchmark::chunk_streaming_workload_name(summary.workload)
                      << ": P95 interest-to-publication " << summary.p95_interest_to_publication_ms
                      << " ms, max owner publication " << summary.maximum_publication_time_us
                      << " us";
            if (summary.workload == benchmark::ChunkStreamingWorkload::file_delta_warm ||
                summary.workload ==
                    benchmark::ChunkStreamingWorkload::file_delta_drop_cache_advised) {
                std::cout << ", P95 payload read " << summary.p95_disk_read_ms
                          << " ms, P95 reader open " << summary.p95_delta_reader_open_ms << " ms";
            }
            std::cout << '\n';
        }
        if (report.value().save_under_streaming.executed) {
            const auto& save = report.value().save_under_streaming;
            std::cout << "save_under_streaming: P95 interest-to-publication "
                      << save.p95_interest_to_publication_ms << " ms, owner handoff "
                      << save.save_submission_ms << " ms, durable acceptance "
                      << save.save_durable_acceptance_ms << " ms, compaction "
                      << save.save_compaction_ms << " ms\n";
        }
    }
    if (options.benchmark.enforce_gates && !report.value().gates_passed()) {
        for (const auto& summary : report.value().summaries()) {
            for (const auto& violation : summary.gates.violations) {
                std::cerr << benchmark::chunk_streaming_workload_name(summary.workload) << ": "
                          << violation.metric << '=' << violation.actual << " exceeds "
                          << violation.limit << '\n';
            }
        }
        for (const auto& violation : report.value().save_under_streaming.gates.violations) {
            std::cerr << "save_under_streaming: " << violation.metric << '=' << violation.actual
                      << " exceeds " << violation.limit << '\n';
        }
        return 3;
    }
    return 0;
#endif
}

} // namespace

int main(int argc, char** argv) {
    return core::run_process_entry(argv[0], [argc, argv] {
        auto options = parse_options(argc, argv);
        if (!options) {
            print_usage(std::cerr);
            std::cerr << options.error().code << ": " << options.error().message << '\n';
            return 2;
        }
        if (options.value().help) {
            print_usage(std::cout);
            return 0;
        }
        return run(options.value());
    });
}
