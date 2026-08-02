#include "engine/content/content_validation.hpp"
#include "engine/core/process_entry.hpp"
#include "game/runtime/multiplayer_network_impairment_benchmark.hpp"

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
    benchmark::MultiplayerNetworkImpairmentBenchmarkConfig benchmark;
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
                    "multiplayer_network_impairment_benchmark.missing_value",
                    std::string(argument) + " requires a value");
            }
            return core::Result<std::string_view>::success(argv[++index]);
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--enforce-gates") {
            options.benchmark.enforce_gates = true;
        } else if (argument == "--clients" || argument == "--ticks" ||
                   argument == "--neutral-tail-ticks" || argument == "--warmup-timeout-ticks" ||
                   argument == "--recovery-timeout-ticks" || argument == "--one-way-latency-ms" ||
                   argument == "--variation-ms" || argument == "--loss-basis-points" ||
                   argument == "--hard-corrections" ||
                   argument == "--hard-corrections-per-client" ||
                   argument == "--eligible-messages" ||
                   argument == "--eligible-messages-per-client" || argument == "--minimum-drops" ||
                   argument == "--minimum-drops-per-client" || argument == "--pending-impaired" ||
                   argument == "--pending-impaired-per-client") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint32_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "multiplayer_network_impairment_benchmark.invalid_count",
                    std::string(argument) + " must be an unsigned integer");
            }
            if (argument == "--clients") {
                options.benchmark.client_count = *parsed;
            } else if (argument == "--ticks") {
                options.benchmark.measured_ticks = *parsed;
            } else if (argument == "--neutral-tail-ticks") {
                options.benchmark.neutral_input_tail_ticks = *parsed;
            } else if (argument == "--warmup-timeout-ticks") {
                options.benchmark.warmup_timeout_ticks = *parsed;
            } else if (argument == "--recovery-timeout-ticks") {
                options.benchmark.recovery_timeout_ticks = *parsed;
            } else if (argument == "--one-way-latency-ms") {
                options.benchmark.simulated_one_way_latency_ms = *parsed;
            } else if (argument == "--variation-ms") {
                options.benchmark.simulated_delay_variation_ms = *parsed;
            } else if (argument == "--loss-basis-points") {
                options.benchmark.simulated_unreliable_loss_basis_points = *parsed;
            } else if (argument == "--hard-corrections") {
                options.benchmark.maximum_hard_correction_count = *parsed;
            } else if (argument == "--hard-corrections-per-client") {
                options.benchmark.maximum_hard_correction_count_per_client = *parsed;
            } else if (argument == "--eligible-messages") {
                options.benchmark.minimum_impairment_eligible_unreliable_message_count = *parsed;
            } else if (argument == "--eligible-messages-per-client") {
                options.benchmark.minimum_impairment_eligible_unreliable_message_count_per_client =
                    *parsed;
            } else if (argument == "--minimum-drops") {
                options.benchmark.minimum_simulated_unreliable_drop_count = *parsed;
            } else if (argument == "--minimum-drops-per-client") {
                options.benchmark.minimum_simulated_unreliable_drop_count_per_client = *parsed;
            } else if (argument == "--pending-impaired") {
                options.benchmark.maximum_pending_impaired_message_count = *parsed;
            } else {
                options.benchmark.maximum_pending_impaired_message_count_per_client = *parsed;
            }
        } else if (argument == "--seed" || argument == "--average-server-bytes" ||
                   argument == "--rolling-server-bytes" || argument == "--average-client-bytes" ||
                   argument == "--rolling-client-bytes") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<std::uint64_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "multiplayer_network_impairment_benchmark.invalid_integer",
                    std::string(argument) + " must be an unsigned integer");
            }
            if (argument == "--seed") {
                options.benchmark.seed = *parsed;
            } else if (argument == "--average-server-bytes") {
                options.benchmark.maximum_average_server_to_client_bytes_per_second = *parsed;
            } else if (argument == "--rolling-server-bytes") {
                options.benchmark.maximum_rolling_one_second_server_to_client_bytes = *parsed;
            } else if (argument == "--average-client-bytes") {
                options.benchmark.maximum_average_server_to_client_bytes_per_second_per_client =
                    *parsed;
            } else {
                options.benchmark.maximum_rolling_one_second_server_to_client_bytes_per_client =
                    *parsed;
            }
        } else if (argument == "--tick-p95-ms" || argument == "--tick-p99-ms" ||
                   argument == "--tick-max-ms" || argument == "--input-ratio" ||
                   argument == "--client-input-ratio" || argument == "--correction-distance-m" ||
                   argument == "--final-state-error-m" || argument == "--displacement-m") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_number<double>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(
                    "multiplayer_network_impairment_benchmark.invalid_gate",
                    std::string(argument) + " must be a finite number");
            }
            if (argument == "--tick-p95-ms") {
                options.benchmark.maximum_server_tick_p95_ms = *parsed;
            } else if (argument == "--tick-p99-ms") {
                options.benchmark.maximum_server_tick_p99_ms = *parsed;
            } else if (argument == "--tick-max-ms") {
                options.benchmark.maximum_server_tick_ms = *parsed;
            } else if (argument == "--input-ratio") {
                options.benchmark.minimum_accepted_input_ratio = *parsed;
            } else if (argument == "--client-input-ratio") {
                options.benchmark.minimum_client_authoritative_input_ratio = *parsed;
            } else if (argument == "--correction-distance-m") {
                options.benchmark.maximum_correction_distance_m = *parsed;
            } else if (argument == "--final-state-error-m") {
                options.benchmark.maximum_final_state_error_m = *parsed;
            } else {
                options.benchmark.minimum_authoritative_displacement_m = *parsed;
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
                "multiplayer_network_impairment_benchmark.unknown_option",
                "unknown multiplayer network impairment benchmark option: " +
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
        << "usage: heartstead_multiplayer_network_impairment_benchmark [options]\n"
           "  --clients N                Concurrent clients (default 8)\n"
           "  --ticks N                  Measured 60 Hz movement ticks (default 600)\n"
           "  --neutral-tail-ticks N     Final measured neutral-input ticks (default 30)\n"
           "  --warmup-timeout-ticks N   Stable bootstrap bound (default 120)\n"
           "  --recovery-timeout-ticks N Final convergence bound (default 32)\n"
           "  --one-way-latency-ms N     Simulated one-way delay (default 50)\n"
           "  --variation-ms N           Uniform +/- delay variation (default 10)\n"
           "  --loss-basis-points N      Unreliable loss stimulus (default 200)\n"
           "  --tick-p95-ms N            Server tick P95 gate (default 12.5)\n"
           "  --tick-p99-ms N            Server tick P99 gate (default 16.667)\n"
           "  --tick-max-ms N            Server tick maximum gate (default 50)\n"
           "  --input-ratio N            Exclusive accepted-input ratio floor (default 0.90)\n"
           "  --client-input-ratio N     Exclusive per-client progress floor (default 0.90)\n"
           "  --hard-corrections N       Maximum aggregate hard corrections (default 8)\n"
           "  --hard-corrections-per-client N  Maximum hard corrections per client (default 1)\n"
           "  --correction-distance-m N  Exclusive correction-distance cap (default 1)\n"
           "  --final-state-error-m N     Final client/server position cap (default 0.10)\n"
           "  --average-server-bytes N   Aggregate average S->C bytes/s cap (default 1572864)\n"
           "  --rolling-server-bytes N   Aggregate rolling one-second S->C cap (default 1572864)\n"
           "  --average-client-bytes N   Per-client average S->C bytes/s cap (default 196608)\n"
           "  --rolling-client-bytes N   Per-client rolling one-second S->C cap (default 196608)\n"
           "  --eligible-messages N      Aggregate minimum loss-eligible messages (default 8)\n"
           "  --eligible-messages-per-client N  Per-client eligible minimum (default 1)\n"
           "  --minimum-drops N          Aggregate simulated drop minimum (default 8)\n"
           "  --minimum-drops-per-client N  Per-client simulated drop minimum (default 1)\n"
           "  --pending-impaired N       Aggregate in-flight impaired cap (default 256)\n"
           "  --pending-impaired-per-client N  Per-client in-flight cap (default 48)\n"
           "  --displacement-m N          Exclusive authoritative movement floor (default 5)\n"
           "  --seed N                    Deterministic impairment/world seed\n"
           "  --content-root PATH         Source content root\n"
           "  --enforce-gates             Return failure when any gate is missed\n"
           "  --output PATH               Write JSON; otherwise write JSON to stdout\n"
           "\nThe default deterministic stimulus is 100 ms RTT, uniform +/-10 ms configured "
           "delay variation, and 2% unreliable loss. Run it in an optimized build.\n";
}

