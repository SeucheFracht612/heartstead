#include "engine/core/process_entry.hpp"
#include "engine/world/streaming/predictive_streaming_benchmark.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace benchmark = heartstead::world::benchmark;
namespace core = heartstead::core;

struct Options {
    benchmark::PredictiveStreamingBenchmarkConfig benchmark;
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
                    "predictive_streaming_benchmark.missing_value",
                    std::string(argument) + " requires a value");
            }
            return core::Result<std::string_view>::success(argv[++index]);
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--enforce-gates") {
            options.benchmark.enforce_gates = true;
        } else if (argument == "--steady-steps" || argument == "--reversal-steps" ||
                   argument == "--post-teleport-steps" || argument == "--soak-steps" ||
                   argument == "--workers" || argument == "--concurrent") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint32_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "predictive_streaming_benchmark.invalid_count",
                    std::string(argument) + " must be an unsigned integer");
            }
            if (argument == "--steady-steps") {
                options.benchmark.steady_steps = *parsed;
            } else if (argument == "--reversal-steps") {
                options.benchmark.reversal_steps = *parsed;
            } else if (argument == "--post-teleport-steps") {
                options.benchmark.post_teleport_steps = *parsed;
            } else if (argument == "--soak-steps") {
                options.benchmark.soak_steps = *parsed;
            } else if (argument == "--workers") {
                options.benchmark.scheduler.worker_count = *parsed;
            } else {
                options.benchmark.scheduler.max_concurrent_requests = *parsed;
                options.benchmark.scheduler.max_completed_results = *parsed;
                options.benchmark.scheduler.max_reserved_working_bytes =
                    options.benchmark.scheduler.reservation_bytes_per_request * *parsed;
            }
        } else if (argument == "--seed" || argument == "--movement-us" ||
                   argument == "--owner-update-us" || argument == "--timeout-ms" ||
                   argument == "--owner-publication-us") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint64_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "predictive_streaming_benchmark.invalid_integer",
                    std::string(argument) + " must be an unsigned integer");
            }
            if (argument == "--seed") {
                options.benchmark.seed = *parsed;
            } else if (argument == "--movement-us") {
                options.benchmark.movement_interval_us = *parsed;
            } else if (argument == "--owner-update-us") {
                options.benchmark.owner_update_interval_us = *parsed;
            } else if (argument == "--timeout-ms") {
                options.benchmark.settle_timeout_ms = *parsed;
            } else {
                options.benchmark.maximum_owner_publication_us = *parsed;
                options.benchmark.scheduler.max_publication_time_us = *parsed;
            }
        } else if (argument == "--hole-p95-ms" || argument == "--immediate-hit-rate" ||
                   argument == "--accuracy" || argument == "--waste-ratio" ||
                   argument == "--cancellation-ratio" || argument == "--hole-rate-ratio" ||
                   argument == "--memory-slope") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<double>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("predictive_streaming_benchmark.invalid_gate",
                                                      std::string(argument) +
                                                          " must be a finite non-negative number");
            }
            if (argument == "--hole-p95-ms") {
                options.benchmark.maximum_predictive_hole_p95_ms = *parsed;
            } else if (argument == "--immediate-hit-rate") {
                options.benchmark.minimum_predictive_immediate_hit_rate = *parsed;
            } else if (argument == "--accuracy") {
                options.benchmark.minimum_prediction_accuracy = *parsed;
            } else if (argument == "--waste-ratio") {
                options.benchmark.maximum_prediction_waste_ratio = *parsed;
            } else if (argument == "--cancellation-ratio") {
                options.benchmark.minimum_cancellation_completion_ratio = *parsed;
            } else if (argument == "--hole-rate-ratio") {
                options.benchmark.maximum_predictive_hole_rate_ratio_vs_baseline = *parsed;
            } else {
                options.benchmark.maximum_soak_memory_slope_chunks_per_step = *parsed;
            }
        } else if (argument == "--teleport-chunks") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::int64_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "predictive_streaming_benchmark.invalid_teleport",
                    "--teleport-chunks must be an integer");
            }
            options.benchmark.teleport_distance_chunks = *parsed;
        } else if (argument == "--output") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.output = value.value();
        } else {
            return core::Result<Options>::failure(
                "predictive_streaming_benchmark.unknown_option",
                "unknown predictive streaming benchmark option: " + std::string(argument));
        }
    }
    auto status = options.benchmark.validate();
    if (!status) {
        return core::Result<Options>::failure(status.error().code, status.error().message);
    }
    return core::Result<Options>::success(std::move(options));
}

void print_usage(std::ostream& output) {
    output << "usage: heartstead_predictive_streaming_benchmark [options]\n"
              "  --steady-steps N         Forward traversal steps (default 20)\n"
              "  --reversal-steps N       Direction-reversal steps (default 8)\n"
              "  --post-teleport-steps N  Recovery steps after teleport (default 6)\n"
              "  --soak-steps N           Bounded-residency traversal steps (default 32)\n"
              "  --teleport-chunks N      Teleport distance on two axes (default 256)\n"
              "  --movement-us N          Time exposed at each chunk step (default 20000)\n"
              "  --owner-update-us N      Owner update cadence (default 1000)\n"
              "  --timeout-ms N           Final drain timeout (default 10000)\n"
              "  --workers N              Chunk generation workers (default 2)\n"
              "  --concurrent N           Scheduler request/reservation slots (default 8)\n"
              "  --hole-p95-ms N          Predictive visible-hole P95 gate (default 250)\n"
              "  --immediate-hit-rate N   Minimum predictive immediate hit rate (default 0.5)\n"
              "  --accuracy N             Minimum resolved prediction accuracy (default 0.25)\n"
              "  --waste-ratio N          Maximum resolved prediction waste (default 0.75)\n"
              "  --cancellation-ratio N   Minimum completed/requested cancellation ratio\n"
              "  --hole-rate-ratio N      Maximum predictive/baseline hole-rate ratio\n"
              "  --memory-slope N         Maximum soak chunks gained per step (default 0.05)\n"
              "  --owner-publication-us N Owner publication gate and budget (default 500)\n"
              "  --enforce-gates          Return failure when any gate is missed\n"
              "  --output PATH            Write JSON; otherwise write JSON to stdout\n";
}

[[nodiscard]] int run(const Options& options) {
#if !defined(NDEBUG)
    static_cast<void>(options);
    std::cerr << "predictive_streaming_benchmark.unoptimized_build: use an optimized build\n";
    return 2;
#else
    auto report = benchmark::run_predictive_streaming_benchmark(options.benchmark);
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
        const auto& baseline = report.value().baseline;
        const auto& predictive = report.value().predictive;
        std::cout << "baseline holes=" << baseline.steps_with_visible_holes << '/'
                  << baseline.movement_steps << " P95=" << baseline.p95_visible_hole_ms
                  << " ms; predictive holes=" << predictive.steps_with_visible_holes << '/'
                  << predictive.movement_steps << " P95=" << predictive.p95_visible_hole_ms
                  << " ms accuracy=" << predictive.policy_stats.prediction_accuracy
                  << " cancellations=" << predictive.policy_stats.cancelled_requests << '/'
                  << predictive.policy_stats.cancellation_requests
                  << " slope=" << predictive.soak_memory_slope_chunks_per_step << '\n';
    }
    if (options.benchmark.enforce_gates && !report.value().gates_passed()) {
        for (const auto& violation : report.value().gates.violations) {
            std::cerr << violation.metric << '=' << violation.actual << " misses "
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
