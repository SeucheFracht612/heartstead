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
    config.steady_ticks = 3;
    config.maximum_server_tick_p95_ms = 10'000.0;
    config.maximum_server_tick_p99_ms = 10'000.0;
    config.maximum_server_tick_ms = 10'000.0;
    config.maximum_snapshot_serialization_time_us_per_tick = 10'000'000;
    config.maximum_wire_bytes_per_client_per_tick = 10'000'000;

    auto report = benchmark::run_multiplayer_chunk_subscription_benchmark(config, content_report);
    assert(report);
    assert(report.value().validate());
    assert(report.value().transitions.size() == config.traversal_steps + 2U);
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
    assert(report.value().gates_passed());

    const auto json = report.value().to_json();
    assert(json.find("\"schema_version\": 1") != std::string::npos);
    assert(json.find("\"benchmark\": \"multiplayer_chunk_subscriptions\"") != std::string::npos);
    assert(json.find("\"shared_snapshot_reuse_ratio\"") != std::string::npos);
    assert(json.find("\"verified_cross_region_exclusions\"") != std::string::npos);
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
