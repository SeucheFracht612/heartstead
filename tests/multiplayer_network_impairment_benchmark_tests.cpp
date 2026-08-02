#include "engine/content/content_validation.hpp"
#include "game/runtime/multiplayer_network_impairment_benchmark.hpp"

#include <cassert>
#include <filesystem>
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
    config.measured_ticks = 120;
    config.maximum_server_tick_p95_ms = 10'000.0;
    config.maximum_server_tick_p99_ms = 10'000.0;
    config.maximum_server_tick_ms = 10'000.0;

    auto report = benchmark::run_multiplayer_network_impairment_benchmark(config, content_report);
    assert(report);
    assert(report.value().validate());
    assert(report.value().raw_ticks.size() == config.measured_ticks);
    assert(report.value().summary.measured_tick_count == config.measured_ticks);
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
    assert(report.value().summary.client_connected);
    assert(report.value().gates_passed());

    const auto json = report.value().to_json();
    assert(json.find("\"schema_version\": 1") != std::string::npos);
    assert(json.find("\"benchmark\": \"multiplayer_network_impairment\"") != std::string::npos);
    assert(json.find("\"simulated_round_trip_latency_ms\": 100") != std::string::npos);
    assert(json.find("\"impairment_eligible_unreliable_message_count\"") != std::string::npos);
    assert(json.find("\"server_tick_p99_ms\"") != std::string::npos);
    assert(json.find("\"raw_ticks\"") != std::string::npos);
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
