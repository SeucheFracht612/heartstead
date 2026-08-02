#include "engine/core/process_entry.hpp"
#include "engine/world/benchmark/voxel_response_benchmark.hpp"

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
    benchmark::VoxelResponseBenchmarkConfig benchmark;
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
                    "voxel_response_benchmark.missing_value",
                    std::string(argument) + " requires a value");
            }
            return core::Result<std::string_view>::success(argv[++index]);
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--enforce-gates") {
            options.benchmark.enforce_gates = true;
        } else if (argument == "--radius" || argument == "--warmup" ||
                   argument == "--repetitions") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint32_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("voxel_response_benchmark.invalid_count",
                                                      std::string(argument) +
                                                          " must be an unsigned integer");
            }
            if (argument == "--radius") {
                if (*parsed > std::numeric_limits<std::uint16_t>::max()) {
                    return core::Result<Options>::failure("voxel_response_benchmark.invalid_radius",
                                                          "--radius is out of range");
                }
                options.benchmark.horizontal_radius_chunks = static_cast<std::uint16_t>(*parsed);
            } else if (argument == "--warmup") {
                options.benchmark.warmup_repetitions = *parsed;
            } else {
                options.benchmark.repetitions = *parsed;
            }
        } else if (argument == "--update-us" || argument == "--timeout-ms" ||
                   argument == "--snapshot-cells") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint64_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("voxel_response_benchmark.invalid_integer",
                                                      std::string(argument) +
                                                          " must be an unsigned integer");
            }
            if (argument == "--update-us") {
                options.benchmark.update_interval_us = *parsed;
            } else if (argument == "--timeout-ms") {
                options.benchmark.timeout_ms = *parsed;
            } else {
                if (*parsed > std::numeric_limits<std::size_t>::max()) {
                    return core::Result<Options>::failure(
                        "voxel_response_benchmark.invalid_snapshot_budget",
                        "--snapshot-cells is out of range");
                }
                options.benchmark.lighting.max_snapshot_cells_per_update =
                    static_cast<std::size_t>(*parsed);
            }
        } else if (argument == "--collision-p95-ms" || argument == "--relight-p95-ms") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<double>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("voxel_response_benchmark.invalid_latency",
                                                      std::string(argument) +
                                                          " must be a finite positive number");
            }
            if (argument == "--collision-p95-ms") {
                options.benchmark.maximum_collision_p95_ms = *parsed;
            } else {
                options.benchmark.maximum_relight_p95_ms = *parsed;
            }
        } else if (argument == "--output") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.output = value.value();
        } else {
            return core::Result<Options>::failure("voxel_response_benchmark.unknown_option",
                                                  "unknown voxel response benchmark option: " +
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
    output
        << "usage: heartstead_voxel_response_benchmark [options]\n"
           "  --radius N             Square horizontal chunk radius (default 1 = 3x3)\n"
           "  --warmup N             Unmeasured isolated edits (default 2)\n"
           "  --repetitions N        Retained isolated edits (default 9)\n"
           "  --update-us N          Owner update cadence (default 16667, approximately 60 Hz)\n"
           "  --timeout-ms N         Per-phase fail-closed timeout (default 5000)\n"
           "  --snapshot-cells N     Lighting snapshot copy budget per update (default 49152)\n"
           "  --collision-p95-ms N   Collision publication P95 gate (default 100)\n"
           "  --relight-p95-ms N     Full-field relight convergence P95 gate (default 250)\n"
           "  --enforce-gates        Return failure when a P95 response gate is exceeded\n"
           "  --output PATH          Write JSON; otherwise write JSON to stdout\n"
           "\nPerformance measurements require an optimized build. Each edit is issued only after "
           "the previous collision and lighting work has fully published.\n";
}

[[nodiscard]] int run(const Options& options) {
#if !defined(NDEBUG)
    static_cast<void>(options);
    std::cerr << "voxel_response_benchmark.unoptimized_build: rebuild with default-release or "
                 "an optimized profiling configuration\n";
    return 2;
#else
    auto report = benchmark::run_voxel_response_benchmark(options.benchmark);
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
        std::cout << "wrote " << report.value().raw_samples.size()
                  << " isolated response samples to " << options.output << '\n'
                  << "collision P95 " << summary.p95_collision_response_ms << " ms, relight P95 "
                  << summary.p95_relight_convergence_ms << " ms\n";
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
            std::cerr << options.error().code << ": " << options.error().message << '\n';
            print_usage(std::cerr);
            return 2;
        }
        if (options.value().help) {
            print_usage(std::cout);
            return 0;
        }
        return run(options.value());
    });
}
