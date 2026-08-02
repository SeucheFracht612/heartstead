#include "engine/content/content_validation.hpp"
#include "game/runtime/multiplayer_chunk_subscription_benchmark.hpp"

#include <cassert>
#include <filesystem>
#include <string>

namespace {

namespace benchmark = heartstead::game::benchmark;
namespace content = heartstead::content;

std::filesystem::path source_root() {
    return std::filesystem::path(HEARTSTEAD_TEST_SOURCE_DIR);
}

void test_small_multiplayer_benchmark_retains_scaling_evidence(
    const content::ContentValidationReport& content_report) {
    benchmark::MultiplayerChunkSubscriptionBenchmarkConfig config;
    config.client_count = 2;
    config.traversal_steps = 1;
    config.hot_edit_ticks = 100;
    config.steady_ticks = 3;
    config.soak_conditioning_cycles = 1;
    config.soak_cycles = 4;
    config.maximum_server_tick_p95_ms = 10'000.0;
    config.maximum_server_tick_p99_ms = 10'000.0;
    config.maximum_server_tick_ms = 10'000.0;
    config.maximum_hot_edit_server_tick_p95_ms = 10'000.0;
    config.maximum_hot_edit_server_tick_p99_ms = 10'000.0;
    config.maximum_hot_edit_server_tick_ms = 10'000.0;
    config.maximum_snapshot_serialization_time_us_per_tick = 10'000'000;
    config.maximum_wire_bytes_per_client_per_tick = 10'000'000;
    config.maximum_hot_edit_wire_bytes_per_client_per_tick = 10'000'000;
    config.maximum_soak_private_memory_slope_bytes_per_cycle = 1'000'000'000.0;
    config.maximum_soak_private_memory_growth_bytes = 1'000'000'000;

    auto report = benchmark::run_multiplayer_chunk_subscription_benchmark(config, content_report);
    assert(report);
    assert(report.value().validate());
    assert(report.value().transitions.size() == config.traversal_steps + 3U);
    assert(report.value().summary.measured_tick_count == report.value().raw_ticks.size());
    assert(report.value().summary.shared_snapshot_reuse_ratio >=
           static_cast<double>(config.client_count));
    assert(report.value().summary.disjoint_snapshot_reuse_ratio == 1.0);
    assert(report.value().summary.observed_backlog_burst_count >= 1);
    assert(report.value().summary.maximum_backlog_recovery_ticks <=
           config.maximum_backlog_recovery_ticks);
    assert(report.value().summary.final_pending_reliable_messages == 0);
    assert(report.value().summary.final_converged_client_count == config.client_count);
    assert(report.value().summary.verified_cross_region_exclusions ==
           report.value().summary.expected_cross_region_exclusions);
    const auto expected_hot_edit_commands =
        static_cast<std::uint64_t>(config.hot_edit_ticks) * config.client_count;
    assert(report.value().summary.hot_edit_tick_count == config.hot_edit_ticks);
    assert(report.value().summary.hot_edit_command_count == expected_hot_edit_commands);
    assert(report.value().summary.hot_edit_command_result_message_count ==
           expected_hot_edit_commands);
    assert(report.value().summary.hot_edit_world_event_message_count == expected_hot_edit_commands);
    assert(report.value().summary.hot_edit_replication_delta_message_count ==
           expected_hot_edit_commands);
    assert(report.value().summary.hot_edit_delta_advanced_publication_count ==
           expected_hot_edit_commands);
    assert(report.value().summary.hot_edit_delta_avoided_snapshot_count ==
           expected_hot_edit_commands);
    assert(report.value().summary.hot_edit_delta_publication_gap_count == 0);
    assert(report.value().summary.hot_edit_applied_voxel_edit_count == expected_hot_edit_commands);
    assert(report.value().summary.verified_hot_edit_client_states ==
           report.value().summary.expected_hot_edit_client_states);
    assert(report.value().summary.verified_hot_edit_cross_region_exclusions ==
           report.value().summary.expected_hot_edit_cross_region_exclusions);
    assert(report.value().summary.soak_tick_count == report.value().soak_tick_times_us.size());
    assert(report.value().soak_samples.size() == config.soak_cycles + 1U);
    assert(report.value().summary.soak_edit_tick_count == config.soak_cycles * 2U);
    assert(report.value().summary.soak_verified_client_states ==
           report.value().summary.soak_expected_client_states);
    assert(report.value().summary.soak_verified_cross_region_exclusions ==
           report.value().summary.soak_expected_cross_region_exclusions);
    assert(report.value().summary.maximum_soak_settled_queue_depth == 0);
    assert(report.value().summary.soak_peak_server_world_chunk_count ==
           report.value().summary.soak_baseline_server_world_chunk_count);
    assert(report.value().summary.soak_peak_total_client_owned_record_count ==
           report.value().summary.soak_baseline_total_client_owned_record_count);
#if defined(__linux__)
    assert(report.value().summary.soak_process_memory_available);
#endif
    assert(report.value().gates_passed());

    const auto json = report.value().to_json();
    assert(json.find("\"schema_version\": 3") != std::string::npos);
    assert(json.find("\"benchmark\": \"multiplayer_chunk_subscriptions\"") != std::string::npos);
    assert(json.find("\"shared_snapshot_reuse_ratio\"") != std::string::npos);
    assert(json.find("\"verified_cross_region_exclusions\"") != std::string::npos);
    assert(json.find("\"hot_edit_server_tick_p99_ms\"") != std::string::npos);
    assert(json.find("\"verified_hot_edit_cross_region_exclusions\"") != std::string::npos);
    assert(json.find("\"soak_private_memory_slope_bytes_per_cycle\"") != std::string::npos);
    assert(json.find("\"soak_tick_times_us\"") != std::string::npos);
    assert(json.find("\"soak_samples\"") != std::string::npos);
    assert(json.find("\"raw_ticks\"") != std::string::npos);
}

void test_delivery_budget_must_force_a_bounded_snapshot_burst() {
    benchmark::MultiplayerChunkSubscriptionBenchmarkConfig config;
    config.reliable_delivery_messages_per_client_per_tick =
        heartstead::world::VoxelChunk::edge_length * 2U;
    const auto status = config.validate();
    assert(!status);
    assert(status.error().code ==
           "multiplayer_chunk_subscription_benchmark.invalid_delivery_budget");
}

} // namespace

int main() {
    const auto content_report = content::ContentValidation::validate(source_root());
    assert(!content_report.has_errors());
    test_small_multiplayer_benchmark_retains_scaling_evidence(content_report);
    test_delivery_budget_must_force_a_bounded_snapshot_burst();
    return 0;
}
