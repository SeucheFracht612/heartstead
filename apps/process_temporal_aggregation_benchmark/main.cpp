#include "engine/core/process_entry.hpp"
#include "engine/processes/process_temporal_aggregation_benchmark.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace benchmark = heartstead::processes::benchmark;
namespace core = heartstead::core;

struct Options {
    benchmark::ProcessTemporalAggregationBenchmarkConfig benchmark;
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
                    "process_temporal_benchmark.missing_value",
                    std::string(argument) + " requires a value");
            }
            return core::Result<std::string_view>::success(argv[++index]);
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--processes" || argument == "--burst" || argument == "--stalled" ||
                   argument == "--admissions-per-tick" || argument == "--events-per-tick" ||
                   argument == "--warmup" || argument == "--repetitions" ||
                   argument == "--max-backlog-ticks") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint32_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("process_temporal_benchmark.invalid_count",
                                                      std::string(argument) +
                                                          " must be an unsigned integer");
            }
            if (argument == "--processes") {
                options.benchmark.process_count = *parsed;
            } else if (argument == "--burst") {
                options.benchmark.burst_process_count = *parsed;
            } else if (argument == "--stalled") {
                options.benchmark.stalled_process_count = *parsed;
            } else if (argument == "--admissions-per-tick") {
                options.benchmark.temporal.maximum_admissions_per_tick = *parsed;
            } else if (argument == "--events-per-tick") {
                options.benchmark.temporal.maximum_events_per_tick = *parsed;
            } else if (argument == "--warmup") {
                options.benchmark.warmup_repetitions = *parsed;
            } else if (argument == "--repetitions") {
                options.benchmark.repetitions = *parsed;
            } else {
                options.benchmark.maximum_event_backlog_ticks = *parsed;
            }
        } else if (argument == "--ticks" || argument == "--stress-tick" ||
                   argument == "--stalled-interval" || argument == "--seed") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint64_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("process_temporal_benchmark.invalid_integer",
                                                      std::string(argument) +
                                                          " must be an unsigned integer");
            }
            if (argument == "--ticks") {
                options.benchmark.simulation_ticks = *parsed;
            } else if (argument == "--stress-tick") {
                options.benchmark.stress_tick = *parsed;
            } else if (argument == "--stalled-interval") {
                options.benchmark.temporal.stalled_reevaluation_interval_ticks = *parsed;
            } else {
                options.benchmark.seed = *parsed;
            }
        } else if (argument == "--max-p99-ms" || argument == "--min-speedup" ||
                   argument == "--min-resolver-reduction") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<double>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("process_temporal_benchmark.invalid_gate",
                                                      std::string(argument) +
                                                          " must be a finite non-negative number");
            }
            if (argument == "--max-p99-ms") {
                options.benchmark.maximum_temporal_p99_tick_ms = *parsed;
            } else if (argument == "--min-speedup") {
                options.benchmark.minimum_median_speedup = *parsed;
            } else {
                options.benchmark.minimum_resolver_call_reduction_ratio = *parsed;
            }
        } else if (argument == "--output") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.output = value.value();
        } else {
            return core::Result<Options>::failure("process_temporal_benchmark.unknown_option",
                                                  "unknown process temporal benchmark option: " +
                                                      std::string(argument));
        }
    }

    options.benchmark.temporal.maximum_tracked_processes =
        std::max(options.benchmark.temporal.maximum_tracked_processes,
                 static_cast<std::size_t>(options.benchmark.process_count));
    auto status = options.benchmark.validate();
    if (!status) {
        return core::Result<Options>::failure(status.error().code, status.error().message);
    }
    return core::Result<Options>::success(std::move(options));
}

void print_usage(std::ostream& output) {
    output << "usage: heartstead_process_temporal_aggregation_benchmark [options]\n"
              "  --processes N              Process corpus size (default 65536)\n"
              "  --ticks N                  Retained logical ticks per pass (default 600)\n"
              "  --burst N                  Processes due together at stress tick (default 2048)\n"
              "  --stalled N                Zero-rate processes in corpus (default 256)\n"
              "  --stress-tick N            Completion-burst tick (default 300)\n"
              "  --admissions-per-tick N    Temporal admission budget (default 4096)\n"
              "  --events-per-tick N        Temporal event budget (default 1024)\n"
              "  --stalled-interval N       Zero-rate reevaluation interval (default 20)\n"
              "  --warmup N                 Unretained warmup passes (default 1)\n"
              "  --repetitions N            Retained benchmark passes (default 5)\n"
              "  --max-backlog-ticks N      Maximum continuous due backlog (default 2)\n"
              "  --max-p99-ms N             Maximum temporal P99 tick time (default 5.0)\n"
              "  --min-speedup N            Minimum dense/temporal median ratio (default 5.0)\n"
              "  --min-resolver-reduction N Minimum resolver-call reduction (default 0.95)\n"
              "  --seed N                   Deterministic decimal corpus seed\n"
              "  --output PATH              Write JSON; otherwise write JSON to stdout\n";
}

[[nodiscard]] int run(const Options& options) {
#if !defined(NDEBUG)
    static_cast<void>(options);
    std::cerr << "process_temporal_benchmark.unoptimized_build: use an optimized build\n";
    return 2;
#else
    auto report = benchmark::run_process_temporal_aggregation_benchmark(options.benchmark);
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
        std::cout << "temporal median=" << report.value().temporal_timing.median_ms
                  << " ms P99=" << report.value().temporal_timing.p99_ms
                  << " ms; dense median=" << report.value().dense_timing.median_ms
                  << " ms; speedup=" << report.value().median_speedup << "x resolver_reduction="
                  << report.value().minimum_resolver_call_reduction_ratio * 100.0 << "%\n";
    }

    if (!report.value().acceptance_passed()) {
        for (const auto& check : report.value().acceptance) {
            if (check.enabled && !check.passed) {
                std::cerr << check.name << '=' << check.measured << " misses " << check.comparison
                          << check.limit << '\n';
            }
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
