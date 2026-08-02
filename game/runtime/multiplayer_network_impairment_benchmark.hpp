#pragma once

#include "engine/core/result.hpp"
#include "engine/profiling/runtime_metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace heartstead::content {
struct ContentValidationReport;
}

namespace heartstead::game::benchmark {

struct MultiplayerNetworkImpairmentBenchmarkConfig {
    std::uint64_t seed = 0x4e45545f494d5041ULL;
    std::uint32_t client_count = 8;
    std::uint32_t measured_ticks = 600;
    std::uint32_t neutral_input_tail_ticks = 30;
    std::uint32_t warmup_timeout_ticks = 120;
    std::uint32_t recovery_timeout_ticks = 32;
    std::uint32_t simulated_one_way_latency_ms = 50;
    std::uint32_t simulated_delay_variation_ms = 10;
    std::uint32_t simulated_unreliable_loss_basis_points = 200;

    bool enforce_gates = false;
    double maximum_server_tick_p95_ms = 12.5;
    double maximum_server_tick_p99_ms = 16.667;
    double maximum_server_tick_ms = 50.0;
    double minimum_accepted_input_ratio = 0.90;
    double minimum_client_authoritative_input_ratio = 0.90;
    std::uint32_t maximum_hard_correction_count = 8;
    std::uint32_t maximum_hard_correction_count_per_client = 1;
    double maximum_correction_distance_m = 1.0;
    double maximum_final_state_error_m = 0.10;
    std::uint64_t maximum_average_server_to_client_bytes_per_second = 1'536U * 1024U;
    std::uint64_t maximum_rolling_one_second_server_to_client_bytes = 1'536U * 1024U;
    std::uint64_t maximum_average_server_to_client_bytes_per_second_per_client = 192U * 1024U;
    std::uint64_t maximum_rolling_one_second_server_to_client_bytes_per_client = 192U * 1024U;
    std::uint32_t minimum_impairment_eligible_unreliable_message_count = 8;
    std::uint32_t minimum_simulated_unreliable_drop_count = 8;
    std::uint32_t minimum_impairment_eligible_unreliable_message_count_per_client = 1;
    std::uint32_t minimum_simulated_unreliable_drop_count_per_client = 1;
    std::uint32_t maximum_pending_impaired_message_count = 256;
    std::uint32_t maximum_pending_impaired_message_count_per_client = 48;
    double minimum_authoritative_displacement_m = 5.0;

