#include "game/runtime/multiplayer_network_impairment_benchmark.hpp"

#include "engine/content/content_validation.hpp"
#include "engine/math/vector.hpp"
#include "engine/movement/player_input.hpp"
#include "game/runtime/game_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

namespace heartstead::game::benchmark {

namespace {

using BenchmarkClock = std::chrono::steady_clock;

constexpr std::uint32_t benchmark_ticks_per_second = 60;
constexpr double benchmark_fixed_delta_seconds = 1.0 / benchmark_ticks_per_second;
constexpr std::int64_t benchmark_frame_time_ms = 17;
constexpr std::uint32_t stable_warmup_ticks = 4;
constexpr std::uint32_t stable_warmup_pending_impaired_limit_per_client = 32;

[[nodiscard]] std::uint32_t maximum_impairment_delivery_ticks(
    const MultiplayerNetworkImpairmentBenchmarkConfig& config) noexcept {
    const auto maximum_one_way_delay_ms =
        static_cast<std::uint64_t>(config.simulated_one_way_latency_ms) +
        config.simulated_delay_variation_ms;
    return static_cast<std::uint32_t>(
        (maximum_one_way_delay_ms + static_cast<std::uint64_t>(benchmark_frame_time_ms) - 1U) /
        static_cast<std::uint64_t>(benchmark_frame_time_ms));
}

[[nodiscard]] std::uint32_t
required_stable_warmup_ticks(const MultiplayerNetworkImpairmentBenchmarkConfig& config) noexcept {
    return stable_warmup_ticks + 2U * maximum_impairment_delivery_ticks(config);
}

[[nodiscard]] std::uint32_t
minimum_warmup_ticks(const MultiplayerNetworkImpairmentBenchmarkConfig& config) noexcept {
    return stable_warmup_ticks + 6U * maximum_impairment_delivery_ticks(config);
}

struct BenchmarkClient {
    core::NetId id;
    ClientRuntime* runtime = nullptr;
    std::unique_ptr<ClientRuntime> owned_runtime;
    world::WorldPosition initial_authoritative_position;
    std::optional<movement::PlayerInputBundle> recovery_bundle;
};

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

[[nodiscard]] double position_distance(const world::WorldPosition& left,
                                       const world::WorldPosition& right) noexcept {
    return math::length(right.relative_to(left.anchor) - left.local_offset);
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

void refresh_terminal_summary(MultiplayerNetworkImpairmentBenchmarkSummary& summary) {
    if (summary.clients.empty()) {
        summary.minimum_authoritative_displacement_m = 0.0;
        summary.minimum_final_authoritative_input_sequence = 0;
        summary.minimum_final_client_input_sequence = 0;
        summary.maximum_final_unacknowledged_prediction_input_count = 0;
        summary.maximum_final_state_error_m = 0.0;
        summary.all_clients_connected = false;
        return;
    }

    summary.minimum_authoritative_displacement_m = std::numeric_limits<double>::infinity();
    summary.minimum_final_authoritative_input_sequence = std::numeric_limits<std::uint64_t>::max();
    summary.minimum_final_client_input_sequence = std::numeric_limits<std::uint64_t>::max();
    summary.maximum_final_unacknowledged_prediction_input_count = 0;
    summary.maximum_final_state_error_m = 0.0;
    summary.all_clients_connected = true;
    for (const auto& client : summary.clients) {
        summary.minimum_authoritative_displacement_m = std::min(
            summary.minimum_authoritative_displacement_m, client.authoritative_displacement_m);
        summary.minimum_final_authoritative_input_sequence =
            std::min(summary.minimum_final_authoritative_input_sequence,
                     client.final_authoritative_input_sequence);
        summary.minimum_final_client_input_sequence = std::min(
            summary.minimum_final_client_input_sequence, client.final_client_input_sequence);
        summary.maximum_final_unacknowledged_prediction_input_count =
            std::max(summary.maximum_final_unacknowledged_prediction_input_count,
                     client.final_unacknowledged_prediction_input_count);
        summary.maximum_final_state_error_m =
            std::max(summary.maximum_final_state_error_m, client.final_state_error_m);
        summary.all_clients_connected = summary.all_clients_connected && client.connected;
    }
}

[[nodiscard]] MultiplayerNetworkImpairmentBenchmarkSummary
summarize(const MultiplayerNetworkImpairmentBenchmarkConfig& config,
          std::span<const MultiplayerNetworkImpairmentTickSample> raw_ticks,
          std::span<const MultiplayerNetworkImpairmentTickSample> recovery_ticks,
          std::uint32_t warmup_ticks) {
    MultiplayerNetworkImpairmentBenchmarkSummary summary;
    summary.warmup_ticks = warmup_ticks;
    summary.recovery_ticks = static_cast<std::uint32_t>(recovery_ticks.size());
    summary.measured_tick_count = raw_ticks.size();
    summary.client_count = config.client_count;
    if (!raw_ticks.empty()) {
        summary.clients.reserve(raw_ticks.front().clients.size());
        for (const auto& client : raw_ticks.front().clients) {
            MultiplayerNetworkImpairmentClientSummary client_summary;
            client_summary.client_id = client.client_id;
            summary.clients.push_back(client_summary);
        }
    }

    std::vector<std::uint64_t> server_tick_times;
    std::vector<std::uint64_t> runtime_frame_times;
    std::vector<double> correction_distances;
    std::vector<std::vector<double>> client_correction_distances(summary.clients.size());
    server_tick_times.reserve(raw_ticks.size());
    runtime_frame_times.reserve(raw_ticks.size());
    correction_distances.reserve((raw_ticks.size() + recovery_ticks.size()) *
                                 summary.clients.size());

    const auto accumulate_common = [&](const MultiplayerNetworkImpairmentTickSample& sample) {
        summary.predicted_input_count += sample.predicted_input_count;
        summary.reconciled_input_count += sample.reconciled_input_count;
        summary.acknowledged_input_count += sample.acknowledged_input_count;
        summary.hard_correction_count += sample.hard_correction_count;
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
        for (std::size_t index = 0; index < sample.clients.size(); ++index) {
            const auto& client = sample.clients[index];
            auto& client_summary = summary.clients[index];
            client_summary.predicted_input_count += client.predicted_input_count;
            client_summary.reconciled_input_count += client.reconciled_input_count;
            client_summary.acknowledged_input_count += client.acknowledged_input_count;
            client_summary.hard_correction_count += client.hard_correction_count;
            client_summary.peak_pending_impaired_message_count =
                std::max(client_summary.peak_pending_impaired_message_count,
                         client.pending_impaired_message_count);
            correction_distances.push_back(client.maximum_correction_distance_m);
            client_correction_distances[index].push_back(client.maximum_correction_distance_m);
        }
    };

    std::uint64_t rolling_server_to_client_bytes = 0;
    std::vector<std::uint64_t> rolling_client_server_to_client_bytes(summary.clients.size(), 0);
    for (std::size_t index = 0; index < raw_ticks.size(); ++index) {
        const auto& sample = raw_ticks[index];
        server_tick_times.push_back(sample.server_tick_time_us);
        runtime_frame_times.push_back(sample.runtime_frame_time_us);
        accumulate_common(sample);
        summary.accepted_input_count += sample.accepted_input_count;
        summary.rejected_input_count += sample.rejected_input_count;
        summary.repeated_input_count += sample.repeated_input_count;
        summary.client_to_server_wire_bytes += sample.client_to_server_wire_bytes;
        summary.server_to_client_wire_bytes += sample.server_to_client_wire_bytes;
        summary.client_to_server_message_count += sample.client_to_server_message_count;
        summary.server_to_client_message_count += sample.server_to_client_message_count;
        summary.impairment_eligible_unreliable_message_count +=
            sample.impairment_eligible_unreliable_message_count;
        summary.simulated_dropped_unreliable_message_count +=
            sample.simulated_dropped_unreliable_message_count;

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

        for (std::size_t client_index = 0; client_index < sample.clients.size(); ++client_index) {
            const auto& client = sample.clients[client_index];
            auto& client_summary = summary.clients[client_index];
            client_summary.client_to_server_wire_bytes += client.client_to_server_wire_bytes;
            client_summary.server_to_client_wire_bytes += client.server_to_client_wire_bytes;
            client_summary.client_to_server_message_count += client.client_to_server_message_count;
            client_summary.server_to_client_message_count += client.server_to_client_message_count;
            client_summary.impairment_eligible_unreliable_message_count +=
                client.impairment_eligible_unreliable_message_count;
            client_summary.simulated_dropped_unreliable_message_count +=
                client.simulated_dropped_unreliable_message_count;
            client_summary.measured_final_authoritative_input_sequence =
                client.authoritative_input_sequence;

            auto& rolling = rolling_client_server_to_client_bytes[client_index];
            rolling += client.server_to_client_wire_bytes;
            if (index >= benchmark_ticks_per_second) {
                rolling -= raw_ticks[index - benchmark_ticks_per_second]
                               .clients[client_index]
                               .server_to_client_wire_bytes;
            }
            if (index + 1 >= benchmark_ticks_per_second) {
                client_summary.peak_rolling_one_second_server_to_client_bytes = std::max(
                    client_summary.peak_rolling_one_second_server_to_client_bytes, rolling);
            }
        }
    }
    for (const auto& sample : recovery_ticks) {
        accumulate_common(sample);
        summary.maximum_recovery_server_tick_ms =
            std::max(summary.maximum_recovery_server_tick_ms,
                     static_cast<double>(sample.server_tick_time_us) / 1'000.0);
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
        const auto offered_input_count =
            static_cast<double>(raw_ticks.size()) * static_cast<double>(config.client_count);
        summary.accepted_input_ratio =
            static_cast<double>(summary.accepted_input_count) / offered_input_count;
        const auto measured_seconds =
            static_cast<double>(raw_ticks.size()) / benchmark_ticks_per_second;
        summary.average_server_to_client_bytes_per_second =
            static_cast<double>(summary.server_to_client_wire_bytes) / measured_seconds;
        for (auto& client : summary.clients) {
            client.average_server_to_client_bytes_per_second =
                static_cast<double>(client.server_to_client_wire_bytes) / measured_seconds;
        }
    }
    if (summary.impairment_eligible_unreliable_message_count != 0) {
        summary.observed_unreliable_loss_ratio =
            static_cast<double>(summary.simulated_dropped_unreliable_message_count) /
            static_cast<double>(summary.impairment_eligible_unreliable_message_count);
    }

    summary.minimum_client_authoritative_input_ratio = 1.0;
    summary.minimum_client_impairment_eligible_unreliable_message_count =
        std::numeric_limits<std::uint64_t>::max();
    summary.minimum_client_simulated_unreliable_drop_count =
        std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < summary.clients.size(); ++index) {
        auto& client = summary.clients[index];
        client.correction_distance_p95_m = percentile(client_correction_distances[index], 95);
        client.correction_distance_p99_m = percentile(client_correction_distances[index], 99);
        client.maximum_correction_distance_m =
            client_correction_distances[index].empty()
                ? 0.0
                : *std::ranges::max_element(client_correction_distances[index]);
        client.measured_authoritative_input_ratio =
            static_cast<double>(client.measured_final_authoritative_input_sequence) /
            static_cast<double>(config.measured_ticks);
        if (client.impairment_eligible_unreliable_message_count != 0) {
            client.observed_unreliable_loss_ratio =
                static_cast<double>(client.simulated_dropped_unreliable_message_count) /
                static_cast<double>(client.impairment_eligible_unreliable_message_count);
        }
        summary.minimum_client_authoritative_input_ratio =
            std::min(summary.minimum_client_authoritative_input_ratio,
                     client.measured_authoritative_input_ratio);
        summary.maximum_client_hard_correction_count =
            std::max(summary.maximum_client_hard_correction_count, client.hard_correction_count);
        summary.maximum_client_average_server_to_client_bytes_per_second =
            std::max(summary.maximum_client_average_server_to_client_bytes_per_second,
                     client.average_server_to_client_bytes_per_second);
        summary.maximum_client_rolling_one_second_server_to_client_bytes =
            std::max(summary.maximum_client_rolling_one_second_server_to_client_bytes,
                     client.peak_rolling_one_second_server_to_client_bytes);
        summary.minimum_client_impairment_eligible_unreliable_message_count =
            std::min(summary.minimum_client_impairment_eligible_unreliable_message_count,
                     client.impairment_eligible_unreliable_message_count);
        summary.minimum_client_simulated_unreliable_drop_count =
            std::min(summary.minimum_client_simulated_unreliable_drop_count,
                     client.simulated_dropped_unreliable_message_count);
        summary.maximum_client_pending_impaired_message_count =
            std::max(summary.maximum_client_pending_impaired_message_count,
                     client.peak_pending_impaired_message_count);
    }

    const auto* final_sample = !recovery_ticks.empty()
                                   ? &recovery_ticks.back()
                                   : (!raw_ticks.empty() ? &raw_ticks.back() : nullptr);
    if (final_sample != nullptr) {
        summary.final_pending_impaired_message_count = final_sample->pending_impaired_message_count;
        summary.final_pending_reliable_message_count = final_sample->pending_reliable_message_count;
        summary.final_pending_reliable_bytes = final_sample->pending_reliable_bytes;
        for (std::size_t index = 0; index < final_sample->clients.size(); ++index) {
            auto& client = summary.clients[index];
            client.final_pending_impaired_message_count =
                final_sample->clients[index].pending_impaired_message_count;
            client.final_authoritative_input_sequence =
                final_sample->clients[index].authoritative_input_sequence;
            client.connected = final_sample->clients[index].connected;
        }
    }
    refresh_terminal_summary(summary);
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
    maximum("maximum_recovery_server_tick_ms", summary.maximum_recovery_server_tick_ms,
            config.maximum_server_tick_ms);
    exclusive_minimum("accepted_input_ratio", summary.accepted_input_ratio,
                      config.minimum_accepted_input_ratio);
    exclusive_minimum("minimum_client_authoritative_input_ratio",
                      summary.minimum_client_authoritative_input_ratio,
                      config.minimum_client_authoritative_input_ratio);
    maximum("hard_correction_count", static_cast<double>(summary.hard_correction_count),
            static_cast<double>(config.maximum_hard_correction_count));
    maximum("maximum_client_hard_correction_count",
            static_cast<double>(summary.maximum_client_hard_correction_count),
            static_cast<double>(config.maximum_hard_correction_count_per_client));
    exclusive_maximum("maximum_correction_distance_m", summary.maximum_correction_distance_m,
                      config.maximum_correction_distance_m);
    maximum("maximum_final_state_error_m", summary.maximum_final_state_error_m,
            config.maximum_final_state_error_m);
    exclusive_maximum(
        "average_server_to_client_bytes_per_second",
        summary.average_server_to_client_bytes_per_second,
        static_cast<double>(config.maximum_average_server_to_client_bytes_per_second));
    exclusive_maximum(
        "peak_rolling_one_second_server_to_client_bytes",
        static_cast<double>(summary.peak_rolling_one_second_server_to_client_bytes),
        static_cast<double>(config.maximum_rolling_one_second_server_to_client_bytes));
    exclusive_maximum(
        "maximum_client_average_server_to_client_bytes_per_second",
        summary.maximum_client_average_server_to_client_bytes_per_second,
        static_cast<double>(config.maximum_average_server_to_client_bytes_per_second_per_client));
    exclusive_maximum(
        "maximum_client_rolling_one_second_server_to_client_bytes",
        static_cast<double>(summary.maximum_client_rolling_one_second_server_to_client_bytes),
        static_cast<double>(config.maximum_rolling_one_second_server_to_client_bytes_per_client));
    minimum("impairment_eligible_unreliable_message_count",
            static_cast<double>(summary.impairment_eligible_unreliable_message_count),
            static_cast<double>(config.minimum_impairment_eligible_unreliable_message_count));
    minimum("simulated_dropped_unreliable_message_count",
            static_cast<double>(summary.simulated_dropped_unreliable_message_count),
            static_cast<double>(config.minimum_simulated_unreliable_drop_count));
    maximum("peak_pending_impaired_message_count",
            static_cast<double>(summary.peak_pending_impaired_message_count),
            static_cast<double>(config.maximum_pending_impaired_message_count));
    exclusive_minimum("minimum_authoritative_displacement_m",
                      summary.minimum_authoritative_displacement_m,
                      config.minimum_authoritative_displacement_m);
    minimum("minimum_final_authoritative_input_sequence",
            static_cast<double>(summary.minimum_final_authoritative_input_sequence),
            static_cast<double>(config.measured_ticks));
    minimum("minimum_final_client_input_sequence",
            static_cast<double>(summary.minimum_final_client_input_sequence),
            static_cast<double>(config.measured_ticks));
    maximum("maximum_final_unacknowledged_prediction_input_count",
            static_cast<double>(summary.maximum_final_unacknowledged_prediction_input_count), 0.0);
    minimum("all_clients_connected", summary.all_clients_connected ? 1.0 : 0.0, 1.0);
    maximum("rejected_input_count", static_cast<double>(summary.rejected_input_count), 0.0);
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

    for (const auto& client : summary.clients) {
        const auto prefix = "client_" + std::to_string(client.client_id) + ".";
        exclusive_minimum(prefix + "measured_authoritative_input_ratio",
                          client.measured_authoritative_input_ratio,
                          config.minimum_client_authoritative_input_ratio);
        maximum(prefix + "hard_correction_count", static_cast<double>(client.hard_correction_count),
                static_cast<double>(config.maximum_hard_correction_count_per_client));
        exclusive_maximum(prefix + "average_server_to_client_bytes_per_second",
                          client.average_server_to_client_bytes_per_second,
                          static_cast<double>(
                              config.maximum_average_server_to_client_bytes_per_second_per_client));
        exclusive_maximum(
            prefix + "peak_rolling_one_second_server_to_client_bytes",
            static_cast<double>(client.peak_rolling_one_second_server_to_client_bytes),
            static_cast<double>(
                config.maximum_rolling_one_second_server_to_client_bytes_per_client));
        minimum(prefix + "impairment_eligible_unreliable_message_count",
                static_cast<double>(client.impairment_eligible_unreliable_message_count),
                static_cast<double>(
                    config.minimum_impairment_eligible_unreliable_message_count_per_client));
        minimum(prefix + "simulated_dropped_unreliable_message_count",
                static_cast<double>(client.simulated_dropped_unreliable_message_count),
                static_cast<double>(config.minimum_simulated_unreliable_drop_count_per_client));
        maximum(prefix + "peak_pending_impaired_message_count",
                static_cast<double>(client.peak_pending_impaired_message_count),
                static_cast<double>(config.maximum_pending_impaired_message_count_per_client));
        exclusive_minimum(prefix + "authoritative_displacement_m",
                          client.authoritative_displacement_m,
                          config.minimum_authoritative_displacement_m);
        maximum(prefix + "final_state_error_m", client.final_state_error_m,
                config.maximum_final_state_error_m);
    }
    gates.passed = gates.violations.empty();
    return gates;
}

[[nodiscard]] bool
client_summaries_equal(const MultiplayerNetworkImpairmentClientSummary& left,
                       const MultiplayerNetworkImpairmentClientSummary& right) noexcept {
    return left.client_id == right.client_id &&
           left.predicted_input_count == right.predicted_input_count &&
           left.reconciled_input_count == right.reconciled_input_count &&
           left.acknowledged_input_count == right.acknowledged_input_count &&
           left.hard_correction_count == right.hard_correction_count &&
           nearly_equal(left.correction_distance_p95_m, right.correction_distance_p95_m) &&
           nearly_equal(left.correction_distance_p99_m, right.correction_distance_p99_m) &&
           nearly_equal(left.maximum_correction_distance_m, right.maximum_correction_distance_m) &&
           left.client_to_server_wire_bytes == right.client_to_server_wire_bytes &&
           left.server_to_client_wire_bytes == right.server_to_client_wire_bytes &&
           nearly_equal(left.average_server_to_client_bytes_per_second,
                        right.average_server_to_client_bytes_per_second) &&
           left.peak_rolling_one_second_server_to_client_bytes ==
               right.peak_rolling_one_second_server_to_client_bytes &&
           left.client_to_server_message_count == right.client_to_server_message_count &&
           left.server_to_client_message_count == right.server_to_client_message_count &&
           left.impairment_eligible_unreliable_message_count ==
               right.impairment_eligible_unreliable_message_count &&
           left.simulated_dropped_unreliable_message_count ==
               right.simulated_dropped_unreliable_message_count &&
           nearly_equal(left.observed_unreliable_loss_ratio,
                        right.observed_unreliable_loss_ratio) &&
           left.peak_pending_impaired_message_count == right.peak_pending_impaired_message_count &&
           left.final_pending_impaired_message_count ==
               right.final_pending_impaired_message_count &&
           left.measured_final_authoritative_input_sequence ==
               right.measured_final_authoritative_input_sequence &&
           nearly_equal(left.measured_authoritative_input_ratio,
                        right.measured_authoritative_input_ratio) &&
           left.final_authoritative_input_sequence == right.final_authoritative_input_sequence &&
           left.final_client_input_sequence == right.final_client_input_sequence &&
           left.final_unacknowledged_prediction_input_count ==
               right.final_unacknowledged_prediction_input_count &&
           nearly_equal(left.authoritative_displacement_m, right.authoritative_displacement_m) &&
           nearly_equal(left.final_state_error_m, right.final_state_error_m) &&
           left.connected == right.connected;
}

[[nodiscard]] bool summaries_equal(const MultiplayerNetworkImpairmentBenchmarkSummary& left,
                                   const MultiplayerNetworkImpairmentBenchmarkSummary& right) {
    const auto scalars_equal =
        left.warmup_ticks == right.warmup_ticks && left.recovery_ticks == right.recovery_ticks &&
        left.measured_tick_count == right.measured_tick_count &&
        left.client_count == right.client_count &&
        nearly_equal(left.server_tick_p50_ms, right.server_tick_p50_ms) &&
        nearly_equal(left.server_tick_p95_ms, right.server_tick_p95_ms) &&
        nearly_equal(left.server_tick_p99_ms, right.server_tick_p99_ms) &&
        nearly_equal(left.maximum_server_tick_ms, right.maximum_server_tick_ms) &&
        nearly_equal(left.runtime_frame_p50_ms, right.runtime_frame_p50_ms) &&
        nearly_equal(left.runtime_frame_p95_ms, right.runtime_frame_p95_ms) &&
        nearly_equal(left.runtime_frame_p99_ms, right.runtime_frame_p99_ms) &&
        nearly_equal(left.maximum_runtime_frame_ms, right.maximum_runtime_frame_ms) &&
        left.accepted_input_count == right.accepted_input_count &&
        left.rejected_input_count == right.rejected_input_count &&
        left.repeated_input_count == right.repeated_input_count &&
        left.predicted_input_count == right.predicted_input_count &&
        left.reconciled_input_count == right.reconciled_input_count &&
        left.acknowledged_input_count == right.acknowledged_input_count &&
        nearly_equal(left.accepted_input_ratio, right.accepted_input_ratio) &&
        nearly_equal(left.minimum_client_authoritative_input_ratio,
                     right.minimum_client_authoritative_input_ratio) &&
        left.hard_correction_count == right.hard_correction_count &&
        left.maximum_client_hard_correction_count == right.maximum_client_hard_correction_count &&
        nearly_equal(left.correction_distance_p95_m, right.correction_distance_p95_m) &&
        nearly_equal(left.correction_distance_p99_m, right.correction_distance_p99_m) &&
        nearly_equal(left.maximum_correction_distance_m, right.maximum_correction_distance_m) &&
        left.client_to_server_wire_bytes == right.client_to_server_wire_bytes &&
        left.server_to_client_wire_bytes == right.server_to_client_wire_bytes &&
        nearly_equal(left.average_server_to_client_bytes_per_second,
                     right.average_server_to_client_bytes_per_second) &&
        left.peak_rolling_one_second_server_to_client_bytes ==
            right.peak_rolling_one_second_server_to_client_bytes &&
        nearly_equal(left.maximum_client_average_server_to_client_bytes_per_second,
                     right.maximum_client_average_server_to_client_bytes_per_second) &&
        left.maximum_client_rolling_one_second_server_to_client_bytes ==
            right.maximum_client_rolling_one_second_server_to_client_bytes &&
        left.client_to_server_message_count == right.client_to_server_message_count &&
        left.server_to_client_message_count == right.server_to_client_message_count &&
        left.impairment_eligible_unreliable_message_count ==
            right.impairment_eligible_unreliable_message_count &&
        left.simulated_dropped_unreliable_message_count ==
            right.simulated_dropped_unreliable_message_count &&
        nearly_equal(left.observed_unreliable_loss_ratio, right.observed_unreliable_loss_ratio) &&
        left.minimum_client_impairment_eligible_unreliable_message_count ==
            right.minimum_client_impairment_eligible_unreliable_message_count &&
        left.minimum_client_simulated_unreliable_drop_count ==
            right.minimum_client_simulated_unreliable_drop_count &&
        left.peak_pending_impaired_message_count == right.peak_pending_impaired_message_count &&
        left.final_pending_impaired_message_count == right.final_pending_impaired_message_count &&
        left.maximum_client_pending_impaired_message_count ==
            right.maximum_client_pending_impaired_message_count &&
        left.peak_pending_reliable_message_count == right.peak_pending_reliable_message_count &&
        left.final_pending_reliable_message_count == right.final_pending_reliable_message_count &&
        left.peak_pending_reliable_bytes == right.peak_pending_reliable_bytes &&
        left.final_pending_reliable_bytes == right.final_pending_reliable_bytes &&
        left.dropped_reliable_message_count == right.dropped_reliable_message_count &&
        left.malformed_datagram_count == right.malformed_datagram_count &&
        left.rejected_datagram_count == right.rejected_datagram_count &&
        left.rate_limited_datagram_count == right.rate_limited_datagram_count &&
        left.outbound_budget_dropped_unreliable_message_count ==
            right.outbound_budget_dropped_unreliable_message_count &&
        left.disconnected_client_count == right.disconnected_client_count &&
        nearly_equal(left.maximum_recovery_server_tick_ms, right.maximum_recovery_server_tick_ms) &&
        nearly_equal(left.minimum_authoritative_displacement_m,
                     right.minimum_authoritative_displacement_m) &&
        left.minimum_final_authoritative_input_sequence ==
            right.minimum_final_authoritative_input_sequence &&
        left.minimum_final_client_input_sequence == right.minimum_final_client_input_sequence &&
        left.maximum_final_unacknowledged_prediction_input_count ==
            right.maximum_final_unacknowledged_prediction_input_count &&
        nearly_equal(left.maximum_final_state_error_m, right.maximum_final_state_error_m) &&
        left.all_clients_connected == right.all_clients_connected &&
        left.clients.size() == right.clients.size();
    if (!scalars_equal) {
        return false;
    }
    for (std::size_t index = 0; index < left.clients.size(); ++index) {
        if (!client_summaries_equal(left.clients[index], right.clients[index])) {
            return false;
        }
    }
    return true;
}

void write_client_tick(std::ostream& output,
                       const MultiplayerNetworkImpairmentClientTickSample& client) {
    output << "{\"client_id\": " << client.client_id
           << ", \"submitted_input_sequence\": " << client.submitted_input_sequence
           << ", \"authoritative_input_sequence\": " << client.authoritative_input_sequence
           << ", \"predicted_input_count\": " << client.predicted_input_count
           << ", \"reconciled_input_count\": " << client.reconciled_input_count
           << ", \"acknowledged_input_count\": " << client.acknowledged_input_count
           << ", \"hard_correction_count\": " << client.hard_correction_count
           << ", \"maximum_correction_distance_m\": " << client.maximum_correction_distance_m
           << ", \"client_to_server_wire_bytes\": " << client.client_to_server_wire_bytes
           << ", \"server_to_client_wire_bytes\": " << client.server_to_client_wire_bytes
           << ", \"client_to_server_message_count\": " << client.client_to_server_message_count
           << ", \"server_to_client_message_count\": " << client.server_to_client_message_count
           << ", \"impairment_eligible_unreliable_message_count\": "
           << client.impairment_eligible_unreliable_message_count
           << ", \"simulated_dropped_unreliable_message_count\": "
           << client.simulated_dropped_unreliable_message_count
           << ", \"pending_impaired_message_count\": " << client.pending_impaired_message_count
           << ", \"connected\": " << (client.connected ? "true" : "false") << '}';
}

void write_tick(std::ostream& output, const MultiplayerNetworkImpairmentTickSample& sample) {
    output << "{\"ordinal\": " << sample.ordinal
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
           << ", \"connected_client_count\": " << sample.connected_client_count
           << ", \"clients\": [";
    for (std::size_t index = 0; index < sample.clients.size(); ++index) {
        write_client_tick(output, sample.clients[index]);
        if (index + 1 != sample.clients.size()) {
            output << ", ";
        }
    }
    output << "]}";
}

void write_tick_array(std::ostream& output, std::string_view name,
                      std::span<const MultiplayerNetworkImpairmentTickSample> samples,
                      bool trailing_comma) {
    output << "  \"" << name << "\": [";
    if (!samples.empty()) {
        output << '\n';
    }
    for (std::size_t index = 0; index < samples.size(); ++index) {
        output << "    ";
        write_tick(output, samples[index]);
        output << (index + 1 == samples.size() ? "\n" : ",\n");
    }
    output << "  ]" << (trailing_comma ? ",\n" : "\n");
}

void write_client_summary(std::ostream& output,
                          const MultiplayerNetworkImpairmentClientSummary& client) {
    output << "{\"client_id\": " << client.client_id
           << ", \"predicted_input_count\": " << client.predicted_input_count
           << ", \"reconciled_input_count\": " << client.reconciled_input_count
           << ", \"acknowledged_input_count\": " << client.acknowledged_input_count
           << ", \"hard_correction_count\": " << client.hard_correction_count
           << ", \"correction_distance_p95_m\": " << client.correction_distance_p95_m
           << ", \"correction_distance_p99_m\": " << client.correction_distance_p99_m
           << ", \"maximum_correction_distance_m\": " << client.maximum_correction_distance_m
           << ", \"client_to_server_wire_bytes\": " << client.client_to_server_wire_bytes
           << ", \"server_to_client_wire_bytes\": " << client.server_to_client_wire_bytes
           << ", \"average_server_to_client_bytes_per_second\": "
           << client.average_server_to_client_bytes_per_second
           << ", \"peak_rolling_one_second_server_to_client_bytes\": "
           << client.peak_rolling_one_second_server_to_client_bytes
           << ", \"client_to_server_message_count\": " << client.client_to_server_message_count
           << ", \"server_to_client_message_count\": " << client.server_to_client_message_count
           << ", \"impairment_eligible_unreliable_message_count\": "
           << client.impairment_eligible_unreliable_message_count
           << ", \"simulated_dropped_unreliable_message_count\": "
           << client.simulated_dropped_unreliable_message_count
           << ", \"observed_unreliable_loss_ratio\": " << client.observed_unreliable_loss_ratio
           << ", \"peak_pending_impaired_message_count\": "
           << client.peak_pending_impaired_message_count
           << ", \"final_pending_impaired_message_count\": "
           << client.final_pending_impaired_message_count
           << ", \"measured_final_authoritative_input_sequence\": "
           << client.measured_final_authoritative_input_sequence
           << ", \"measured_authoritative_input_ratio\": "
           << client.measured_authoritative_input_ratio
           << ", \"final_authoritative_input_sequence\": "
           << client.final_authoritative_input_sequence
           << ", \"final_client_input_sequence\": " << client.final_client_input_sequence
           << ", \"final_unacknowledged_prediction_input_count\": "
           << client.final_unacknowledged_prediction_input_count
           << ", \"authoritative_displacement_m\": " << client.authoritative_displacement_m
           << ", \"final_state_error_m\": " << client.final_state_error_m
           << ", \"connected\": " << (client.connected ? "true" : "false") << '}';
}

class MultiplayerNetworkImpairmentBenchmarkRunner final {
  public:
    MultiplayerNetworkImpairmentBenchmarkRunner(
        const MultiplayerNetworkImpairmentBenchmarkConfig& config,
        const content::ContentValidationReport& content_report)
        : config_(config), content_report_(content_report) {}

    [[nodiscard]] core::Result<MultiplayerNetworkImpairmentBenchmarkReport> run() {
        auto status = initialize();
        if (!status) {
            return failure(status);
        }
        status = run_measured_workload();
        if (!status) {
            return failure(status);
        }
        status = run_recovery();
        if (!status) {
            return failure(status);
        }

        report_.summary =
            summarize(config_, report_.raw_ticks, report_.recovery_ticks, warmup_ticks_);
        status = populate_terminal_summary();
        if (!status) {
            return failure(status);
        }
        report_.gates = evaluate_gates(config_, report_.summary);
        status = report_.validate();
        if (!status) {
            return failure(status);
        }
        status = runtime_.shutdown();
        if (!status) {
            return failure(status);
        }
        return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::success(
            std::move(report_));
    }

  private:
    [[nodiscard]] core::Result<MultiplayerNetworkImpairmentBenchmarkReport>
    failure(const core::Status& status) const {
        return core::Result<MultiplayerNetworkImpairmentBenchmarkReport>::failure(
            status.error().code, status.error().message);
    }

    [[nodiscard]] core::Status initialize() {
        report_.config = config_;
        report_.runtime = profiling::query_runtime_metadata();
        if (content_report_.has_errors()) {
            return core::Status::failure("multiplayer_network_impairment_benchmark.invalid_content",
                                         "benchmark content validation contains errors");
        }

        auto initialized = GameRuntime::initialize({}, content_report_);
        if (!initialized) {
            return core::Status::failure(initialized.error().code, initialized.error().message);
        }
        runtime_ = std::move(initialized).value();

        RuntimeConfiguration runtime_config;
        runtime_config.fixed_step = {benchmark_ticks_per_second, 4, 250'000};
        runtime_config.simulated_network_one_way_latency_ms = config_.simulated_one_way_latency_ms;
        runtime_config.simulated_network_jitter_ms = config_.simulated_delay_variation_ms;
        runtime_config.simulated_network_unreliable_loss_basis_points =
            config_.simulated_unreliable_loss_basis_points;
        runtime_config.simulated_network_seed = config_.seed;

        auto metadata = content::save_metadata_from_content_report(
            content_report_, "multiplayer-network-impairment-benchmark", config_.seed);
        if (!metadata) {
            return core::Status::failure(metadata.error().code, metadata.error().message);
        }
        SessionRequest request;
        request.metadata = std::move(metadata).value();
        request.scenario_id = "base:scenarios/homestead";
        auto status = runtime_.start_session(runtime_config, std::move(request));
        if (!status) {
            return status;
        }
        session_ = runtime_.session();
        if (session_ == nullptr || session_->server() == nullptr || session_->client() == nullptr) {
            return core::Status::failure(
                "multiplayer_network_impairment_benchmark.missing_runtime",
                "automated benchmark session did not create a server and client runtime");
        }
        server_ = session_->server();
        clients_.push_back(
            BenchmarkClient{session_->client()->client_id(), session_->client(), nullptr, {}, {}});

        for (std::uint32_t index = 1; index < config_.client_count; ++index) {
            auto connected = server_->connect_client();
            if (!connected) {
                return core::Status::failure(connected.error().code, connected.error().message);
            }
            world::WorldStateDesc world_desc;
            world_desc.metadata = server_->world().metadata();
            world_desc.voxel_palette = server_->world().voxel_palette_manifest();
            auto client = std::make_unique<ClientRuntime>(connected.value(), std::move(world_desc),
                                                          &server_->replication_registry(),
                                                          &server_->voxel_palette());
            auto* client_runtime = client.get();
            clients_.push_back(
                BenchmarkClient{connected.value(), client_runtime, std::move(client), {}, {}});
        }
        std::ranges::sort(clients_, {}, &BenchmarkClient::id);
        return warmup();
    }

    [[nodiscard]] core::Status warmup() {
        std::uint32_t stable_ticks = 0;
        while (warmup_ticks_ < config_.warmup_timeout_ticks) {
            now_ms_ += benchmark_frame_time_ms;
            auto tick = server_->run_tick(next_tick_, benchmark_fixed_delta_seconds, now_ms_);
            if (!tick) {
                return core::Status::failure(tick.error().code, tick.error().message);
            }
            const auto render_tick = next_tick_++;
            for (auto& client : clients_) {
                auto synchronized = drain_and_synchronize(client, render_tick);
                if (!synchronized) {
                    return core::Status::failure(synchronized.error().code,
                                                 synchronized.error().message);
                }
            }
            ++warmup_ticks_;

            const auto per_client_pending_bounded =
                tick.value().commands.transport_clients.size() == clients_.size() &&
                std::ranges::all_of(tick.value().commands.transport_clients,
                                    [](const auto& client) {
                                        return client.pending_impaired_message_count <=
                                               stable_warmup_pending_impaired_limit_per_client;
                                    });
            const auto all_clients_ready = std::ranges::all_of(clients_, [](const auto& client) {
                return client.runtime->is_connected() &&
                       client.runtime->local_player_snapshot() != nullptr &&
                       client.runtime->resource_counts().partial_chunk_snapshots == 0;
            });
            const auto& subscriptions = tick.value().chunk_subscriptions;
            const auto subscriptions_settled =
                subscriptions.converged_client_count == config_.client_count &&
                subscriptions.partial_snapshot_count == 0 &&
                subscriptions.stale_publication_count == 0 &&
                subscriptions.deferred_addition_count == 0 &&
                subscriptions.deferred_removal_count == 0 &&
                subscriptions.deferred_snapshot_count == 0 &&
                subscriptions.serialization_budget_deferred_snapshot_count == 0 &&
                subscriptions.reliable_admission_deferral_count == 0;
            const auto& chunk_loading = tick.value().chunk_loading;
            const auto chunk_loading_settled = chunk_loading.in_flight_requests == 0 &&
                                               chunk_loading.completed_mailbox_count == 0 &&
                                               chunk_loading.ready_for_publication_count == 0;
            const auto& chunk_streaming = tick.value().chunk_streaming;
            const auto chunk_streaming_settled =
                chunk_streaming.pending_load_count == 0 &&
                chunk_streaming.deferred_required_load_count == 0 &&
                chunk_streaming.projected_resident_overage == 0 &&
                chunk_streaming.unresolved_resident_overage == 0;
            const auto ready =
                all_clients_ready &&
                server_->host().connected_client_count() == config_.client_count &&
                server_->host().pending_outbound_message_count() == 0 && subscriptions_settled &&
                chunk_loading_settled && chunk_streaming_settled &&
                tick.value().commands.transport_pending_impaired_message_count <=
                    config_.client_count * stable_warmup_pending_impaired_limit_per_client &&
                per_client_pending_bounded;
            stable_ticks = ready ? stable_ticks + 1 : 0;
            if (warmup_ticks_ >= minimum_warmup_ticks(config_) &&
                stable_ticks >= required_stable_warmup_ticks(config_)) {
                break;
            }
        }
        if (warmup_ticks_ < minimum_warmup_ticks(config_) ||
            stable_ticks < required_stable_warmup_ticks(config_)) {
            return core::Status::failure(
                "multiplayer_network_impairment_benchmark.warmup_timeout",
                "all clients, subscriptions, reliable queues, and impaired paths must reach a "
                "stable bounded state before measurement");
        }
        for (auto& client : clients_) {
            const auto* authoritative = server_->player_for_client(client.id);
            if (authoritative == nullptr) {
                return core::Status::failure(
                    "multiplayer_network_impairment_benchmark.missing_initial_player",
                    "connected benchmark client has no authoritative player after warmup");
            }
            client.initial_authoritative_position = authoritative->state.position;
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<ClientRuntimeStats>
    drain_and_synchronize(BenchmarkClient& client, std::uint64_t render_tick) {
        auto messages = server_->drain_client_messages(client.id);
        if (!messages) {
            return core::Result<ClientRuntimeStats>::failure(messages.error().code,
                                                             messages.error().message);
        }
        auto status = client.runtime->receive(messages.value());
        if (!status) {
            return core::Result<ClientRuntimeStats>::failure(status.error().code,
                                                             status.error().message);
        }
        auto synchronized =
            client.runtime->synchronize(render_tick, std::numeric_limits<std::size_t>::max());
        if (!synchronized) {
            return core::Result<ClientRuntimeStats>::failure(synchronized.error().code,
                                                             synchronized.error().message);
        }
        return synchronized;
    }

    [[nodiscard]] core::Status submit_measured_inputs(std::uint32_t ordinal) {
        constexpr auto maximum_axis = std::numeric_limits<std::int16_t>::max();
        constexpr std::array<std::pair<std::int16_t, std::int16_t>, 8> directions{{
            {0, maximum_axis},
            {maximum_axis, maximum_axis},
            {maximum_axis, 0},
            {maximum_axis, static_cast<std::int16_t>(-maximum_axis)},
            {0, static_cast<std::int16_t>(-maximum_axis)},
            {static_cast<std::int16_t>(-maximum_axis), static_cast<std::int16_t>(-maximum_axis)},
            {static_cast<std::int16_t>(-maximum_axis), 0},
            {static_cast<std::int16_t>(-maximum_axis), maximum_axis},
        }};
        const auto active = ordinal <= config_.measured_ticks - config_.neutral_input_tail_ticks;
        for (std::size_t index = 0; index < clients_.size(); ++index) {
            auto& client = clients_[index];
            const auto* predicted = client.runtime->local_player_snapshot();
            if (predicted == nullptr) {
                return core::Status::failure(
                    "multiplayer_network_impairment_benchmark.missing_predicted_player",
                    "connected benchmark client lost its local prediction snapshot");
            }
            movement::PlayerInputFrame input;
            input.tick = ordinal;
            input.sequence = ordinal;
            if (predicted->state.simulation_tick >= input.tick) {
                if (predicted->state.simulation_tick == std::numeric_limits<std::uint64_t>::max()) {
                    return core::Status::failure(
                        "multiplayer_network_impairment_benchmark.movement_tick_exhausted",
                        "benchmark prediction exhausted its 64-bit tick space");
                }
                input.tick = predicted->state.simulation_tick + 1;
            }
            if (active) {
                input.move_x = directions[index % directions.size()].first;
                input.move_z = directions[index % directions.size()].second;
            }

            auto bundle = client.runtime->movement_input_bundle(input);
            if (!bundle) {
                return core::Status::failure(bundle.error().code, bundle.error().message);
            }
            auto status = server_->submit_movement_input(client.id, bundle.value(), now_ms_);
            if (!status) {
                return status;
            }
            status = client.runtime->predict_local_input(bundle.value().frames.back());
            if (!status) {
                return status;
            }
            client.recovery_bundle = bundle.value();
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Status retransmit_unacknowledged_inputs() {
        for (const auto& client : clients_) {
            const auto* authoritative = server_->player_for_client(client.id);
            if (authoritative == nullptr) {
                return core::Status::failure(
                    "multiplayer_network_impairment_benchmark.missing_recovery_player",
                    "benchmark lost an authoritative player during recovery");
            }
            if (authoritative->state.last_input_sequence >= config_.measured_ticks) {
                continue;
            }
            if (!client.recovery_bundle.has_value()) {
                return core::Status::failure(
                    "multiplayer_network_impairment_benchmark.missing_recovery_bundle",
                    "benchmark cannot recover an unacknowledged final input without retained "
                    "redundancy");
            }
            auto status =
                server_->submit_movement_input(client.id, *client.recovery_bundle, now_ms_);
            if (!status) {
                return status;
            }
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<MultiplayerNetworkImpairmentTickSample>
    run_tick(std::uint32_t ordinal, std::uint64_t submitted_input_sequence) {
        now_ms_ += benchmark_frame_time_ms;
        const auto frame_started = BenchmarkClock::now();
        auto tick = server_->run_tick(next_tick_, benchmark_fixed_delta_seconds, now_ms_);
        if (!tick) {
            return core::Result<MultiplayerNetworkImpairmentTickSample>::failure(
                tick.error().code, tick.error().message);
        }
        const auto render_tick = next_tick_++;
        if (tick.value().commands.transport_clients.size() != clients_.size()) {
            return core::Result<MultiplayerNetworkImpairmentTickSample>::failure(
                "multiplayer_network_impairment_benchmark.transport_client_mismatch",
                "transport telemetry must retain exactly one row for every benchmark client");
        }

        MultiplayerNetworkImpairmentTickSample sample;
        sample.ordinal = ordinal;
        sample.input_sequence = submitted_input_sequence;
        sample.authoritative_tick = tick.value().simulation.tick;
        sample.server_tick_time_us = tick.value().wall_time_us;
        sample.simulation_time_ms = tick.value().simulation.total_ms;
        sample.accepted_input_count = tick.value().accepted_movement_input_count;
        sample.rejected_input_count = tick.value().rejected_movement_input_count;
        sample.repeated_input_count = tick.value().repeated_input_count;
        sample.client_to_server_wire_bytes = tick.value().commands.transport_client_to_server_bytes;
        sample.server_to_client_wire_bytes = tick.value().commands.transport_server_to_client_bytes;
        sample.client_to_server_message_count =
            tick.value().commands.transport_client_to_server_message_count;
        sample.server_to_client_message_count =
            tick.value().commands.transport_server_to_client_message_count;
        sample.impairment_eligible_unreliable_message_count =
            tick.value().commands.transport_impairment_eligible_unreliable_message_count;
        sample.simulated_dropped_unreliable_message_count =
            tick.value().commands.transport_simulated_dropped_unreliable_message_count;
        sample.pending_impaired_message_count =
            tick.value().commands.transport_pending_impaired_message_count;
        sample.dropped_reliable_message_count =
            tick.value().commands.transport_dropped_reliable_message_count;
        sample.malformed_datagram_count = tick.value().commands.transport_malformed_datagram_count;
        sample.rejected_datagram_count = tick.value().commands.transport_rejected_datagram_count;
        sample.rate_limited_datagram_count =
            tick.value().commands.transport_rate_limited_datagram_count;
        sample.outbound_budget_dropped_unreliable_message_count =
            tick.value().commands.outbound_budget_dropped_unreliable_message_count;
        sample.disconnected_client_count = static_cast<std::uint32_t>(
            tick.value().commands.disconnected_clients.size() +
            tick.value().commands.outbound_delivery.overload_disconnected_clients.size());
        sample.connected_client_count =
            static_cast<std::uint32_t>(server_->host().connected_client_count());
        sample.clients.reserve(clients_.size());

        for (std::size_t index = 0; index < clients_.size(); ++index) {
            auto& client = clients_[index];
            const auto& transport = tick.value().commands.transport_clients[index];
            if (transport.client_id != client.id) {
                return core::Result<MultiplayerNetworkImpairmentTickSample>::failure(
                    "multiplayer_network_impairment_benchmark.transport_client_order",
                    "transport telemetry rows must use the benchmark's sorted client order");
            }
            auto synchronized = drain_and_synchronize(client, render_tick);
            if (!synchronized) {
                return core::Result<MultiplayerNetworkImpairmentTickSample>::failure(
                    synchronized.error().code, synchronized.error().message);
            }
            const auto* authoritative = server_->player_for_client(client.id);
            if (authoritative == nullptr) {
                return core::Result<MultiplayerNetworkImpairmentTickSample>::failure(
                    "multiplayer_network_impairment_benchmark.missing_authoritative_player",
                    "benchmark client lost its authoritative player record");
            }

            MultiplayerNetworkImpairmentClientTickSample client_sample;
            client_sample.client_id = client.id.value();
            client_sample.submitted_input_sequence = submitted_input_sequence;
            client_sample.authoritative_input_sequence = authoritative->state.last_input_sequence;
            client_sample.predicted_input_count = synchronized.value().predicted_input_count;
            client_sample.reconciled_input_count = synchronized.value().reconciled_input_count;
            client_sample.acknowledged_input_count = synchronized.value().acknowledged_input_count;
            client_sample.hard_correction_count = synchronized.value().hard_correction_count;
            client_sample.maximum_correction_distance_m =
                synchronized.value().maximum_correction_distance;
            client_sample.client_to_server_wire_bytes = transport.client_to_server_bytes;
            client_sample.server_to_client_wire_bytes = transport.server_to_client_bytes;
            client_sample.client_to_server_message_count = transport.client_to_server_message_count;
            client_sample.server_to_client_message_count = transport.server_to_client_message_count;
            client_sample.impairment_eligible_unreliable_message_count =
                transport.impairment_eligible_unreliable_message_count;
            client_sample.simulated_dropped_unreliable_message_count =
                transport.simulated_dropped_unreliable_message_count;
            client_sample.pending_impaired_message_count = transport.pending_impaired_message_count;
            client_sample.connected = client.runtime->is_connected();

            sample.predicted_input_count += client_sample.predicted_input_count;
            sample.reconciled_input_count += client_sample.reconciled_input_count;
            sample.acknowledged_input_count += client_sample.acknowledged_input_count;
            sample.hard_correction_count += client_sample.hard_correction_count;
            sample.maximum_correction_distance_m = std::max(
                sample.maximum_correction_distance_m, client_sample.maximum_correction_distance_m);
            sample.clients.push_back(client_sample);
        }
        sample.pending_reliable_message_count = server_->host().pending_outbound_message_count();
        sample.pending_reliable_bytes = server_->host().pending_outbound_bytes();
        sample.runtime_frame_time_us = elapsed_microseconds(frame_started);
        return core::Result<MultiplayerNetworkImpairmentTickSample>::success(std::move(sample));
    }

    [[nodiscard]] core::Status run_measured_workload() {
        report_.raw_ticks.reserve(config_.measured_ticks);
        for (std::uint32_t ordinal = 1; ordinal <= config_.measured_ticks; ++ordinal) {
            auto status = submit_measured_inputs(ordinal);
            if (!status) {
                return status;
            }
            auto sample = run_tick(ordinal, ordinal);
            if (!sample) {
                return core::Status::failure(sample.error().code, sample.error().message);
            }
            report_.raw_ticks.push_back(std::move(sample).value());
        }
        return core::Status::ok();
    }

    [[nodiscard]] bool recovered() const {
        if (server_->host().pending_outbound_message_count() != 0 ||
            server_->host().pending_outbound_bytes() != 0 ||
            server_->host().connected_client_count() != config_.client_count) {
            return false;
        }
        for (const auto& client : clients_) {
            const auto* authoritative = server_->player_for_client(client.id);
            const auto* snapshot = client.runtime->local_player_snapshot();
            if (authoritative == nullptr || snapshot == nullptr ||
                !client.runtime->is_connected() ||
                authoritative->state.last_input_sequence != config_.measured_ticks ||
                snapshot->last_processed_input_sequence != config_.measured_ticks ||
                client.runtime->resource_counts().unacknowledged_prediction_inputs != 0 ||
                position_distance(authoritative->state.position, snapshot->state.position) >
                    config_.maximum_final_state_error_m) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] core::Status run_recovery() {
        report_.recovery_ticks.reserve(config_.recovery_timeout_ticks);
        if (recovered()) {
            return core::Status::ok();
        }
        for (std::uint32_t ordinal = 1; ordinal <= config_.recovery_timeout_ticks; ++ordinal) {
            auto status = retransmit_unacknowledged_inputs();
            if (!status) {
                return status;
            }
            auto sample = run_tick(ordinal, config_.measured_ticks);
            if (!sample) {
                return core::Status::failure(sample.error().code, sample.error().message);
            }
            report_.recovery_ticks.push_back(std::move(sample).value());
            if (recovered()) {
                return core::Status::ok();
            }
        }
        return core::Status::failure(
            "multiplayer_network_impairment_benchmark.recovery_timeout",
            "clients did not acknowledge every measured input, empty prediction state, drain "
            "reliable queues, and converge to authoritative positions within the recovery bound");
    }

    [[nodiscard]] core::Status populate_terminal_summary() {
        if (report_.summary.clients.size() != clients_.size()) {
            return core::Status::failure(
                "multiplayer_network_impairment_benchmark.summary_client_mismatch",
                "benchmark summary must retain one terminal row for every client");
        }
        for (std::size_t index = 0; index < clients_.size(); ++index) {
            const auto& client = clients_[index];
            auto& summary = report_.summary.clients[index];
            const auto* authoritative = server_->player_for_client(client.id);
            const auto* snapshot = client.runtime->local_player_snapshot();
            if (authoritative == nullptr || snapshot == nullptr ||
                summary.client_id != client.id.value()) {
                return core::Status::failure(
                    "multiplayer_network_impairment_benchmark.missing_final_player",
                    "benchmark lost authoritative or replicated terminal player state");
            }
            summary.final_authoritative_input_sequence = authoritative->state.last_input_sequence;
            summary.final_client_input_sequence = snapshot->last_processed_input_sequence;
            summary.final_unacknowledged_prediction_input_count =
                client.runtime->resource_counts().unacknowledged_prediction_inputs;
            summary.authoritative_displacement_m = position_distance(
                client.initial_authoritative_position, authoritative->state.position);
            summary.final_state_error_m =
                position_distance(authoritative->state.position, snapshot->state.position);
            summary.connected = client.runtime->is_connected();
        }
        refresh_terminal_summary(report_.summary);
        return core::Status::ok();
    }

    const MultiplayerNetworkImpairmentBenchmarkConfig& config_;
    const content::ContentValidationReport& content_report_;
    MultiplayerNetworkImpairmentBenchmarkReport report_;
    GameRuntime runtime_;
    RuntimeSession* session_ = nullptr;
    ServerRuntime* server_ = nullptr;
    std::vector<BenchmarkClient> clients_;
    std::uint64_t next_tick_ = 1;
    std::int64_t now_ms_ = 0;
    std::uint32_t warmup_ticks_ = 0;
};

} // namespace

core::Status MultiplayerNetworkImpairmentBenchmarkConfig::validate() const {
    if (client_count < 2 || client_count > 64 || measured_ticks < 100 ||
        measured_ticks > 1'000'000 || neutral_input_tail_ticks == 0 ||
        neutral_input_tail_ticks >= measured_ticks || warmup_timeout_ticks == 0 ||
        warmup_timeout_ticks > 1'000'000 || recovery_timeout_ticks == 0 ||
        recovery_timeout_ticks > 1'000'000) {
        return core::Status::failure(
            "multiplayer_network_impairment_benchmark.invalid_workload",
            "benchmark requires 2-64 clients, 100-1,000,000 measured ticks, a nonempty neutral "
            "tail, and bounded nonzero warmup and recovery windows");
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
    if (warmup_timeout_ticks < minimum_warmup_ticks(*this)) {
        return core::Status::failure(
            "multiplayer_network_impairment_benchmark.invalid_workload",
            "warmup timeout must cover connection, assignment, subscription, and stable "
            "maximum-delay delivery windows");
    }
    if (!finite_positive(maximum_server_tick_p95_ms) ||
        !finite_positive(maximum_server_tick_p99_ms) || !finite_positive(maximum_server_tick_ms) ||
        maximum_server_tick_p95_ms > maximum_server_tick_p99_ms ||
        maximum_server_tick_p99_ms > maximum_server_tick_ms ||
        !finite_positive(minimum_accepted_input_ratio) || minimum_accepted_input_ratio > 1.0 ||
        !finite_positive(minimum_client_authoritative_input_ratio) ||
        minimum_client_authoritative_input_ratio > 1.0 ||
        maximum_hard_correction_count_per_client > maximum_hard_correction_count ||
        !finite_positive(maximum_correction_distance_m) ||
        !finite_positive(maximum_final_state_error_m) ||
        maximum_average_server_to_client_bytes_per_second == 0 ||
        maximum_rolling_one_second_server_to_client_bytes == 0 ||
        maximum_average_server_to_client_bytes_per_second_per_client == 0 ||
        maximum_rolling_one_second_server_to_client_bytes_per_client == 0 ||
        minimum_impairment_eligible_unreliable_message_count == 0 ||
        minimum_simulated_unreliable_drop_count == 0 ||
        minimum_impairment_eligible_unreliable_message_count_per_client == 0 ||
        minimum_simulated_unreliable_drop_count_per_client == 0 ||
        maximum_pending_impaired_message_count == 0 ||
        maximum_pending_impaired_message_count_per_client == 0 ||
        maximum_pending_impaired_message_count_per_client >
            maximum_pending_impaired_message_count ||
        !finite_positive(minimum_authoritative_displacement_m)) {
        return core::Status::failure(
            "multiplayer_network_impairment_benchmark.invalid_gates",
            "tick, input, correction, convergence, traffic, impairment, backlog, and displacement "
            "gates must be finite and internally ordered");
    }
    return core::Status::ok();
}

core::Status MultiplayerNetworkImpairmentBenchmarkReport::validate() const {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    if (raw_ticks.size() != config.measured_ticks ||
        recovery_ticks.size() > config.recovery_timeout_ticks ||
        summary.measured_tick_count != raw_ticks.size() ||
        summary.recovery_ticks != recovery_ticks.size() ||
        summary.client_count != config.client_count || summary.warmup_ticks == 0 ||
        summary.warmup_ticks > config.warmup_timeout_ticks) {
        return core::Status::failure(
            "multiplayer_network_impairment_benchmark.incomplete_report",
            "benchmark report is missing bounded warmup, measured, recovery, or client evidence");
    }

    std::uint64_t previous_authoritative_tick = 0;
    std::vector<std::uint64_t> client_ids;
    std::vector<std::uint64_t> previous_authoritative_sequences;
    const auto validate_samples =
        [&](std::span<const MultiplayerNetworkImpairmentTickSample> samples,
            bool recovery) -> core::Status {
        for (std::size_t index = 0; index < samples.size(); ++index) {
            const auto& sample = samples[index];
            const auto expected_input_sequence =
                recovery ? static_cast<std::uint64_t>(config.measured_ticks) : index + 1;
            if (sample.ordinal != index + 1 || sample.input_sequence != expected_input_sequence ||
                sample.authoritative_tick <= previous_authoritative_tick ||
                sample.runtime_frame_time_us == 0 || sample.server_tick_time_us == 0 ||
                !std::isfinite(sample.simulation_time_ms) || sample.simulation_time_ms < 0.0 ||
                !std::isfinite(sample.maximum_correction_distance_m) ||
                sample.maximum_correction_distance_m < 0.0 ||
                sample.clients.size() != config.client_count ||
                sample.simulated_dropped_unreliable_message_count >
                    sample.impairment_eligible_unreliable_message_count) {
                return core::Status::failure(
                    "multiplayer_network_impairment_benchmark.invalid_tick_sample",
                    "ticks must be ordered, timed, finite, complete, and retain a valid loss "
                    "denominator");
            }
            previous_authoritative_tick = sample.authoritative_tick;

            std::uint64_t predicted = 0;
            std::uint64_t reconciled = 0;
            std::uint64_t acknowledged = 0;
            std::uint64_t hard_corrections = 0;
            std::uint64_t client_to_server_bytes = 0;
            std::uint64_t server_to_client_bytes = 0;
            std::uint64_t client_to_server_messages = 0;
            std::uint64_t server_to_client_messages = 0;
            std::uint64_t eligible = 0;
            std::uint64_t dropped = 0;
            std::uint64_t pending = 0;
            std::uint32_t connected = 0;
            double maximum_correction = 0.0;
            for (std::size_t client_index = 0; client_index < sample.clients.size();
                 ++client_index) {
                const auto& client = sample.clients[client_index];
                if (client.client_id == 0 ||
                    (client_index != 0 &&
                     sample.clients[client_index - 1].client_id >= client.client_id) ||
                    client.submitted_input_sequence != expected_input_sequence ||
                    client.authoritative_input_sequence > client.submitted_input_sequence ||
                    !std::isfinite(client.maximum_correction_distance_m) ||
                    client.maximum_correction_distance_m < 0.0 ||
                    client.simulated_dropped_unreliable_message_count >
                        client.impairment_eligible_unreliable_message_count) {
                    return core::Status::failure(
                        "multiplayer_network_impairment_benchmark.invalid_client_tick_sample",
                        "per-client ticks must be sorted, finite, sequenced, and retain valid loss "
                        "evidence");
                }
                if (client_ids.empty()) {
                    client_ids.reserve(sample.clients.size());
                    previous_authoritative_sequences.assign(sample.clients.size(), 0);
                    for (const auto& initial : sample.clients) {
                        client_ids.push_back(initial.client_id);
                    }
                }
                if (client.client_id != client_ids[client_index] ||
                    client.authoritative_input_sequence <
                        previous_authoritative_sequences[client_index]) {
                    return core::Status::failure(
                        "multiplayer_network_impairment_benchmark.client_sequence_regression",
                        "client identity and authoritative input progress must remain stable and "
                        "monotonic");
                }
                previous_authoritative_sequences[client_index] =
                    client.authoritative_input_sequence;
                if ((!recovery && client.predicted_input_count != 1) ||
                    (recovery && client.predicted_input_count != 0)) {
                    return core::Status::failure(
                        "multiplayer_network_impairment_benchmark.prediction_workload_mismatch",
                        "measured ticks must predict one new input per client and recovery ticks "
                        "must not create new prediction input");
                }

                predicted += client.predicted_input_count;
                reconciled += client.reconciled_input_count;
                acknowledged += client.acknowledged_input_count;
                hard_corrections += client.hard_correction_count;
                client_to_server_bytes += client.client_to_server_wire_bytes;
                server_to_client_bytes += client.server_to_client_wire_bytes;
                client_to_server_messages += client.client_to_server_message_count;
                server_to_client_messages += client.server_to_client_message_count;
                eligible += client.impairment_eligible_unreliable_message_count;
                dropped += client.simulated_dropped_unreliable_message_count;
                pending += client.pending_impaired_message_count;
                connected += client.connected ? 1U : 0U;
                maximum_correction =
                    std::max(maximum_correction, client.maximum_correction_distance_m);
            }
            if (predicted != sample.predicted_input_count ||
                reconciled != sample.reconciled_input_count ||
                acknowledged != sample.acknowledged_input_count ||
                hard_corrections != sample.hard_correction_count ||
                client_to_server_bytes != sample.client_to_server_wire_bytes ||
                server_to_client_bytes != sample.server_to_client_wire_bytes ||
                client_to_server_messages != sample.client_to_server_message_count ||
                server_to_client_messages != sample.server_to_client_message_count ||
                eligible != sample.impairment_eligible_unreliable_message_count ||
                dropped != sample.simulated_dropped_unreliable_message_count ||
                pending != sample.pending_impaired_message_count ||
                connected != sample.connected_client_count ||
                !nearly_equal(maximum_correction, sample.maximum_correction_distance_m)) {
                return core::Status::failure(
                    "multiplayer_network_impairment_benchmark.client_aggregate_mismatch",
                    "every aggregate tick counter must exactly reconcile to its sorted per-client "
                    "rows");
            }
        }
        return core::Status::ok();
    };

    status = validate_samples(raw_ticks, false);
    if (status) {
        status = validate_samples(recovery_ticks, true);
    }
    if (!status) {
        return status;
    }

    auto expected = summarize(config, raw_ticks, recovery_ticks, summary.warmup_ticks);
    if (summary.clients.size() != expected.clients.size()) {
        return core::Status::failure(
            "multiplayer_network_impairment_benchmark.invalid_summary_clients",
            "benchmark summary must retain one sorted row per evidence client");
    }
    for (std::size_t index = 0; index < summary.clients.size(); ++index) {
        const auto& actual = summary.clients[index];
        auto& reproduced = expected.clients[index];
        if (actual.client_id != reproduced.client_id ||
            !std::isfinite(actual.authoritative_displacement_m) ||
            actual.authoritative_displacement_m < 0.0 ||
            !std::isfinite(actual.final_state_error_m) || actual.final_state_error_m < 0.0 ||
            actual.final_client_input_sequence > config.measured_ticks ||
            actual.final_unacknowledged_prediction_input_count > config.measured_ticks) {
            return core::Status::failure(
                "multiplayer_network_impairment_benchmark.invalid_terminal_client_summary",
                "terminal client state must be finite, bounded, and match retained client "
                "identity");
        }
        reproduced.final_client_input_sequence = actual.final_client_input_sequence;
        reproduced.final_unacknowledged_prediction_input_count =
            actual.final_unacknowledged_prediction_input_count;
        reproduced.authoritative_displacement_m = actual.authoritative_displacement_m;
        reproduced.final_state_error_m = actual.final_state_error_m;
    }
    refresh_terminal_summary(expected);
    if (!summaries_equal(expected, summary)) {
        return core::Status::failure(
            "multiplayer_network_impairment_benchmark.invalid_summary",
            "benchmark summary does not reproduce measured, recovery, per-client, and terminal "
            "evidence");
    }

    const auto expected_gates = evaluate_gates(config, summary);
    if (!gates.evaluated || gates.passed != expected_gates.passed ||
        gates.violations.size() != expected_gates.violations.size()) {
        return core::Status::failure(
            "multiplayer_network_impairment_benchmark.invalid_gate_evaluation",
            "benchmark gate state must reproduce the configured gate evaluation");
    }
    for (std::size_t index = 0; index < gates.violations.size(); ++index) {
        if (gates.violations[index].metric != expected_gates.violations[index].metric ||
            !nearly_equal(gates.violations[index].actual,
                          expected_gates.violations[index].actual) ||
            !nearly_equal(gates.violations[index].limit, expected_gates.violations[index].limit)) {
            return core::Status::failure(
                "multiplayer_network_impairment_benchmark.invalid_gate_violation",
                "retained gate violations must exactly match the configured evaluation");
        }
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
           << "    \"client_count\": " << config.client_count << ",\n"
           << "    \"measured_ticks\": " << config.measured_ticks << ",\n"
           << "    \"neutral_input_tail_ticks\": " << config.neutral_input_tail_ticks << ",\n"
           << "    \"warmup_timeout_ticks\": " << config.warmup_timeout_ticks << ",\n"
           << "    \"recovery_timeout_ticks\": " << config.recovery_timeout_ticks << ",\n"
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
           << "    \"minimum_client_authoritative_input_ratio\": "
           << config.minimum_client_authoritative_input_ratio << ",\n"
           << "    \"maximum_hard_correction_count\": " << config.maximum_hard_correction_count
           << ",\n"
           << "    \"maximum_hard_correction_count_per_client\": "
           << config.maximum_hard_correction_count_per_client << ",\n"
           << "    \"maximum_correction_distance_m\": " << config.maximum_correction_distance_m
           << ",\n"
           << "    \"maximum_final_state_error_m\": " << config.maximum_final_state_error_m << ",\n"
           << "    \"maximum_average_server_to_client_bytes_per_second\": "
           << config.maximum_average_server_to_client_bytes_per_second << ",\n"
           << "    \"maximum_rolling_one_second_server_to_client_bytes\": "
           << config.maximum_rolling_one_second_server_to_client_bytes << ",\n"
           << "    \"maximum_average_server_to_client_bytes_per_second_per_client\": "
           << config.maximum_average_server_to_client_bytes_per_second_per_client << ",\n"
           << "    \"maximum_rolling_one_second_server_to_client_bytes_per_client\": "
           << config.maximum_rolling_one_second_server_to_client_bytes_per_client << ",\n"
           << "    \"minimum_impairment_eligible_unreliable_message_count\": "
           << config.minimum_impairment_eligible_unreliable_message_count << ",\n"
           << "    \"minimum_simulated_unreliable_drop_count\": "
           << config.minimum_simulated_unreliable_drop_count << ",\n"
           << "    \"minimum_impairment_eligible_unreliable_message_count_per_client\": "
           << config.minimum_impairment_eligible_unreliable_message_count_per_client << ",\n"
           << "    \"minimum_simulated_unreliable_drop_count_per_client\": "
           << config.minimum_simulated_unreliable_drop_count_per_client << ",\n"
           << "    \"maximum_pending_impaired_message_count\": "
           << config.maximum_pending_impaired_message_count << ",\n"
           << "    \"maximum_pending_impaired_message_count_per_client\": "
           << config.maximum_pending_impaired_message_count_per_client << ",\n"
           << "    \"minimum_authoritative_displacement_m\": "
           << config.minimum_authoritative_displacement_m << "\n  },\n";

    output
        << "  \"summary\": {\n"
        << "    \"warmup_ticks\": " << summary.warmup_ticks << ",\n"
        << "    \"recovery_ticks\": " << summary.recovery_ticks << ",\n"
        << "    \"measured_tick_count\": " << summary.measured_tick_count << ",\n"
        << "    \"client_count\": " << summary.client_count << ",\n"
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
        << "    \"minimum_client_authoritative_input_ratio\": "
        << summary.minimum_client_authoritative_input_ratio << ",\n"
        << "    \"hard_correction_count\": " << summary.hard_correction_count << ",\n"
        << "    \"maximum_client_hard_correction_count\": "
        << summary.maximum_client_hard_correction_count << ",\n"
        << "    \"correction_distance_p95_m\": " << summary.correction_distance_p95_m << ",\n"
        << "    \"correction_distance_p99_m\": " << summary.correction_distance_p99_m << ",\n"
        << "    \"maximum_correction_distance_m\": " << summary.maximum_correction_distance_m
        << ",\n"
        << "    \"client_to_server_wire_bytes\": " << summary.client_to_server_wire_bytes << ",\n"
        << "    \"server_to_client_wire_bytes\": " << summary.server_to_client_wire_bytes << ",\n"
        << "    \"average_server_to_client_bytes_per_second\": "
        << summary.average_server_to_client_bytes_per_second << ",\n"
        << "    \"peak_rolling_one_second_server_to_client_bytes\": "
        << summary.peak_rolling_one_second_server_to_client_bytes << ",\n"
        << "    \"maximum_client_average_server_to_client_bytes_per_second\": "
        << summary.maximum_client_average_server_to_client_bytes_per_second << ",\n"
        << "    \"maximum_client_rolling_one_second_server_to_client_bytes\": "
        << summary.maximum_client_rolling_one_second_server_to_client_bytes << ",\n"
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
        << "    \"minimum_client_impairment_eligible_unreliable_message_count\": "
        << summary.minimum_client_impairment_eligible_unreliable_message_count << ",\n"
        << "    \"minimum_client_simulated_unreliable_drop_count\": "
        << summary.minimum_client_simulated_unreliable_drop_count << ",\n"
        << "    \"peak_pending_impaired_message_count\": "
        << summary.peak_pending_impaired_message_count << ",\n"
        << "    \"final_pending_impaired_message_count\": "
        << summary.final_pending_impaired_message_count << ",\n"
        << "    \"maximum_client_pending_impaired_message_count\": "
        << summary.maximum_client_pending_impaired_message_count << ",\n"
        << "    \"peak_pending_reliable_message_count\": "
        << summary.peak_pending_reliable_message_count << ",\n"
        << "    \"final_pending_reliable_message_count\": "
        << summary.final_pending_reliable_message_count << ",\n"
        << "    \"peak_pending_reliable_bytes\": " << summary.peak_pending_reliable_bytes << ",\n"
        << "    \"final_pending_reliable_bytes\": " << summary.final_pending_reliable_bytes << ",\n"
        << "    \"dropped_reliable_message_count\": " << summary.dropped_reliable_message_count
        << ",\n"
        << "    \"malformed_datagram_count\": " << summary.malformed_datagram_count << ",\n"
        << "    \"rejected_datagram_count\": " << summary.rejected_datagram_count << ",\n"
        << "    \"rate_limited_datagram_count\": " << summary.rate_limited_datagram_count << ",\n"
        << "    \"outbound_budget_dropped_unreliable_message_count\": "
        << summary.outbound_budget_dropped_unreliable_message_count << ",\n"
        << "    \"disconnected_client_count\": " << summary.disconnected_client_count << ",\n"
        << "    \"maximum_recovery_server_tick_ms\": " << summary.maximum_recovery_server_tick_ms
        << ",\n"
        << "    \"minimum_authoritative_displacement_m\": "
        << summary.minimum_authoritative_displacement_m << ",\n"
        << "    \"minimum_final_authoritative_input_sequence\": "
        << summary.minimum_final_authoritative_input_sequence << ",\n"
        << "    \"minimum_final_client_input_sequence\": "
        << summary.minimum_final_client_input_sequence << ",\n"
        << "    \"maximum_final_unacknowledged_prediction_input_count\": "
        << summary.maximum_final_unacknowledged_prediction_input_count << ",\n"
        << "    \"maximum_final_state_error_m\": " << summary.maximum_final_state_error_m << ",\n"
        << "    \"all_clients_connected\": " << (summary.all_clients_connected ? "true" : "false")
        << ",\n"
        << "    \"clients\": [";
    if (!summary.clients.empty()) {
        output << '\n';
    }
    for (std::size_t index = 0; index < summary.clients.size(); ++index) {
        output << "      ";
        write_client_summary(output, summary.clients[index]);
        output << (index + 1 == summary.clients.size() ? "\n" : ",\n");
    }
    output << "    ]\n  },\n";

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
    output << "    ]\n  },\n";
    write_tick_array(output, "raw_ticks", raw_ticks, true);
    write_tick_array(output, "recovery_ticks", recovery_ticks, false);
    output << "}\n";
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
    return MultiplayerNetworkImpairmentBenchmarkRunner(config, content_report).run();
}

} // namespace heartstead::game::benchmark
