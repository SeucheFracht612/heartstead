#include "game/runtime/multiplayer_network_impairment_benchmark.hpp"

#include "engine/content/content_validation.hpp"
#include "engine/math/vector.hpp"
#include "engine/movement/player_input.hpp"
#include "game/runtime/game_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <ranges>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

namespace heartstead::game::benchmark {

namespace {

using BenchmarkClock = std::chrono::steady_clock;

constexpr std::uint32_t benchmark_ticks_per_second = 60;
constexpr std::uint64_t benchmark_frame_time_us = 16'667;
constexpr std::int64_t benchmark_frame_time_ms = 17;
constexpr std::uint32_t stable_warmup_ticks = 4;
constexpr std::uint32_t stable_warmup_pending_impaired_limit = 32;

[[nodiscard]] bool finite_positive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool nearly_equal(double left, double right) noexcept {
    const auto scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= scale * 1.0e-9;
}

[[nodiscard]] std::uint64_t elapsed_microseconds(BenchmarkClock::time_point started) noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(BenchmarkClock::now() - started)
            .count();
    if (elapsed <= 0) {
        return 1;
    }
    const auto nanoseconds = static_cast<std::uint64_t>(elapsed);
    return 1 + (nanoseconds - 1) / 1'000;
}

template <typename Value>
[[nodiscard]] Value percentile(std::vector<Value> values, std::uint32_t percentile_value) {
    if (values.empty()) {
        return Value{};
    }
    std::ranges::sort(values);
    const auto rank = (values.size() * percentile_value + 99U) / 100U;
    return values[std::max<std::size_t>(1, rank) - 1];
}

[[nodiscard]] double percentile_ms(const std::vector<std::uint64_t>& values,
                                   std::uint32_t percentile_value) {
    return static_cast<double>(percentile(values, percentile_value)) / 1'000.0;
}

[[nodiscard]] core::Status write_text_file(const std::filesystem::path& path,
                                           const std::string& contents) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return core::Status::failure(
                "multiplayer_network_impairment_benchmark.create_directory_failed",
                "failed to create benchmark output directory: " + error.message());
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Status::failure("multiplayer_network_impairment_benchmark.open_output_failed",
                                     "failed to open benchmark output path");
    }
    output << contents;
    if (!output) {
        return core::Status::failure("multiplayer_network_impairment_benchmark.write_output_failed",
                                     "failed to write benchmark output");
    }
    return core::Status::ok();
}

void write_json_string(std::ostream& output, std::string_view value) {
    output << '"';
    for (const auto character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            output << character;
            break;
        }
    }
    output << '"';
}

void write_runtime_metadata(std::ostream& output, const profiling::RuntimeMetadata& runtime) {
    output << "  \"runtime\": {\n    \"engine_version\": ";
    write_json_string(output, runtime.engine_version);
    output << ",\n    \"git_commit\": ";
    write_json_string(output, runtime.git_commit);
    output << ",\n    \"git_dirty\": " << (runtime.git_dirty ? "true" : "false")
           << ",\n    \"build_configuration\": ";
    write_json_string(output, runtime.build_configuration);
    output << ",\n    \"compiler\": ";
    write_json_string(output, runtime.compiler);
    output << ",\n    \"platform\": ";
    write_json_string(output, runtime.platform);
    output << ",\n    \"architecture\": ";
    write_json_string(output, runtime.architecture);
    output << ",\n    \"operating_system\": ";
    write_json_string(output, runtime.operating_system);
    output << ",\n    \"cpu_model\": ";
    write_json_string(output, runtime.cpu_model);
    output << ",\n    \"logical_cpu_count\": " << runtime.logical_cpu_count
           << ",\n    \"tracy_enabled\": " << (runtime.tracy_enabled ? "true" : "false")
           << "\n  },\n";
}

