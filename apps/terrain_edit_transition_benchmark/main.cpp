#include "engine/core/process_entry.hpp"
#include "engine/renderer/benchmark/terrain_edit_transition_benchmark.hpp"

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

namespace benchmark = heartstead::renderer::benchmark;
namespace core = heartstead::core;
namespace rhi = heartstead::renderer::rhi;

struct Options {
    benchmark::TerrainEditTransitionBenchmarkConfig benchmark;
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
                    "terrain_edit_transition_benchmark.missing_value",
                    std::string(argument) + " requires a value");
            }
            return core::Result<std::string_view>::success(argv[++index]);
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--enforce-gates") {
            options.benchmark.enforce_gates = true;
        } else if (argument == "--backend") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            if (value.value() == "headless") {
                options.benchmark.render_backend = rhi::RenderBackend::headless;
            } else if (value.value() == "vulkan") {
                options.benchmark.render_backend = rhi::RenderBackend::vulkan;
            } else {
                return core::Result<Options>::failure(
                    "terrain_edit_transition_benchmark.invalid_backend",
                    "--backend must be headless or vulkan");
            }
        } else if (argument == "--radius" || argument == "--warmup" ||
                   argument == "--repetitions" || argument == "--patch-resolution" ||
                   argument == "--far-workers" || argument == "--far-concurrency") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint32_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "terrain_edit_transition_benchmark.invalid_count",
                    std::string(argument) + " must be an unsigned integer");
            }
            if (argument == "--radius") {
                if (*parsed > std::numeric_limits<std::uint16_t>::max()) {
                    return core::Result<Options>::failure(
                        "terrain_edit_transition_benchmark.invalid_radius",
                        "--radius is out of range");
                }
                options.benchmark.world_radius_chunks = static_cast<std::uint16_t>(*parsed);
            } else if (argument == "--warmup") {
                options.benchmark.warmup_repetitions = *parsed;
            } else if (argument == "--repetitions") {
                options.benchmark.repetitions = *parsed;
            } else if (argument == "--patch-resolution") {
                options.benchmark.far_rendering.clipmap.patch_resolution = *parsed;
            } else if (argument == "--far-workers") {
                options.benchmark.far_rendering.mesh_scheduler.worker_count = *parsed;
            } else {
                options.benchmark.far_rendering.mesh_scheduler.maximum_concurrent_jobs = *parsed;
            }
        } else if (argument == "--update-us" || argument == "--timeout-ms") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint64_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "terrain_edit_transition_benchmark.invalid_integer",
                    std::string(argument) + " must be an unsigned integer");
            }
            if (argument == "--update-us") {
                options.benchmark.update_interval_us = *parsed;
            } else {
                options.benchmark.timeout_ms = *parsed;
            }
        } else if (argument == "--near-p95-ms" || argument == "--mid-p95-ms" ||
                   argument == "--far-p95-ms" || argument == "--full-p95-ms" ||
                   argument == "--owner-ms" || argument == "--upload-prep-ms" ||
                   argument == "--gpu-wait-ms") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<double>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "terrain_edit_transition_benchmark.invalid_gate",
                    std::string(argument) + " must be a finite number");
            }
            if (argument == "--near-p95-ms") {
                options.benchmark.maximum_near_draw_p95_ms = *parsed;
            } else if (argument == "--mid-p95-ms") {
                options.benchmark.maximum_mid_convergence_p95_ms = *parsed;
            } else if (argument == "--far-p95-ms") {
                options.benchmark.maximum_far_convergence_p95_ms = *parsed;
            } else if (argument == "--full-p95-ms") {
                options.benchmark.maximum_full_convergence_p95_ms = *parsed;
            } else if (argument == "--owner-ms") {
                options.benchmark.maximum_owner_update_ms = *parsed;
            } else if (argument == "--upload-prep-ms") {
                options.benchmark.maximum_upload_preparation_ms = *parsed;
            } else {
                options.benchmark.maximum_synchronous_gpu_wait_ms = *parsed;
            }
        } else if (argument == "--output") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.output = value.value();
        } else {
            return core::Result<Options>::failure(
                "terrain_edit_transition_benchmark.unknown_option",
                "unknown terrain edit-transition benchmark option: " + std::string(argument));
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
        << "usage: heartstead_terrain_edit_transition_benchmark [options]\n"
           "  --backend NAME       Buffer backend: headless or vulkan (default headless)\n"
           "  --radius N           Flat-world radius (default 4 = 81 chunks)\n"
           "  --warmup N           Unretained repetitions (default 2)\n"
           "  --repetitions N      Retained isolated edits (default 9)\n"
           "  --update-us N        Owner cadence (default 16667, approximately 60 Hz)\n"
           "  --timeout-ms N       Per-phase fail-closed timeout (default 5000)\n"
           "  --patch-resolution N Far-patch cell resolution (default 8)\n"
           "  --far-workers N      Far topology workers (default 2)\n"
           "  --far-concurrency N  Far in-flight/ready bound (default 3)\n"
           "  --near-p95-ms N      Exact near draw P95 gate (default 50)\n"
           "  --mid-p95-ms N       Complete mid replacement P95 gate (default 250)\n"
           "  --far-p95-ms N       Complete far replacement P95 gate (default 500)\n"
           "  --full-p95-ms N      Whole transition P95 gate (default 500)\n"
           "  --owner-ms N         Worst renderer-owner update gate (default 12)\n"
           "  --upload-prep-ms N  Near upload-preparation gate (default 0.5)\n"
           "  --gpu-wait-ms N      Synchronous GPU-wait gate (default 0)\n"
           "  --enforce-gates      Return failure when a latency gate is exceeded\n"
           "  --output PATH        Write JSON; otherwise write JSON to stdout\n"
           "\nEach retained run starts resident, applies one aligned authoritative voxel edit, "
           "waits "
           "for exact near draw eligibility and complete mid/far replacement, then forces a "
           "two-edit supersession race. Old residents must remain drawable throughout. GPU draw "
           "execution, presentation, and display scan-out are outside the endpoints. Performance "
           "measurements require an optimized build.\n";
}

[[nodiscard]] int run(const Options& options) {
#if !defined(NDEBUG)
    static_cast<void>(options);
    std::cerr << "terrain_edit_transition_benchmark.unoptimized_build: rebuild with "
                 "default-release or an optimized profiling configuration\n";
    return 2;
#else
    auto report = benchmark::run_terrain_edit_transition_benchmark(options.benchmark);
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
                  << " terrain edit-transition samples to " << options.output << '\n'
                  << "near/mid/far/full P95 " << summary.p95_near_draw_ms << '/'
                  << summary.p95_mid_convergence_ms << '/' << summary.p95_far_convergence_ms << '/'
                  << summary.p95_full_convergence_ms << " ms, worst owner update "
                  << summary.maximum_owner_update_ms << " ms\n";
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
