#include "engine/core/process_entry.hpp"
#include "engine/save/chunk_delta_journal_benchmark.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace benchmark = heartstead::save::benchmark;
namespace core = heartstead::core;

struct Options {
    benchmark::ChunkDeltaJournalBenchmarkConfig benchmark;
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

[[nodiscard]] core::Result<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const auto argument = std::string_view(argv[index]);
        const auto next = [&]() -> core::Result<std::string_view> {
            if (index + 1 >= argc) {
                return core::Result<std::string_view>::failure(
                    "chunk_delta_journal_benchmark.missing_value",
                    std::string(argument) + " requires a value");
            }
            return core::Result<std::string_view>::success(argv[++index]);
        };

        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--enforce-gates") {
            options.benchmark.enforce_gates = true;
        } else if (argument == "--base-records" || argument == "--payload-bytes") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::size_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("chunk_delta_journal_benchmark.invalid_size",
                                                      std::string(argument) +
                                                          " must be an unsigned integer");
            }
            if (argument == "--base-records") {
                options.benchmark.base_record_count = *parsed;
            } else {
                options.benchmark.payload_bytes = *parsed;
            }
        } else if (argument == "--warmup-appends" || argument == "--appends" ||
                   argument == "--open-warmup" || argument == "--open-repetitions") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint32_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("chunk_delta_journal_benchmark.invalid_count",
                                                      std::string(argument) +
                                                          " must be an unsigned integer");
            }
            if (argument == "--warmup-appends") {
                options.benchmark.warmup_append_count = *parsed;
            } else if (argument == "--appends") {
                options.benchmark.append_repetitions = *parsed;
            } else if (argument == "--open-warmup") {
                options.benchmark.open_warmup_repetitions = *parsed;
            } else {
                options.benchmark.open_repetitions = *parsed;
            }
        } else if (argument == "--initial-writer-open-ms" || argument == "--append-p95-ms" ||
                   argument == "--writer-open-p95-ms" || argument == "--reader-open-p95-ms" ||
                   argument == "--checkpoint-ms" ||
                   argument == "--post-checkpoint-reader-open-ms") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<double>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "chunk_delta_journal_benchmark.invalid_latency",
                    std::string(argument) + " must be a finite positive number");
            }
            if (argument == "--initial-writer-open-ms") {
                options.benchmark.maximum_initial_writer_open_ms = *parsed;
            } else if (argument == "--append-p95-ms") {
                options.benchmark.maximum_append_p95_ms = *parsed;
            } else if (argument == "--writer-open-p95-ms") {
                options.benchmark.maximum_writer_open_p95_ms = *parsed;
            } else if (argument == "--reader-open-p95-ms") {
                options.benchmark.maximum_reader_open_p95_ms = *parsed;
            } else if (argument == "--checkpoint-ms") {
                options.benchmark.maximum_checkpoint_ms = *parsed;
            } else {
                options.benchmark.maximum_post_checkpoint_reader_open_ms = *parsed;
            }
        } else if (argument == "--fixture-parent") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.benchmark.fixture_parent = value.value();
        } else if (argument == "--output") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.output = value.value();
        } else {
            return core::Result<Options>::failure("chunk_delta_journal_benchmark.unknown_option",
                                                  "unknown chunk delta journal benchmark option: " +
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
    output << "usage: heartstead_chunk_delta_journal_benchmark [options]\n"
              "  --base-records N                 Base indexed records (default 16384)\n"
              "  --payload-bytes N                Bytes per base/update payload (default 88)\n"
              "  --warmup-appends N               Unmeasured durable appends (default 8)\n"
              "  --appends N                      Retained durable appends (default 128)\n"
              "  --open-warmup N                  Unmeasured reader/writer opens (default 2)\n"
              "  --open-repetitions N             Retained reader/writer opens (default 9)\n"
              "  --fixture-parent PATH            Parent volume for the ephemeral fixture\n"
              "  --initial-writer-open-ms N       Initial writer-open gate (default 250)\n"
              "  --append-p95-ms N                Durable append P95 gate (default 25)\n"
              "  --writer-open-p95-ms N           Journal writer reopen P95 gate (default 250)\n"
              "  --reader-open-p95-ms N           Journal reader reopen P95 gate (default 250)\n"
              "  --checkpoint-ms N                Full checkpoint gate (default 75000)\n"
              "  --post-checkpoint-reader-open-ms N\n"
              "                                     Compacted reader-open gate (default 250)\n"
              "  --enforce-gates                  Return failure when a gate is exceeded\n"
              "  --output PATH                    Write JSON; otherwise write JSON to stdout\n"
              "\nPerformance measurements require an optimized build. Fixture generation, every "
              "durable append, restart-style reader/writer opens, one full checkpoint, exact "
              "post-checkpoint verification, and fixture cleanup are reported separately.\n";
}

[[nodiscard]] int run(const Options& options) {
#if !defined(NDEBUG)
    static_cast<void>(options);
    std::cerr << "chunk_delta_journal_benchmark.unoptimized_build: rebuild with default-release "
                 "or an optimized profiling configuration\n";
    return 2;
#else
    auto report = benchmark::run_chunk_delta_journal_benchmark(options.benchmark);
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
        const auto summary = report.value().summary();
        std::cout << "wrote " << summary.append_sample_count << " durable append samples to "
                  << options.output << '\n'
                  << "append P95 " << summary.p95_append_ms << " ms, writer-open P95 "
                  << summary.p95_writer_open_ms << " ms, reader-open P95 "
                  << summary.p95_reader_open_ms << " ms, checkpoint " << summary.checkpoint_ms
                  << " ms\n";
    }
    if (options.benchmark.enforce_gates && !report.value().gates_passed()) {
        for (const auto& violation : report.value().summary().gates.violations) {
            std::cerr << violation.metric << '=' << violation.actual << " exceeds "
                      << violation.limit << '\n';
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