[[nodiscard]] MultiplayerNetworkImpairmentBenchmarkSummary
summarize(const MultiplayerNetworkImpairmentBenchmarkConfig& config,
          std::span<const MultiplayerNetworkImpairmentTickSample> raw_ticks,
          std::uint32_t warmup_ticks) {
    MultiplayerNetworkImpairmentBenchmarkSummary summary;
    summary.warmup_ticks = warmup_ticks;
    summary.measured_tick_count = raw_ticks.size();
    std::vector<std::uint64_t> server_tick_times;
    std::vector<std::uint64_t> runtime_frame_times;
    std::vector<double> correction_distances;
    server_tick_times.reserve(raw_ticks.size());
    runtime_frame_times.reserve(raw_ticks.size());
    correction_distances.reserve(raw_ticks.size());

    std::uint64_t rolling_server_to_client_bytes = 0;
    for (std::size_t index = 0; index < raw_ticks.size(); ++index) {
        const auto& sample = raw_ticks[index];
        server_tick_times.push_back(sample.server_tick_time_us);
        runtime_frame_times.push_back(sample.runtime_frame_time_us);
        correction_distances.push_back(sample.maximum_correction_distance_m);
        summary.accepted_input_count += sample.accepted_input_count;
        summary.rejected_input_count += sample.rejected_input_count;
        summary.repeated_input_count += sample.repeated_input_count;
        summary.predicted_input_count += sample.predicted_input_count;
        summary.reconciled_input_count += sample.reconciled_input_count;
        summary.acknowledged_input_count += sample.acknowledged_input_count;
        summary.hard_correction_count += sample.hard_correction_count;
        summary.client_to_server_wire_bytes += sample.client_to_server_wire_bytes;
        summary.server_to_client_wire_bytes += sample.server_to_client_wire_bytes;
        summary.client_to_server_message_count += sample.client_to_server_message_count;
        summary.server_to_client_message_count += sample.server_to_client_message_count;
        summary.impairment_eligible_unreliable_message_count +=
            sample.impairment_eligible_unreliable_message_count;
        summary.simulated_dropped_unreliable_message_count +=
            sample.simulated_dropped_unreliable_message_count;
        summary.peak_pending_impaired_message_count = std::max(
            summary.peak_pending_impaired_message_count, sample.pending_impaired_message_count);
        summary.peak_pending_reliable_message_count = std::max(
            summary.peak_pending_reliable_message_count, sample.pending_reliable_message_count);
        summary.peak_pending_reliable_bytes =
            std::max(summary.peak_pending_reliable_bytes, sample.pending_reliable_bytes);
        summary.dropped_reliable_message_count += sample.dropped_reliable_message_count;
        summary.malformed_datagram_count += sample.malformed_datagram_count;
        summary.rejected_datagram_count += sample.rejected_datagram_count;
        summary.rate_limited_datagram_count += sample.rate_limited_datagram_count;
        summary.outbound_budget_dropped_unreliable_message_count +=
            sample.outbound_budget_dropped_unreliable_message_count;
        summary.disconnected_client_count += sample.disconnected_client_count;

        rolling_server_to_client_bytes += sample.server_to_client_wire_bytes;
        if (index >= benchmark_ticks_per_second) {
            rolling_server_to_client_bytes -=
                raw_ticks[index - benchmark_ticks_per_second].server_to_client_wire_bytes;
        }
        if (index + 1 >= benchmark_ticks_per_second) {
            summary.peak_rolling_one_second_server_to_client_bytes =
                std::max(summary.peak_rolling_one_second_server_to_client_bytes,
                         rolling_server_to_client_bytes);
        }
    }

    summary.server_tick_p50_ms = percentile_ms(server_tick_times, 50);
    summary.server_tick_p95_ms = percentile_ms(server_tick_times, 95);
    summary.server_tick_p99_ms = percentile_ms(server_tick_times, 99);
    summary.maximum_server_tick_ms =
        server_tick_times.empty()
            ? 0.0
            : static_cast<double>(*std::ranges::max_element(server_tick_times)) / 1'000.0;
    summary.runtime_frame_p50_ms = percentile_ms(runtime_frame_times, 50);
    summary.runtime_frame_p95_ms = percentile_ms(runtime_frame_times, 95);
    summary.runtime_frame_p99_ms = percentile_ms(runtime_frame_times, 99);
    summary.maximum_runtime_frame_ms =
        runtime_frame_times.empty()
            ? 0.0
            : static_cast<double>(*std::ranges::max_element(runtime_frame_times)) / 1'000.0;
    summary.correction_distance_p95_m = percentile(correction_distances, 95);
    summary.correction_distance_p99_m = percentile(correction_distances, 99);
    summary.maximum_correction_distance_m =
        correction_distances.empty() ? 0.0 : *std::ranges::max_element(correction_distances);
    if (!raw_ticks.empty()) {
        summary.accepted_input_ratio = static_cast<double>(summary.accepted_input_count) /
                                       static_cast<double>(config.measured_ticks);
        const auto measured_seconds =
            static_cast<double>(raw_ticks.size()) / benchmark_ticks_per_second;
        summary.average_server_to_client_bytes_per_second =
            static_cast<double>(summary.server_to_client_wire_bytes) / measured_seconds;
        summary.final_pending_impaired_message_count =
            raw_ticks.back().pending_impaired_message_count;
        summary.final_pending_reliable_message_count =
            raw_ticks.back().pending_reliable_message_count;
        summary.final_pending_reliable_bytes = raw_ticks.back().pending_reliable_bytes;
        summary.client_connected = raw_ticks.back().client_connected;
    }
    if (summary.impairment_eligible_unreliable_message_count != 0) {
        summary.observed_unreliable_loss_ratio =
            static_cast<double>(summary.simulated_dropped_unreliable_message_count) /
            static_cast<double>(summary.impairment_eligible_unreliable_message_count);
    }
    return summary;
}

