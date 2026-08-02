#include "engine/core/process_entry.hpp"
#include "engine/renderer/benchmark/chunk_render_readiness_benchmark.hpp"

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
    benchmark::ChunkRenderReadinessBenchmarkConfig benchmark;
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
                    "chunk_render_readiness_benchmark.missing_value",
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
                    "chunk_render_readiness_benchmark.invalid_backend",
                    "--backend must be headless or vulkan");
            }
        } else if (argument == "--radius" || argument == "--warmup" ||
                   argument == "--repetitions") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint32_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "chunk_render_readiness_benchmark.invalid_count",
                    std::string(argument) + " must be an unsigned integer");
            }
            if (argument == "--radius") {
                if (*parsed > std::numeric_limits<std::uint16_t>::max()) {
                    return core::Result<Options>::failure(
                        "chunk_render_readiness_benchmark.invalid_radius",
                        "--radius is out of range");
                }
                options.benchmark.horizontal_radius_chunks = static_cast<std::uint16_t>(*parsed);
            } else if (argument == "--warmup") {
                options.benchmark.warmup_repetitions = *parsed;
            } else {
                options.benchmark.repetitions = *parsed;
            }
        } else if (argument == "--seed" || argument == "--update-us" ||
                   argument == "--timeout-ms") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint64_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "chunk_render_readiness_benchmark.invalid_integer",
                    std::string(argument) + " must be an unsigned integer");
            }
            if (argument == "--seed") {
                options.benchmark.seed = *parsed;
            } else if (argument == "--update-us") {
                options.benchmark.update_interval_us = *parsed;
            } else {
                options.benchmark.timeout_ms = *parsed;
            }
        } else if (argument == "--draw-p95-ms" || argument == "--upload-prep-ms" ||
                   argument == "--gpu-wait-ms" || argument == "--mesh-amplification") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<double>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "chunk_render_readiness_benchmark.invalid_gate",
                    std::string(argument) + " must be a finite number");
            }
            if (argument == "--draw-p95-ms") {
                options.benchmark.maximum_draw_eligibility_p95_ms = *parsed;
            } else if (argument == "--upload-prep-ms") {
                options.benchmark.maximum_upload_preparation_ms = *parsed;
            } else if (argument == "--gpu-wait-ms") {
                options.benchmark.maximum_synchronous_gpu_wait_ms = *parsed;
            } else {
                options.benchmark.maximum_mesh_builds_per_publication = *parsed;
            }
        } else if (argument == "--output") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.output = value.value();
        } else {
            return core::Result<Options>::failure(
                "chunk_render_readiness_benchmark.unknown_option",
                "unknown chunk render-readiness benchmark option: " + std::string(argument));
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
        << "usage: heartstead_chunk_render_readiness_benchmark [options]\n"
           "  --backend NAME       Buffer-upload backend: headless or vulkan (default headless)\n"
           "  --seed N             Deterministic terrace corpus seed\n"
           "  --radius N           Circular required-interest radius (default 2 = 13 chunks)\n"
           "  --warmup N           Unretained cold workload repetitions (default 2)\n"
           "  --repetitions N      Retained cold workload repetitions (default 9)\n"
           "  --update-us N        Owner cadence (default 16667, approximately 60 Hz)\n"
           "  --timeout-ms N       Per-workload fail-closed timeout (default 5000)\n"
           "  --draw-p95-ms N      Interest-to-draw-command P95 gate (default 250)\n"
           "  --upload-prep-ms N  Maximum upload-preparation gate (default 0.5)\n"
           "  --gpu-wait-ms N      Maximum synchronous GPU-wait gate (default 0)\n"
           "  --mesh-amplification N Maximum mesh builds per publication (default 2.5)\n"
           "  --enforce-gates      Return failure when a response gate is exceeded\n"
           "  --output PATH        Write JSON; otherwise write JSON to stdout\n"
           "\nThe endpoint is first exact current chunk draw-command eligibility. It includes "
           "load publication, asynchronous meshing, RHI buffer upload, culling, and draw-list "
           "construction, but excludes GPU draw execution, presentation, and display scan-out. "
           "Performance measurements require an optimized build.\n";
}

[[nodiscard]] int run(const Options& options) {
#if !defined(NDEBUG)
    static_cast<void>(options);
    std::cerr << "chunk_render_readiness_benchmark.unoptimized_build: rebuild with "
                 "default-release or an optimized profiling configuration\n";
    return 2;
#else
    auto report = benchmark::run_chunk_render_readiness_benchmark(options.benchmark);
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
                  << " render-readiness samples to " << options.output << '\n'
                  << "draw eligibility P95 " << summary.p95_interest_to_draw_eligibility_ms
                  << " ms, maximum upload preparation " << summary.maximum_upload_preparation_ms
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
