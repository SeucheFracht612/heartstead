#include "engine/content/content_validation.hpp"
#include "game/runtime/multiplayer_network_impairment_benchmark.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <string>

namespace {

namespace benchmark = heartstead::game::benchmark;
namespace content = heartstead::content;

std::filesystem::path source_root() {
    return std::filesystem::path(HEARTSTEAD_TEST_SOURCE_DIR);
}

void test_impaired_runtime_retains_raw_loss_and_prediction_evidence(
    const content::ContentValidationReport& content_report) {
    benchmark::MultiplayerNetworkImpairmentBenchmarkConfig config;
    config.client_count = 2;
    config.measured_ticks = 120;
    config.neutral_input_tail_ticks = 12;
    config.simulated_unreliable_loss_basis_points = 500;
    config.maximum_server_tick_p95_ms = 10'000.0;
    config.maximum_server_tick_p99_ms = 10'000.0;
    config.maximum_server_tick_ms = 10'000.0;
    config.maximum_pending_impaired_message_count_per_client = 128;

    auto report = benchmark::run_multiplayer_network_impairment_benchmark(config, content_report);
    assert(report);
    assert(report.value().validate());
    assert(report.value().raw_ticks.size() == config.measured_ticks);
    assert(!report.value().recovery_ticks.empty());
    assert(report.value().summary.measured_tick_count == config.measured_ticks);
    assert(report.value().summary.client_count == config.client_count);
    assert(report.value().summary.clients.size() == config.client_count);
    assert(report.value().summary.recovery_ticks == report.value().recovery_ticks.size());
    assert(report.value().summary.impairment_eligible_unreliable_message_count > 0);
    assert(report.value().summary.simulated_dropped_unreliable_message_count > 0);
    assert(report.value().summary.simulated_dropped_unreliable_message_count <
           report.value().summary.impairment_eligible_unreliable_message_count);
    assert(report.value().summary.observed_unreliable_loss_ratio > 0.0);
    assert(report.value().summary.accepted_input_ratio > config.minimum_accepted_input_ratio);
    assert(report.value().summary.hard_correction_count <= config.maximum_hard_correction_count);
    assert(report.value().summary.maximum_correction_distance_m <
           config.maximum_correction_distance_m);
    assert(report.value().summary.final_pending_reliable_message_count == 0);
    assert(report.value().summary.minimum_final_authoritative_input_sequence ==
           config.measured_ticks);
    assert(report.value().summary.minimum_final_client_input_sequence == config.measured_ticks);
    assert(report.value().summary.maximum_final_unacknowledged_prediction_input_count == 0);
    assert(report.value().summary.maximum_final_state_error_m <=
           config.maximum_final_state_error_m);
    assert(report.value().summary.all_clients_connected);
    for (const auto& client : report.value().summary.clients) {
        assert(client.measured_authoritative_input_ratio >
               config.minimum_client_authoritative_input_ratio);
        assert(client.impairment_eligible_unreliable_message_count > 0);
        assert(client.simulated_dropped_unreliable_message_count > 0);
        assert(client.final_authoritative_input_sequence == config.measured_ticks);
        assert(client.final_client_input_sequence == config.measured_ticks);
        assert(client.final_unacknowledged_prediction_input_count == 0);
        assert(client.authoritative_displacement_m > config.minimum_authoritative_displacement_m);
        assert(client.final_state_error_m <= config.maximum_final_state_error_m);
        assert(client.connected);
    }
    for (const auto& tick : report.value().raw_ticks) {
        assert(tick.clients.size() == config.client_count);
        assert(tick.predicted_input_count == config.client_count);
        assert(tick.connected_client_count == config.client_count);
        assert(tick.clients.front().client_id < tick.clients.back().client_id);
        const auto client_to_server_bytes =
            std::accumulate(tick.clients.begin(), tick.clients.end(), std::uint64_t{0},
                            [](std::uint64_t total, const auto& client) {
                                return total + client.client_to_server_wire_bytes;
                            });
        const auto server_to_client_bytes =
            std::accumulate(tick.clients.begin(), tick.clients.end(), std::uint64_t{0},
                            [](std::uint64_t total, const auto& client) {
                                return total + client.server_to_client_wire_bytes;
                            });
        assert(client_to_server_bytes == tick.client_to_server_wire_bytes);
        assert(server_to_client_bytes == tick.server_to_client_wire_bytes);
    }
    assert(report.value().gates_passed());

    const auto json = report.value().to_json();
    assert(json.find("\"schema_version\": 2") != std::string::npos);
    assert(json.find("\"benchmark\": \"multiplayer_network_impairment\"") != std::string::npos);
    assert(json.find("\"simulated_round_trip_latency_ms\": 100") != std::string::npos);
    assert(json.find("\"client_count\": 2") != std::string::npos);
    assert(json.find("\"impairment_eligible_unreliable_message_count\"") != std::string::npos);
    assert(json.find("\"server_tick_p99_ms\"") != std::string::npos);
    assert(json.find("\"raw_ticks\"") != std::string::npos);
    assert(json.find("\"recovery_ticks\"") != std::string::npos);
}

void test_impairment_workload_rejects_ambiguous_or_total_loss() {
    benchmark::MultiplayerNetworkImpairmentBenchmarkConfig config;
    config.simulated_unreliable_loss_basis_points = 0;
    auto status = config.validate();
    assert(!status);
    assert(status.error().code == "multiplayer_network_impairment_benchmark.invalid_impairment");

    config.simulated_unreliable_loss_basis_points = 10'000;
    status = config.validate();
    assert(!status);
    assert(status.error().code == "multiplayer_network_impairment_benchmark.invalid_impairment");
}

} // namespace

int main() {
    const auto content_report = content::ContentValidation::validate(source_root());
    assert(!content_report.has_errors());
    test_impaired_runtime_retains_raw_loss_and_prediction_evidence(content_report);
    test_impairment_workload_rejects_ambiguous_or_total_loss();
    return 0;
}