[[nodiscard]] MultiplayerNetworkImpairmentBenchmarkGateEvaluation
evaluate_gates(const MultiplayerNetworkImpairmentBenchmarkConfig& config,
               const MultiplayerNetworkImpairmentBenchmarkSummary& summary) {
    MultiplayerNetworkImpairmentBenchmarkGateEvaluation gates;
    gates.evaluated = true;
    const auto maximum = [&gates](std::string metric, double actual, double limit) {
        if (actual > limit) {
            gates.violations.push_back({std::move(metric), actual, limit});
        }
    };
    const auto exclusive_maximum = [&gates](std::string metric, double actual, double limit) {
        if (actual >= limit) {
            gates.violations.push_back({std::move(metric), actual, limit});
        }
    };
    const auto minimum = [&gates](std::string metric, double actual, double limit) {
        if (actual < limit) {
            gates.violations.push_back({std::move(metric), actual, limit});
        }
    };
    const auto exclusive_minimum = [&gates](std::string metric, double actual, double limit) {
        if (actual <= limit) {
            gates.violations.push_back({std::move(metric), actual, limit});
        }
    };

    maximum("server_tick_p95_ms", summary.server_tick_p95_ms, config.maximum_server_tick_p95_ms);
    maximum("server_tick_p99_ms", summary.server_tick_p99_ms, config.maximum_server_tick_p99_ms);
    maximum("maximum_server_tick_ms", summary.maximum_server_tick_ms,
            config.maximum_server_tick_ms);
    exclusive_minimum("accepted_input_ratio", summary.accepted_input_ratio,
                      config.minimum_accepted_input_ratio);
    maximum("hard_correction_count", static_cast<double>(summary.hard_correction_count),
            static_cast<double>(config.maximum_hard_correction_count));
    exclusive_maximum("maximum_correction_distance_m", summary.maximum_correction_distance_m,
                      config.maximum_correction_distance_m);
    exclusive_maximum(
        "average_server_to_client_bytes_per_second",
        summary.average_server_to_client_bytes_per_second,
        static_cast<double>(config.maximum_average_server_to_client_bytes_per_second));
    exclusive_maximum(
        "peak_rolling_one_second_server_to_client_bytes",
        static_cast<double>(summary.peak_rolling_one_second_server_to_client_bytes),
        static_cast<double>(config.maximum_rolling_one_second_server_to_client_bytes));
    minimum("impairment_eligible_unreliable_message_count",
            static_cast<double>(summary.impairment_eligible_unreliable_message_count),
            static_cast<double>(config.minimum_impairment_eligible_unreliable_message_count));
    minimum("simulated_dropped_unreliable_message_count",
            static_cast<double>(summary.simulated_dropped_unreliable_message_count),
            static_cast<double>(config.minimum_simulated_unreliable_drop_count));
    maximum("peak_pending_impaired_message_count",
            static_cast<double>(summary.peak_pending_impaired_message_count),
            static_cast<double>(config.maximum_pending_impaired_message_count));
    exclusive_minimum("authoritative_displacement_m", summary.authoritative_displacement_m,
                      config.minimum_authoritative_displacement_m);
    minimum("client_connected", summary.client_connected ? 1.0 : 0.0, 1.0);
    maximum("final_pending_reliable_message_count",
            static_cast<double>(summary.final_pending_reliable_message_count), 0.0);
    maximum("final_pending_reliable_bytes",
            static_cast<double>(summary.final_pending_reliable_bytes), 0.0);
    maximum("dropped_reliable_message_count",
            static_cast<double>(summary.dropped_reliable_message_count), 0.0);
    maximum("malformed_datagram_count", static_cast<double>(summary.malformed_datagram_count), 0.0);
    maximum("rejected_datagram_count", static_cast<double>(summary.rejected_datagram_count), 0.0);
    maximum("rate_limited_datagram_count", static_cast<double>(summary.rate_limited_datagram_count),
            0.0);
    maximum("outbound_budget_dropped_unreliable_message_count",
            static_cast<double>(summary.outbound_budget_dropped_unreliable_message_count), 0.0);
    maximum("disconnected_client_count", static_cast<double>(summary.disconnected_client_count),
            0.0);
    gates.passed = gates.violations.empty();
    return gates;
}

} // namespace

core::Status MultiplayerNetworkImpairmentBenchmarkConfig::validate() const {
    if (measured_ticks < 100 || measured_ticks > 1'000'000 || warmup_timeout_ticks == 0 ||
        warmup_timeout_ticks > 1'000'000) {
        return core::Status::failure(
            "multiplayer_network_impairment_benchmark.invalid_workload",
            "benchmark requires 100-1,000,000 measured ticks and a bounded nonzero warmup");
    }
    if (simulated_one_way_latency_ms == 0 || simulated_one_way_latency_ms > 60'000 ||
        simulated_delay_variation_ms > simulated_one_way_latency_ms ||
        simulated_unreliable_loss_basis_points == 0 ||
        simulated_unreliable_loss_basis_points >= 10'000) {
        return core::Status::failure(
            "multiplayer_network_impairment_benchmark.invalid_impairment",
            "benchmark requires positive bounded latency and partial unreliable loss, with delay "
            "variation no greater than one-way latency");
    }
    if (!finite_positive(maximum_server_tick_p95_ms) ||
        !finite_positive(maximum_server_tick_p99_ms) || !finite_positive(maximum_server_tick_ms) ||
        maximum_server_tick_p95_ms > maximum_server_tick_p99_ms ||
        maximum_server_tick_p99_ms > maximum_server_tick_ms ||
        !finite_positive(minimum_accepted_input_ratio) || minimum_accepted_input_ratio > 1.0 ||
        !finite_positive(maximum_correction_distance_m) ||
        maximum_average_server_to_client_bytes_per_second == 0 ||
        maximum_rolling_one_second_server_to_client_bytes == 0 ||
        minimum_impairment_eligible_unreliable_message_count == 0 ||
        minimum_simulated_unreliable_drop_count == 0 ||
        maximum_pending_impaired_message_count == 0 ||
        !finite_positive(minimum_authoritative_displacement_m)) {
        return core::Status::failure(
            "multiplayer_network_impairment_benchmark.invalid_gates",
            "tick, input, correction, traffic, impairment, backlog, and displacement gates must "
            "be finite and internally ordered");
    }
    return core::Status::ok();
}

