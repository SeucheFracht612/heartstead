#include "engine/content/content_validation.hpp"
#include "engine/core/process_entry.hpp"
#include "game/runtime/multiplayer_chunk_subscription_benchmark.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace benchmark = heartstead::game::benchmark;
namespace content = heartstead::content;
namespace core = heartstead::core;

struct Options {
    benchmark::MultiplayerChunkSubscriptionBenchmarkConfig benchmark;
    std::filesystem::path content_root = HEARTSTEAD_BENCHMARK_SOURCE_DIR;
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
                    "multiplayer_chunk_subscription_benchmark.missing_value",
                    std::string(argument) + " requires a value");
            }
            return core::Result<std::string_view>::success(argv[++index]);
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--enforce-gates") {
            options.benchmark.enforce_gates = true;
        } else if (argument == "--require-precise-memory") {
            options.benchmark.require_precise_process_memory = true;
        } else if (argument == "--clients" || argument == "--traversal-steps" ||
                   argument == "--hot-edit-ticks" || argument == "--steady-ticks" ||
                   argument == "--soak-conditioning-edits" ||
                   argument == "--soak-conditioning-cycles" || argument == "--soak-cycles" ||
                   argument == "--warmup-timeout-ticks" ||
                   argument == "--transition-timeout-ticks" || argument == "--delivery-messages" ||
                   argument == "--convergence-ticks" || argument == "--backlog-recovery-ticks") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint32_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "multiplayer_chunk_subscription_benchmark.invalid_count",
                    std::string(argument) + " must be an unsigned integer");
            }
            if (argument == "--clients") {
                options.benchmark.client_count = *parsed;
            } else if (argument == "--traversal-steps") {
                options.benchmark.traversal_steps = *parsed;
            } else if (argument == "--hot-edit-ticks") {
                options.benchmark.hot_edit_ticks = *parsed;
            } else if (argument == "--steady-ticks") {
                options.benchmark.steady_ticks = *parsed;
            } else if (argument == "--soak-conditioning-edits") {
                options.benchmark.soak_conditioning_edit_ticks = *parsed;
            } else if (argument == "--soak-conditioning-cycles") {
                options.benchmark.soak_conditioning_cycles = *parsed;
            } else if (argument == "--soak-cycles") {
                options.benchmark.soak_cycles = *parsed;
            } else if (argument == "--warmup-timeout-ticks") {
                options.benchmark.warmup_timeout_ticks = *parsed;
            } else if (argument == "--transition-timeout-ticks") {
                options.benchmark.transition_timeout_ticks = *parsed;
            } else if (argument == "--delivery-messages") {
                options.benchmark.reliable_delivery_messages_per_client_per_tick = *parsed;
            } else if (argument == "--convergence-ticks") {
                options.benchmark.maximum_transition_convergence_ticks = *parsed;
            } else {
                options.benchmark.maximum_backlog_recovery_ticks = *parsed;
            }
        } else if (argument == "--seed" || argument == "--serialization-us" ||
                   argument == "--serialization-overshoot-us" || argument == "--wire-bytes" ||
                   argument == "--hot-edit-wire-bytes" ||
                   argument == "--soak-memory-growth-bytes") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint64_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "multiplayer_chunk_subscription_benchmark.invalid_integer",
                    std::string(argument) + " must be an unsigned integer");
            }
            if (argument == "--seed") {
                options.benchmark.seed = *parsed;
            } else if (argument == "--serialization-us") {
                options.benchmark.maximum_snapshot_serialization_time_us_per_tick = *parsed;
            } else if (argument == "--serialization-overshoot-us") {
                options.benchmark.maximum_snapshot_serialization_time_overshoot_us = *parsed;
            } else if (argument == "--hot-edit-wire-bytes") {
                options.benchmark.maximum_hot_edit_wire_bytes_per_client_per_tick = *parsed;
            } else if (argument == "--soak-memory-growth-bytes") {
                options.benchmark.maximum_soak_private_memory_growth_bytes = *parsed;
            } else {
                options.benchmark.maximum_wire_bytes_per_client_per_tick = *parsed;
            }
        } else if (argument == "--spread-chunks" || argument == "--traversal-stride") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::int64_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "multiplayer_chunk_subscription_benchmark.invalid_distance",
                    std::string(argument) + " must be an integer");
            }
            if (argument == "--spread-chunks") {
                options.benchmark.spread_distance_chunks = *parsed;
            } else {
                options.benchmark.traversal_stride_chunks = *parsed;
            }
        } else if (argument == "--tick-p95-ms" || argument == "--tick-p99-ms" ||
                   argument == "--tick-max-ms" || argument == "--hot-edit-tick-p95-ms" ||
                   argument == "--hot-edit-tick-p99-ms" || argument == "--hot-edit-tick-max-ms" ||
                   argument == "--shared-reuse" || argument == "--disjoint-reuse" ||
                   argument == "--soak-memory-slope-bytes") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<double>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "multiplayer_chunk_subscription_benchmark.invalid_gate",
                    std::string(argument) + " must be a finite positive number");
            }
            if (argument == "--tick-p95-ms") {
                options.benchmark.maximum_server_tick_p95_ms = *parsed;
            } else if (argument == "--tick-p99-ms") {
                options.benchmark.maximum_server_tick_p99_ms = *parsed;
            } else if (argument == "--tick-max-ms") {
                options.benchmark.maximum_server_tick_ms = *parsed;
            } else if (argument == "--hot-edit-tick-p95-ms") {
                options.benchmark.maximum_hot_edit_server_tick_p95_ms = *parsed;
            } else if (argument == "--hot-edit-tick-p99-ms") {
                options.benchmark.maximum_hot_edit_server_tick_p99_ms = *parsed;
            } else if (argument == "--hot-edit-tick-max-ms") {
                options.benchmark.maximum_hot_edit_server_tick_ms = *parsed;
            } else if (argument == "--shared-reuse") {
                options.benchmark.minimum_shared_snapshot_reuse_ratio = *parsed;
            } else if (argument == "--soak-memory-slope-bytes") {
                options.benchmark.maximum_soak_private_memory_slope_bytes_per_cycle = *parsed;
            } else {
                options.benchmark.maximum_disjoint_snapshot_reuse_ratio = *parsed;
            }
        } else if (argument == "--content-root" || argument == "--output") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            if (argument == "--content-root") {
                options.content_root = value.value();
            } else {
                options.output = value.value();
            }
        } else {
            return core::Result<Options>::failure(
                "multiplayer_chunk_subscription_benchmark.unknown_option",
                "unknown multiplayer chunk subscription benchmark option: " +
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
        << "usage: heartstead_multiplayer_chunk_subscription_benchmark [options]\n"
           "  --clients N                    Simulated clients (default 8)\n"
           "  --traversal-steps N            Disjoint traversal transitions (default 6)\n"
           "  --hot-edit-ticks N             Disjoint edit samples, minimum 100 (default 120)\n"
           "  --steady-ticks N               Final steady-state samples (default 24)\n"
           "  --soak-conditioning-edits N   Edit ticks used to cap bounded histories (default "
           "256)\n"
           "  --soak-conditioning-cycles N  Traversal/edit allocator conditioning (default 8)\n"
           "  --soak-cycles N                Measured fixed-state soak cycles (default 64)\n"
           "  --spread-chunks N              Distance between client regions (default 128)\n"
           "  --traversal-stride N           Chunk stride per traversal step (default 4)\n"
           "  --delivery-messages N          Reliable messages/client/tick (default 48)\n"
           "  --warmup-timeout-ticks N       Unmeasured settlement bound (default 4096)\n"
           "  --transition-timeout-ticks N   Per-transition fail-closed bound (default 32)\n"
           "  --tick-p95-ms N                Server tick P95 gate (default 12.5)\n"
           "  --tick-p99-ms N                Server tick P99 gate (default 16.667)\n"
           "  --tick-max-ms N                Server tick maximum gate (default 50)\n"
           "  --hot-edit-tick-p95-ms N       Hot-edit server tick P95 gate (default 12.5)\n"
           "  --hot-edit-tick-p99-ms N       Hot-edit server tick P99 gate (default 16.667)\n"
           "  --hot-edit-tick-max-ms N       Hot-edit server tick maximum gate (default 50)\n"
           "  --convergence-ticks N          Transition convergence gate (default 16)\n"
           "  --backlog-recovery-ticks N     Reliable backlog recovery gate (default 2)\n"
           "  --shared-reuse N               Minimum clustered encode reuse (default 2)\n"
           "  --disjoint-reuse N             Maximum spread encode reuse (default 1.05)\n"
           "  --serialization-us N           Snapshot serialization/tick budget (default 4000)\n"
           "  --serialization-overshoot-us N Maximum one-operation overshoot (default 1000)\n"
           "  --wire-bytes N                 Wire bytes/client/tick gate (default 327680)\n"
           "  --hot-edit-wire-bytes N        Hot-edit wire bytes/client/tick gate (default 2048)\n"
           "  --soak-memory-slope-bytes N    Private-memory slope/cycle gate (default 65536)\n"
           "  --soak-memory-growth-bytes N   Private-memory endpoint growth gate (default "
           "8388608)\n"
           "  --require-precise-memory       Fail when Linux smaps-style memory is unavailable\n"
           "  --seed N                       Deterministic path/world seed\n"
           "  --content-root PATH            Source content root\n"
           "  --enforce-gates                Return failure when any gate is missed\n"
           "  --output PATH                  Write JSON; otherwise write JSON to stdout\n"
           "\nThis deterministic in-memory workload excludes simulated RTT and packet loss. Run "
           "it in an "
           "optimized build.\n";
}

[[nodiscard]] int run(const Options& options) {
#if !defined(NDEBUG)
    static_cast<void>(options);
    std::cerr << "multiplayer_chunk_subscription_benchmark.unoptimized_build: use an optimized "
                 "build\n";
    return 2;
#else
    const auto content_report = content::ContentValidation::validate(options.content_root);
    if (content_report.has_errors()) {
        std::cerr << "multiplayer_chunk_subscription_benchmark.invalid_content: content root "
                     "failed validation\n";
        return 1;
    }
    auto report =
        benchmark::run_multiplayer_chunk_subscription_benchmark(options.benchmark, content_report);
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
        const auto& summary = report.value().summary;
        std::cout << "wrote " << summary.measured_tick_count << " foreground + "
                  << summary.soak_tick_count << " soak server ticks for "
                  << options.benchmark.client_count << " clients to " << options.output
                  << "; P95=" << summary.server_tick_p95_ms
                  << " ms P99=" << summary.server_tick_p99_ms
                  << " ms hot_edit_P99=" << summary.hot_edit_server_tick_p99_ms
                  << " ms soak_P99=" << summary.soak_server_tick_p99_ms
                  << " ms soak_private_slope=" << summary.soak_private_memory_slope_bytes_per_cycle
                  << " B/cycle" << " convergence=" << summary.maximum_transition_convergence_ticks
                  << " ticks backlog_recovery=" << summary.maximum_backlog_recovery_ticks
                  << " ticks shared_reuse=" << summary.shared_snapshot_reuse_ratio << 'x' << '\n';
    }
    if (options.benchmark.enforce_gates && !report.value().gates_passed()) {
        for (const auto& violation : report.value().gates.violations) {
            std::cerr << violation.metric << '=' << violation.actual << " misses gate "
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
