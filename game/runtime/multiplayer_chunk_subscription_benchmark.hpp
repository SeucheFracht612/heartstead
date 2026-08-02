#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/profiling/runtime_metadata.hpp"
#include "engine/world/chunks/chunk_subscription.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::content {
struct ContentValidationReport;
}

namespace heartstead::game::benchmark {

enum class MultiplayerChunkSubscriptionPhase : std::uint8_t {
    cluster_transition,
    spread_transition,
    traversal_transition,
    steady_state,
};

[[nodiscard]] std::string_view
multiplayer_chunk_subscription_phase_name(MultiplayerChunkSubscriptionPhase phase) noexcept;

struct MultiplayerChunkSubscriptionBenchmarkConfig {
    std::uint64_t seed = 0x4d554c5449434855ULL;
    std::uint32_t client_count = 8;
    std::uint32_t traversal_steps = 6;
    std::uint32_t steady_ticks = 24;
    std::uint32_t warmup_timeout_ticks = 4'096;
    std::uint32_t transition_timeout_ticks = 32;
    std::int64_t spread_distance_chunks = 128;
    std::int64_t traversal_stride_chunks = 4;
    std::uint32_t reliable_delivery_messages_per_client_per_tick = 48;
    world::ChunkSubscriptionPolicy subscriptions;

    bool enforce_gates = false;
    double maximum_server_tick_p95_ms = 12.5;
    double maximum_server_tick_p99_ms = 16.667;
    double maximum_server_tick_ms = 50.0;
    std::uint32_t maximum_transition_convergence_ticks = 16;
    std::uint32_t maximum_backlog_recovery_ticks = 2;
    double minimum_shared_snapshot_reuse_ratio = 2.0;
    double maximum_disjoint_snapshot_reuse_ratio = 1.05;
    std::uint64_t maximum_snapshot_serialization_time_us_per_tick = 4'000;
    std::uint64_t maximum_snapshot_serialization_time_overshoot_us = 1'000;
    std::uint64_t maximum_wire_bytes_per_client_per_tick = 320u * 1024u;