core::Status MultiplayerNetworkImpairmentBenchmarkReport::validate() const {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    if (raw_ticks.size() != config.measured_ticks ||
        summary.measured_tick_count != raw_ticks.size() || summary.warmup_ticks == 0 ||
        summary.warmup_ticks > config.warmup_timeout_ticks) {
        return core::Status::failure(
            "multiplayer_network_impairment_benchmark.incomplete_report",
            "benchmark report is missing warmup or raw measured tick evidence");
    }
    std::uint64_t previous_authoritative_tick = 0;
    for (std::size_t index = 0; index < raw_ticks.size(); ++index) {
        const auto& sample = raw_ticks[index];
        if (sample.ordinal != index + 1 || sample.input_sequence != index + 1 ||
            sample.authoritative_tick <= previous_authoritative_tick ||
            sample.runtime_frame_time_us == 0 || sample.server_tick_time_us == 0 ||
            !std::isfinite(sample.simulation_time_ms) || sample.simulation_time_ms < 0.0 ||
            !std::isfinite(sample.maximum_correction_distance_m) ||
            sample.maximum_correction_distance_m < 0.0 || !sample.client_connected ||
            sample.simulated_dropped_unreliable_message_count >
                sample.impairment_eligible_unreliable_message_count) {
            return core::Status::failure(
                "multiplayer_network_impairment_benchmark.invalid_tick_sample",
                "raw ticks must be ordered, timed, connected, finite, and retain a valid loss "
                "denominator");
        }
        previous_authoritative_tick = sample.authoritative_tick;
    }

    auto expected = summarize(config, raw_ticks, summary.warmup_ticks);
    expected.authoritative_displacement_m = summary.authoritative_displacement_m;
    expected.final_acknowledged_input_sequence = summary.final_acknowledged_input_sequence;
    expected.client_connected = summary.client_connected;
    const auto exact_summary =
        expected.measured_tick_count == summary.measured_tick_count &&
        expected.accepted_input_count == summary.accepted_input_count &&
        expected.rejected_input_count == summary.rejected_input_count &&
        expected.repeated_input_count == summary.repeated_input_count &&
        expected.hard_correction_count == summary.hard_correction_count &&
        expected.client_to_server_wire_bytes == summary.client_to_server_wire_bytes &&
        expected.server_to_client_wire_bytes == summary.server_to_client_wire_bytes &&
        expected.impairment_eligible_unreliable_message_count ==
            summary.impairment_eligible_unreliable_message_count &&
        expected.simulated_dropped_unreliable_message_count ==
            summary.simulated_dropped_unreliable_message_count &&
        expected.peak_pending_impaired_message_count ==
            summary.peak_pending_impaired_message_count &&
        expected.final_pending_impaired_message_count ==
            summary.final_pending_impaired_message_count &&
        expected.final_pending_reliable_message_count ==
            summary.final_pending_reliable_message_count &&
        expected.final_pending_reliable_bytes == summary.final_pending_reliable_bytes &&
        nearly_equal(expected.server_tick_p95_ms, summary.server_tick_p95_ms) &&
        nearly_equal(expected.server_tick_p99_ms, summary.server_tick_p99_ms) &&
        nearly_equal(expected.accepted_input_ratio, summary.accepted_input_ratio) &&
        nearly_equal(expected.maximum_correction_distance_m,
                     summary.maximum_correction_distance_m) &&
        nearly_equal(expected.average_server_to_client_bytes_per_second,
                     summary.average_server_to_client_bytes_per_second) &&
        nearly_equal(expected.observed_unreliable_loss_ratio,
                     summary.observed_unreliable_loss_ratio);
    if (!exact_summary || !std::isfinite(summary.authoritative_displacement_m) ||
        summary.authoritative_displacement_m < 0.0) {
        return core::Status::failure("multiplayer_network_impairment_benchmark.invalid_summary",
                                     "benchmark summary does not reproduce its raw tick evidence");
    }
    const auto expected_gate_pass = gates.violations.empty();
    if (!gates.evaluated || gates.passed != expected_gate_pass) {
        return core::Status::failure(
            "multiplayer_network_impairment_benchmark.invalid_gate_evaluation",
            "benchmark gate state must be evaluated and agree with retained violations");
    }
    return core::Status::ok();
}