[[nodiscard]] int run(const Options& options) {
#if !defined(NDEBUG)
    static_cast<void>(options);
    std::cerr << "multiplayer_network_impairment_benchmark.unoptimized_build: use an optimized "
                 "build\n";
    return 2;
#else
    const auto content_report = content::ContentValidation::validate(options.content_root);
    if (content_report.has_errors()) {
        std::cerr << "multiplayer_network_impairment_benchmark.invalid_content: content root "
                     "failed validation\n";
        return 1;
    }
    auto report =
        benchmark::run_multiplayer_network_impairment_benchmark(options.benchmark, content_report);
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
        std::cout << "wrote " << summary.measured_tick_count << " impaired ticks for "
                  << summary.client_count << " clients to " << options.output
                  << "; recovery=" << summary.recovery_ticks
                  << " server P95=" << summary.server_tick_p95_ms
                  << " ms P99=" << summary.server_tick_p99_ms
                  << " ms inputs=" << summary.accepted_input_ratio * 100.0
                  << "% min_client_progress="
                  << summary.minimum_client_authoritative_input_ratio * 100.0
                  << "% corrections=" << summary.hard_correction_count
                  << " loss=" << summary.observed_unreliable_loss_ratio * 100.0
                  << "% average_s2c=" << summary.average_server_to_client_bytes_per_second
                  << " B/s max_client_s2c="
                  << summary.maximum_client_average_server_to_client_bytes_per_second
                  << " B/s pending_peak=" << summary.peak_pending_impaired_message_count << '\n';
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