    [[nodiscard]] core::Status validate() const;
};

struct MultiplayerNetworkImpairmentClientTickSample {
    std::uint64_t client_id = 0;
    std::uint64_t submitted_input_sequence = 0;
    std::uint64_t authoritative_input_sequence = 0;
    std::uint32_t predicted_input_count = 0;
    std::uint32_t reconciled_input_count = 0;
    std::uint32_t acknowledged_input_count = 0;
    std::uint32_t hard_correction_count = 0;
    double maximum_correction_distance_m = 0.0;
    std::uint64_t client_to_server_wire_bytes = 0;
    std::uint64_t server_to_client_wire_bytes = 0;
    std::uint32_t client_to_server_message_count = 0;
    std::uint32_t server_to_client_message_count = 0;
    std::uint32_t impairment_eligible_unreliable_message_count = 0;
    std::uint32_t simulated_dropped_unreliable_message_count = 0;
    std::uint32_t pending_impaired_message_count = 0;
    bool connected = false;
};

struct MultiplayerNetworkImpairmentTickSample {
    std::uint32_t ordinal = 0;
    std::uint64_t input_sequence = 0;
    std::uint64_t authoritative_tick = 0;
    std::uint64_t runtime_frame_time_us = 0;
    std::uint64_t server_tick_time_us = 0;
    double simulation_time_ms = 0.0;
    std::uint32_t accepted_input_count = 0;
    std::uint32_t rejected_input_count = 0;
    std::uint32_t repeated_input_count = 0;
    std::uint32_t predicted_input_count = 0;
    std::uint32_t reconciled_input_count = 0;
    std::uint32_t acknowledged_input_count = 0;
    std::uint32_t hard_correction_count = 0;
    double maximum_correction_distance_m = 0.0;
    std::uint64_t client_to_server_wire_bytes = 0;
    std::uint64_t server_to_client_wire_bytes = 0;
    std::uint32_t client_to_server_message_count = 0;
    std::uint32_t server_to_client_message_count = 0;
    std::uint32_t impairment_eligible_unreliable_message_count = 0;
    std::uint32_t simulated_dropped_unreliable_message_count = 0;
    std::uint32_t pending_impaired_message_count = 0;
    std::size_t pending_reliable_message_count = 0;
    std::uint64_t pending_reliable_bytes = 0;
    std::uint32_t dropped_reliable_message_count = 0;
    std::uint32_t malformed_datagram_count = 0;
    std::uint32_t rejected_datagram_count = 0;
    std::uint32_t rate_limited_datagram_count = 0;
    std::uint32_t outbound_budget_dropped_unreliable_message_count = 0;
    std::uint32_t disconnected_client_count = 0;
    std::uint32_t connected_client_count = 0;
    std::vector<MultiplayerNetworkImpairmentClientTickSample> clients;
};

struct MultiplayerNetworkImpairmentClientSummary {
    std::uint64_t client_id = 0;
    std::uint64_t predicted_input_count = 0;
    std::uint64_t reconciled_input_count = 0;
    std::uint64_t acknowledged_input_count = 0;
    std::uint64_t hard_correction_count = 0;
    double correction_distance_p95_m = 0.0;
    double correction_distance_p99_m = 0.0;
    double maximum_correction_distance_m = 0.0;
    std::uint64_t client_to_server_wire_bytes = 0;
    std::uint64_t server_to_client_wire_bytes = 0;
    double average_server_to_client_bytes_per_second = 0.0;
    std::uint64_t peak_rolling_one_second_server_to_client_bytes = 0;
    std::uint64_t client_to_server_message_count = 0;
    std::uint64_t server_to_client_message_count = 0;
    std::uint64_t impairment_eligible_unreliable_message_count = 0;
    std::uint64_t simulated_dropped_unreliable_message_count = 0;
    double observed_unreliable_loss_ratio = 0.0;
    std::uint32_t peak_pending_impaired_message_count = 0;
    std::uint32_t final_pending_impaired_message_count = 0;
    std::uint64_t measured_final_authoritative_input_sequence = 0;
    double measured_authoritative_input_ratio = 0.0;
    std::uint64_t final_authoritative_input_sequence = 0;
    std::uint64_t final_client_input_sequence = 0;
    std::size_t final_unacknowledged_prediction_input_count = 0;
    double authoritative_displacement_m = 0.0;
    double final_state_error_m = 0.0;
    bool connected = false;
};

struct MultiplayerNetworkImpairmentBenchmarkSummary {
    std::uint32_t warmup_ticks = 0;
    std::uint32_t recovery_ticks = 0;
    std::size_t measured_tick_count = 0;
    std::uint32_t client_count = 0;
    double server_tick_p50_ms = 0.0;
    double server_tick_p95_ms = 0.0;
    double server_tick_p99_ms = 0.0;
    double maximum_server_tick_ms = 0.0;
    double runtime_frame_p50_ms = 0.0;
    double runtime_frame_p95_ms = 0.0;
    double runtime_frame_p99_ms = 0.0;
    double maximum_runtime_frame_ms = 0.0;
    std::uint64_t accepted_input_count = 0;
    std::uint64_t rejected_input_count = 0;
    std::uint64_t repeated_input_count = 0;
    std::uint64_t predicted_input_count = 0;
    std::uint64_t reconciled_input_count = 0;
    std::uint64_t acknowledged_input_count = 0;
    double accepted_input_ratio = 0.0;
    double minimum_client_authoritative_input_ratio = 0.0;
    std::uint64_t hard_correction_count = 0;
    std::uint64_t maximum_client_hard_correction_count = 0;
    double correction_distance_p95_m = 0.0;
    double correction_distance_p99_m = 0.0;
    double maximum_correction_distance_m = 0.0;
    std::uint64_t client_to_server_wire_bytes = 0;
    std::uint64_t server_to_client_wire_bytes = 0;
    double average_server_to_client_bytes_per_second = 0.0;
    std::uint64_t peak_rolling_one_second_server_to_client_bytes = 0;
    double maximum_client_average_server_to_client_bytes_per_second = 0.0;
    std::uint64_t maximum_client_rolling_one_second_server_to_client_bytes = 0;
    std::uint64_t client_to_server_message_count = 0;
    std::uint64_t server_to_client_message_count = 0;
    std::uint64_t impairment_eligible_unreliable_message_count = 0;
    std::uint64_t simulated_dropped_unreliable_message_count = 0;
    double observed_unreliable_loss_ratio = 0.0;
    std::uint64_t minimum_client_impairment_eligible_unreliable_message_count = 0;
    std::uint64_t minimum_client_simulated_unreliable_drop_count = 0;
    std::uint32_t peak_pending_impaired_message_count = 0;
    std::uint32_t final_pending_impaired_message_count = 0;
    std::uint32_t maximum_client_pending_impaired_message_count = 0;
    std::size_t peak_pending_reliable_message_count = 0;
    std::size_t final_pending_reliable_message_count = 0;
    std::uint64_t peak_pending_reliable_bytes = 0;
    std::uint64_t final_pending_reliable_bytes = 0;
    std::uint64_t dropped_reliable_message_count = 0;
    std::uint64_t malformed_datagram_count = 0;
    std::uint64_t rejected_datagram_count = 0;
    std::uint64_t rate_limited_datagram_count = 0;
    std::uint64_t outbound_budget_dropped_unreliable_message_count = 0;
    std::uint64_t disconnected_client_count = 0;
    double maximum_recovery_server_tick_ms = 0.0;
    double minimum_authoritative_displacement_m = 0.0;
    std::uint64_t minimum_final_authoritative_input_sequence = 0;
    std::uint64_t minimum_final_client_input_sequence = 0;
    std::size_t maximum_final_unacknowledged_prediction_input_count = 0;
    double maximum_final_state_error_m = 0.0;
    bool all_clients_connected = false;
    std::vector<MultiplayerNetworkImpairmentClientSummary> clients;
};

struct MultiplayerNetworkImpairmentBenchmarkViolation {
    std::string metric;
    double actual = 0.0;
    double limit = 0.0;
};

struct MultiplayerNetworkImpairmentBenchmarkGateEvaluation {
    bool evaluated = false;
    bool passed = true;
    std::vector<MultiplayerNetworkImpairmentBenchmarkViolation> violations;
};

struct MultiplayerNetworkImpairmentBenchmarkReport {
    static constexpr std::uint32_t schema_version = 2;

    MultiplayerNetworkImpairmentBenchmarkConfig config;
    profiling::RuntimeMetadata runtime;
    MultiplayerNetworkImpairmentBenchmarkSummary summary;
    MultiplayerNetworkImpairmentBenchmarkGateEvaluation gates;
    std::vector<MultiplayerNetworkImpairmentTickSample> raw_ticks;
    std::vector<MultiplayerNetworkImpairmentTickSample> recovery_ticks;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] bool gates_passed() const noexcept;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] core::Status write_json(const std::filesystem::path& path) const;
};

[[nodiscard]] core::Result<MultiplayerNetworkImpairmentBenchmarkReport>
run_multiplayer_network_impairment_benchmark(
    const MultiplayerNetworkImpairmentBenchmarkConfig& config,
    const content::ContentValidationReport& content_report);

} // namespace heartstead::game::benchmark