bool MultiplayerNetworkImpairmentBenchmarkReport::gates_passed() const noexcept {
    return gates.evaluated && gates.passed;
}

std::string MultiplayerNetworkImpairmentBenchmarkReport::to_json() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "{\n  \"schema_version\": " << schema_version
           << ",\n  \"benchmark\": \"multiplayer_network_impairment\",\n";
    write_runtime_metadata(output, runtime);
    output << "  \"config\": {\n"
           << "    \"seed\": " << config.seed << ",\n"
           << "    \"ticks_per_second\": " << benchmark_ticks_per_second << ",\n"
           << "    \"measured_ticks\": " << config.measured_ticks << ",\n"
           << "    \"warmup_timeout_ticks\": " << config.warmup_timeout_ticks << ",\n"
           << "    \"simulated_one_way_latency_ms\": " << config.simulated_one_way_latency_ms
           << ",\n"
           << "    \"simulated_round_trip_latency_ms\": "
           << config.simulated_one_way_latency_ms * 2U << ",\n"
           << "    \"simulated_delay_variation_ms\": " << config.simulated_delay_variation_ms
           << ",\n"
           << "    \"delay_variation_distribution\": \"uniform\",\n"
           << "    \"simulated_unreliable_loss_basis_points\": "
           << config.simulated_unreliable_loss_basis_points << ",\n"
           << "    \"enforce_gates\": " << (config.enforce_gates ? "true" : "false") << ",\n"
           << "    \"maximum_server_tick_p95_ms\": " << config.maximum_server_tick_p95_ms << ",\n"
           << "    \"maximum_server_tick_p99_ms\": " << config.maximum_server_tick_p99_ms << ",\n"
           << "    \"maximum_server_tick_ms\": " << config.maximum_server_tick_ms << ",\n"
           << "    \"minimum_accepted_input_ratio\": " << config.minimum_accepted_input_ratio
           << ",\n"
           << "    \"maximum_hard_correction_count\": " << config.maximum_hard_correction_count
           << ",\n"
           << "    \"maximum_correction_distance_m\": " << config.maximum_correction_distance_m
           << ",\n"
           << "    \"maximum_average_server_to_client_bytes_per_second\": "
           << config.maximum_average_server_to_client_bytes_per_second << ",\n"
           << "    \"maximum_rolling_one_second_server_to_client_bytes\": "
           << config.maximum_rolling_one_second_server_to_client_bytes << ",\n"
           << "    \"minimum_impairment_eligible_unreliable_message_count\": "
           << config.minimum_impairment_eligible_unreliable_message_count << ",\n"
           << "    \"minimum_simulated_unreliable_drop_count\": "
           << config.minimum_simulated_unreliable_drop_count << ",\n"
           << "    \"maximum_pending_impaired_message_count\": "
           << config.maximum_pending_impaired_message_count << ",\n"
           << "    \"minimum_authoritative_displacement_m\": "
           << config.minimum_authoritative_displacement_m << "\n  },\n";

    output << "  \"summary\": {\n"
           << "    \"warmup_ticks\": " << summary.warmup_ticks << ",\n"
           << "    \"measured_tick_count\": " << summary.measured_tick_count << ",\n"
           << "    \"server_tick_p50_ms\": " << summary.server_tick_p50_ms << ",\n"
           << "    \"server_tick_p95_ms\": " << summary.server_tick_p95_ms << ",\n"
           << "    \"server_tick_p99_ms\": " << summary.server_tick_p99_ms << ",\n"
           << "    \"maximum_server_tick_ms\": " << summary.maximum_server_tick_ms << ",\n"
           << "    \"runtime_frame_p50_ms\": " << summary.runtime_frame_p50_ms << ",\n"
           << "    \"runtime_frame_p95_ms\": " << summary.runtime_frame_p95_ms << ",\n"
           << "    \"runtime_frame_p99_ms\": " << summary.runtime_frame_p99_ms << ",\n"
           << "    \"maximum_runtime_frame_ms\": " << summary.maximum_runtime_frame_ms << ",\n"
           << "    \"accepted_input_count\": " << summary.accepted_input_count << ",\n"
           << "    \"rejected_input_count\": " << summary.rejected_input_count << ",\n"
           << "    \"repeated_input_count\": " << summary.repeated_input_count << ",\n"
           << "    \"predicted_input_count\": " << summary.predicted_input_count << ",\n"
           << "    \"reconciled_input_count\": " << summary.reconciled_input_count << ",\n"
           << "    \"acknowledged_input_count\": " << summary.acknowledged_input_count << ",\n"
           << "    \"accepted_input_ratio\": " << summary.accepted_input_ratio << ",\n"
           << "    \"hard_correction_count\": " << summary.hard_correction_count << ",\n"
           << "    \"correction_distance_p95_m\": " << summary.correction_distance_p95_m << ",\n"
           << "    \"correction_distance_p99_m\": " << summary.correction_distance_p99_m << ",\n"
           << "    \"maximum_correction_distance_m\": " << summary.maximum_correction_distance_m
           << ",\n"
           << "    \"client_to_server_wire_bytes\": " << summary.client_to_server_wire_bytes
           << ",\n"
           << "    \"server_to_client_wire_bytes\": " << summary.server_to_client_wire_bytes
           << ",\n"
           << "    \"average_server_to_client_bytes_per_second\": "
           << summary.average_server_to_client_bytes_per_second << ",\n"
           << "    \"peak_rolling_one_second_server_to_client_bytes\": "
           << summary.peak_rolling_one_second_server_to_client_bytes << ",\n"
           << "    \"client_to_server_message_count\": " << summary.client_to_server_message_count
           << ",\n"
           << "    \"server_to_client_message_count\": " << summary.server_to_client_message_count
           << ",\n"
           << "    \"impairment_eligible_unreliable_message_count\": "
           << summary.impairment_eligible_unreliable_message_count << ",\n"
           << "    \"simulated_dropped_unreliable_message_count\": "
           << summary.simulated_dropped_unreliable_message_count << ",\n"
           << "    \"observed_unreliable_loss_ratio\": " << summary.observed_unreliable_loss_ratio
           << ",\n"
           << "    \"peak_pending_impaired_message_count\": "
           << summary.peak_pending_impaired_message_count << ",\n"
           << "    \"final_pending_impaired_message_count\": "
           << summary.final_pending_impaired_message_count << ",\n"
           << "    \"peak_pending_reliable_message_count\": "
           << summary.peak_pending_reliable_message_count << ",\n"
           << "    \"final_pending_reliable_message_count\": "
           << summary.final_pending_reliable_message_count << ",\n"
           << "    \"peak_pending_reliable_bytes\": " << summary.peak_pending_reliable_bytes
           << ",\n"
           << "    \"final_pending_reliable_bytes\": " << summary.final_pending_reliable_bytes
           << ",\n"
           << "    \"dropped_reliable_message_count\": " << summary.dropped_reliable_message_count
           << ",\n"
           << "    \"malformed_datagram_count\": " << summary.malformed_datagram_count << ",\n"
           << "    \"rejected_datagram_count\": " << summary.rejected_datagram_count << ",\n"
           << "    \"rate_limited_datagram_count\": " << summary.rate_limited_datagram_count
           << ",\n"
           << "    \"outbound_budget_dropped_unreliable_message_count\": "
           << summary.outbound_budget_dropped_unreliable_message_count << ",\n"
           << "    \"disconnected_client_count\": " << summary.disconnected_client_count << ",\n"
           << "    \"authoritative_displacement_m\": " << summary.authoritative_displacement_m
           << ",\n"
           << "    \"final_acknowledged_input_sequence\": "
           << summary.final_acknowledged_input_sequence << ",\n"
           << "    \"client_connected\": " << (summary.client_connected ? "true" : "false")
           << "\n  },\n";

    output << "  \"gates\": {\n    \"evaluated\": " << (gates.evaluated ? "true" : "false")
           << ",\n    \"passed\": " << (gates.passed ? "true" : "false")
           << ",\n    \"violations\": [";
    if (!gates.violations.empty()) {
        output << '\n';
    }
    for (std::size_t index = 0; index < gates.violations.size(); ++index) {
        const auto& violation = gates.violations[index];
        output << "      {\"metric\": ";
        write_json_string(output, violation.metric);
        output << ", \"actual\": " << violation.actual << ", \"limit\": " << violation.limit << '}';
        output << (index + 1 == gates.violations.size() ? "\n" : ",\n");
    }
    output << "    ]\n  },\n  \"raw_ticks\": [\n";
    for (std::size_t index = 0; index < raw_ticks.size(); ++index) {
        const auto& sample = raw_ticks[index];
        output << "    {\"ordinal\": " << sample.ordinal
               << ", \"input_sequence\": " << sample.input_sequence
               << ", \"authoritative_tick\": " << sample.authoritative_tick
               << ", \"runtime_frame_time_us\": " << sample.runtime_frame_time_us
               << ", \"server_tick_time_us\": " << sample.server_tick_time_us
               << ", \"simulation_time_ms\": " << sample.simulation_time_ms
               << ", \"accepted_input_count\": " << sample.accepted_input_count
               << ", \"rejected_input_count\": " << sample.rejected_input_count
               << ", \"repeated_input_count\": " << sample.repeated_input_count
               << ", \"predicted_input_count\": " << sample.predicted_input_count
               << ", \"reconciled_input_count\": " << sample.reconciled_input_count
               << ", \"acknowledged_input_count\": " << sample.acknowledged_input_count
               << ", \"hard_correction_count\": " << sample.hard_correction_count
               << ", \"maximum_correction_distance_m\": " << sample.maximum_correction_distance_m
               << ", \"client_to_server_wire_bytes\": " << sample.client_to_server_wire_bytes
               << ", \"server_to_client_wire_bytes\": " << sample.server_to_client_wire_bytes
               << ", \"client_to_server_message_count\": " << sample.client_to_server_message_count
               << ", \"server_to_client_message_count\": " << sample.server_to_client_message_count
               << ", \"impairment_eligible_unreliable_message_count\": "
               << sample.impairment_eligible_unreliable_message_count
               << ", \"simulated_dropped_unreliable_message_count\": "
               << sample.simulated_dropped_unreliable_message_count
               << ", \"pending_impaired_message_count\": " << sample.pending_impaired_message_count
               << ", \"pending_reliable_message_count\": " << sample.pending_reliable_message_count
               << ", \"pending_reliable_bytes\": " << sample.pending_reliable_bytes
               << ", \"dropped_reliable_message_count\": " << sample.dropped_reliable_message_count
               << ", \"malformed_datagram_count\": " << sample.malformed_datagram_count
               << ", \"rejected_datagram_count\": " << sample.rejected_datagram_count
               << ", \"rate_limited_datagram_count\": " << sample.rate_limited_datagram_count
               << ", \"outbound_budget_dropped_unreliable_message_count\": "
               << sample.outbound_budget_dropped_unreliable_message_count
               << ", \"disconnected_client_count\": " << sample.disconnected_client_count
               << ", \"client_connected\": " << (sample.client_connected ? "true" : "false") << '}';
        output << (index + 1 == raw_ticks.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

core::Status
MultiplayerNetworkImpairmentBenchmarkReport::write_json(const std::filesystem::path& path) const {
    auto status = validate();
    if (!status) {
        return status;
    }
    return write_text_file(path, to_json());
}

core::Result<MultiplayerNetworkImpairmentBenchmarkReport>
run_multiplayer_network_impairment_benchmark(
    const MultiplayerNetworkImpairmentBenchmarkConfig& config,
    const content::ContentValidationReport& content_report) {
    auto status = config.validate();
    if (!status) {
        return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
            status.error().code, status.error().message);
    }
    if (content_report.has_errors()) {
        return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
            "multiplayer_network_impairment_benchmark.invalid_content",
            "benchmark content validation contains errors");
    }

    auto initialized = GameRuntime::initialize({}, content_report);
    if (!initialized) {
        return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
            initialized.error().code, initialized.error().message);
    }
    auto runtime = std::move(initialized).value();
    RuntimeConfiguration runtime_config;
    runtime_config.fixed_step = {benchmark_ticks_per_second, 4, 250'000};
    runtime_config.simulated_network_one_way_latency_ms = config.simulated_one_way_latency_ms;
    runtime_config.simulated_network_jitter_ms = config.simulated_delay_variation_ms;
    runtime_config.simulated_network_unreliable_loss_basis_points =
        config.simulated_unreliable_loss_basis_points;
    runtime_config.simulated_network_seed = config.seed;

    auto metadata = content::save_metadata_from_content_report(
        content_report, "multiplayer-network-impairment-benchmark", config.seed);
    if (!metadata) {
        return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
            metadata.error().code, metadata.error().message);
    }
    SessionRequest request;
    request.metadata = std::move(metadata).value();
    request.scenario_id = "base:scenarios/homestead";
    status = runtime.start_session(runtime_config, std::move(request));
    if (!status) {
        return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
            status.error().code, status.error().message);
    }
    auto* session = runtime.session();
    if (session == nullptr || session->server() == nullptr || session->client() == nullptr) {
        return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
            "multiplayer_network_impairment_benchmark.missing_runtime",
            "automated benchmark session did not create a server and client runtime");
    }

    std::int64_t now_ms = 0;
    std::uint32_t warmup_ticks = 0;
    std::uint32_t stable_ticks = 0;
    while (warmup_ticks < config.warmup_timeout_ticks) {
        now_ms += benchmark_frame_time_ms;
        auto frame = runtime.run_frame({benchmark_frame_time_us, now_ms});
        if (!frame) {
            return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
                frame.error().code, frame.error().message);
        }
        ++warmup_ticks;
        if (frame.value().client_presentation_error.has_value() ||
            frame.value().server_ticks.size() != 1) {
            return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
                "multiplayer_network_impairment_benchmark.invalid_warmup_frame",
                "warmup requires exactly one successful server tick and client synchronization");
        }
        const auto& tick = frame.value().server_ticks.front();
        const auto ready = session->client()->is_connected() &&
                           session->client()->local_player_snapshot() != nullptr &&
                           session->server()->host().pending_outbound_message_count() == 0 &&
                           tick.chunk_subscriptions.partial_snapshot_count == 0 &&
                           tick.commands.transport_pending_impaired_message_count <=
                               stable_warmup_pending_impaired_limit;
        stable_ticks = ready ? stable_ticks + 1 : 0;
        if (stable_ticks >= stable_warmup_ticks) {
            break;
        }
    }
    if (stable_ticks < stable_warmup_ticks) {
        return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
            "multiplayer_network_impairment_benchmark.warmup_timeout",
            "client bootstrap and impaired transport did not reach a stable bounded state");
    }

    const auto* initial_snapshot = session->client()->local_player_snapshot();
    if (initial_snapshot == nullptr) {
        return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
            "multiplayer_network_impairment_benchmark.missing_initial_player",
            "connected benchmark client has no local player snapshot");
    }
    const auto initial_position = initial_snapshot->state.position;

    MultiplayerNetworkImpairmentBenchmarkReport report;
    report.config = config;
    report.runtime = profiling::query_runtime_metadata();
    report.raw_ticks.reserve(config.measured_ticks);
    for (std::uint32_t ordinal = 1; ordinal <= config.measured_ticks; ++ordinal) {
        movement::PlayerInputFrame input;
        input.tick = ordinal;
        input.sequence = ordinal;
        input.move_z = std::numeric_limits<std::int16_t>::max();
        status = session->submit_player_input(input, now_ms);
        if (!status) {
            return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
                status.error().code, status.error().message);
        }

        now_ms += benchmark_frame_time_ms;
        const auto frame_started = BenchmarkClock::now();
        auto frame = runtime.run_frame({benchmark_frame_time_us, now_ms});
        const auto frame_time_us = elapsed_microseconds(frame_started);
        if (!frame) {
            return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
                frame.error().code, frame.error().message);
        }
        if (frame.value().client_presentation_error.has_value()) {
            return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
                frame.value().client_presentation_error->code,
                frame.value().client_presentation_error->message);
        }
        if (frame.value().server_ticks.size() != 1) {
            return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
                "multiplayer_network_impairment_benchmark.invalid_fixed_step",
                "every measured frame must execute exactly one authoritative tick");
        }
        const auto& tick = frame.value().server_ticks.front();
        MultiplayerNetworkImpairmentTickSample sample;
        sample.ordinal = ordinal;
        sample.input_sequence = input.sequence;
        sample.authoritative_tick = tick.simulation.tick;
        sample.runtime_frame_time_us = frame_time_us;
        sample.server_tick_time_us = tick.wall_time_us;
        sample.simulation_time_ms = tick.simulation.total_ms;
        sample.accepted_input_count = tick.accepted_movement_input_count;
        sample.rejected_input_count = tick.rejected_movement_input_count;
        sample.repeated_input_count = tick.repeated_input_count;
        sample.predicted_input_count = frame.value().client.predicted_input_count;
        sample.reconciled_input_count = frame.value().client.reconciled_input_count;
        sample.acknowledged_input_count = frame.value().client.acknowledged_input_count;
        sample.hard_correction_count = frame.value().client.hard_correction_count;
        sample.maximum_correction_distance_m = frame.value().client.maximum_correction_distance;
        sample.client_to_server_wire_bytes = tick.commands.transport_client_to_server_bytes;
        sample.server_to_client_wire_bytes = tick.commands.transport_server_to_client_bytes;
        sample.client_to_server_message_count =
            tick.commands.transport_client_to_server_message_count;
        sample.server_to_client_message_count =
            tick.commands.transport_server_to_client_message_count;
        sample.impairment_eligible_unreliable_message_count =
            tick.commands.transport_impairment_eligible_unreliable_message_count;
        sample.simulated_dropped_unreliable_message_count =
            tick.commands.transport_simulated_dropped_unreliable_message_count;
        sample.pending_impaired_message_count =
            tick.commands.transport_pending_impaired_message_count;
        sample.pending_reliable_message_count =
            session->server()->host().pending_outbound_message_count();
        sample.pending_reliable_bytes = session->server()->host().pending_outbound_bytes();
        sample.dropped_reliable_message_count =
            tick.commands.transport_dropped_reliable_message_count;
        sample.malformed_datagram_count = tick.commands.transport_malformed_datagram_count;
        sample.rejected_datagram_count = tick.commands.transport_rejected_datagram_count;
        sample.rate_limited_datagram_count = tick.commands.transport_rate_limited_datagram_count;
        sample.outbound_budget_dropped_unreliable_message_count =
            tick.commands.outbound_budget_dropped_unreliable_message_count;
        sample.disconnected_client_count = static_cast<std::uint32_t>(
            tick.commands.disconnected_clients.size() +
            tick.commands.outbound_delivery.overload_disconnected_clients.size());
        sample.client_connected = session->client()->is_connected();
        report.raw_ticks.push_back(std::move(sample));
    }

    report.summary = summarize(config, report.raw_ticks, warmup_ticks);
    const auto client_id = session->client()->client_id();
    const auto* authoritative = session->server()->player_for_client(client_id);
    const auto* final_snapshot = session->client()->local_player_snapshot();
    if (authoritative == nullptr || final_snapshot == nullptr) {
        return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
            "multiplayer_network_impairment_benchmark.missing_final_player",
            "benchmark lost authoritative or replicated player state");
    }
    const auto displacement = authoritative->state.position.relative_to(initial_position.anchor) -
                              initial_position.local_offset;
    report.summary.authoritative_displacement_m = math::length(displacement);
    report.summary.final_acknowledged_input_sequence =
        final_snapshot->last_processed_input_sequence;
    report.summary.client_connected = session->client()->is_connected();
    report.gates = evaluate_gates(config, report.summary);

    status = report.validate();
    if (!status) {
        return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
            status.error().code, status.error().message);
    }
    status = runtime.shutdown();
    if (!status) {
        return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
            status.error().code, status.error().message);
    }
    return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::success(std::move(report));
}

} // namespace heartstead::game::benchmark