    [[nodiscard]] core::Status validate() const;
};

struct MultiplayerChunkClientTickTraffic {
    core::NetId client_id;
    std::uint32_t reliable_messages = 0;
    std::uint32_t unreliable_messages = 0;
    std::uint32_t chunk_snapshot_slice_messages = 0;
    std::uint32_t chunk_removal_messages = 0;
    std::uint64_t reliable_wire_bytes = 0;
    std::uint64_t unreliable_wire_bytes = 0;
    std::uint64_t chunk_snapshot_wire_bytes = 0;
    std::uint64_t chunk_removal_wire_bytes = 0;
    std::uint32_t completed_chunk_snapshots = 0;
    std::uint32_t applied_chunk_removals = 0;
};

struct MultiplayerChunkSubscriptionTickSample {
    MultiplayerChunkSubscriptionPhase phase = MultiplayerChunkSubscriptionPhase::cluster_transition;
    std::uint32_t phase_ordinal = 0;
    std::uint64_t tick = 0;
    std::uint64_t server_tick_time_us = 0;
    std::uint32_t connected_client_count = 0;
    std::size_t subscription_count = 0;
    std::size_t maximum_client_subscription_count = 0;
    std::uint32_t converged_client_count = 0;
    std::uint32_t added_subscription_count = 0;
    std::uint32_t removed_subscription_count = 0;
    std::uint32_t removal_message_count = 0;
    std::size_t partial_snapshot_count = 0;
    std::size_t stale_publication_count = 0;
    std::uint64_t deferred_addition_count = 0;
    std::uint64_t deferred_removal_count = 0;
    std::uint64_t deferred_snapshot_count = 0;
    std::uint32_t snapshot_chunk_count = 0;
    std::uint32_t snapshot_slice_message_count = 0;
    std::uint32_t snapshot_serialization_operation_count = 0;
    std::uint64_t snapshot_payload_bytes = 0;
    std::uint64_t snapshot_serialization_time_us = 0;
    std::uint64_t snapshot_serialization_time_overshoot_us = 0;
    std::uint64_t serialization_budget_deferred_snapshot_count = 0;
    std::uint32_t reliable_admission_deferral_count = 0;
    std::size_t pending_reliable_message_count = 0;
    std::uint64_t pending_reliable_bytes = 0;
    std::uint32_t disconnected_client_count = 0;
    std::vector<MultiplayerChunkClientTickTraffic> clients;
};

struct MultiplayerChunkSubscriptionTransitionSample {
    MultiplayerChunkSubscriptionPhase phase = MultiplayerChunkSubscriptionPhase::cluster_transition;
    std::uint32_t ordinal = 0;
    std::uint32_t ticks_to_converge = 0;
    std::size_t peak_pending_reliable_messages = 0;
    bool converged = false;
};

struct MultiplayerChunkClientTrafficSummary {
    core::NetId client_id;
    std::uint64_t reliable_messages = 0;
    std::uint64_t unreliable_messages = 0;
    std::uint64_t chunk_snapshot_slice_messages = 0;
    std::uint64_t chunk_removal_messages = 0;
    std::uint64_t reliable_wire_bytes = 0;
    std::uint64_t unreliable_wire_bytes = 0;
    std::uint64_t chunk_snapshot_wire_bytes = 0;
    std::uint64_t chunk_removal_wire_bytes = 0;
    std::uint64_t maximum_wire_bytes_per_tick = 0;
};

struct MultiplayerChunkSubscriptionBenchmarkSummary {
    std::uint64_t measured_tick_count = 0;
    double server_tick_p50_ms = 0.0;
    double server_tick_p95_ms = 0.0;
    double server_tick_p99_ms = 0.0;
    double maximum_server_tick_ms = 0.0;
    std::uint32_t maximum_transition_convergence_ticks = 0;
    std::uint32_t observed_backlog_burst_count = 0;
    std::uint32_t maximum_backlog_recovery_ticks = 0;
    std::size_t peak_pending_reliable_messages = 0;
    std::uint64_t peak_pending_reliable_bytes = 0;
    std::size_t maximum_client_subscription_count = 0;
    std::uint32_t maximum_added_subscriptions_per_tick = 0;
    std::uint32_t maximum_removed_subscriptions_per_tick = 0;
    std::size_t maximum_partial_snapshot_count = 0;
    std::size_t maximum_stale_publication_count = 0;
    std::uint64_t maximum_deferred_snapshot_count = 0;
    std::uint64_t maximum_snapshot_serialization_time_us = 0;
    std::uint64_t maximum_snapshot_serialization_time_overshoot_us = 0;
    std::uint64_t serialization_budget_deferred_snapshot_count = 0;
    std::uint32_t cluster_snapshot_chunk_count = 0;
    std::uint32_t cluster_snapshot_serialization_operation_count = 0;
    double shared_snapshot_reuse_ratio = 0.0;
    std::uint32_t spread_snapshot_chunk_count = 0;
    std::uint32_t spread_snapshot_serialization_operation_count = 0;
    double disjoint_snapshot_reuse_ratio = 0.0;
    std::uint64_t reliable_wire_bytes = 0;
    std::uint64_t unreliable_wire_bytes = 0;
    std::uint64_t maximum_wire_bytes_per_client_per_tick = 0;
    std::uint64_t verified_cross_region_exclusions = 0;
    std::uint64_t expected_cross_region_exclusions = 0;
    std::size_t final_pending_reliable_messages = 0;
    std::uint64_t final_pending_reliable_bytes = 0;
    std::uint32_t final_converged_client_count = 0;
    std::uint32_t disconnected_client_count = 0;
    std::vector<MultiplayerChunkClientTrafficSummary> clients;
};

struct MultiplayerChunkSubscriptionBenchmarkViolation {
    std::string metric;
    double actual = 0.0;
    double limit = 0.0;
};

struct MultiplayerChunkSubscriptionBenchmarkGateEvaluation {
    bool evaluated = false;
    bool passed = true;
    std::vector<MultiplayerChunkSubscriptionBenchmarkViolation> violations;
};

struct MultiplayerChunkSubscriptionBenchmarkReport {
    static constexpr std::uint32_t schema_version = 1;

    MultiplayerChunkSubscriptionBenchmarkConfig config;
    profiling::RuntimeMetadata runtime;
    MultiplayerChunkSubscriptionBenchmarkSummary summary;
    std::vector<MultiplayerChunkSubscriptionTransitionSample> transitions;
    std::vector<MultiplayerChunkSubscriptionTickSample> raw_ticks;
    MultiplayerChunkSubscriptionBenchmarkGateEvaluation gates;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] bool gates_passed() const noexcept;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] core::Status write_json(const std::filesystem::path& path) const;
};

[[nodiscard]] core::Result<MultiplayerChunkSubscriptionBenchmarkReport>
run_multiplayer_chunk_subscription_benchmark(
    const MultiplayerChunkSubscriptionBenchmarkConfig& config,
    const content::ContentValidationReport& content);

} // namespace heartstead::game::benchmark
