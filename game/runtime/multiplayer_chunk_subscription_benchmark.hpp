#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/profiling/runtime_metadata.hpp"
#include "engine/world/chunks/chunk_subscription.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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
    hot_edit_transition,
    hot_edit,
    steady_state,
    soak_transition,
    soak_edit,
};

[[nodiscard]] std::string_view
multiplayer_chunk_subscription_phase_name(MultiplayerChunkSubscriptionPhase phase) noexcept;

struct MultiplayerChunkSubscriptionBenchmarkConfig {
    std::uint64_t seed = 0x4d554c5449434855ULL;
    std::uint32_t client_count = 8;
    std::uint32_t traversal_steps = 6;
    std::uint32_t hot_edit_ticks = 120;
    std::uint32_t steady_ticks = 24;
    std::uint32_t soak_conditioning_edit_ticks = 256;
    std::uint32_t soak_conditioning_cycles = 8;
    std::uint32_t soak_cycles = 64;
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
    double maximum_hot_edit_server_tick_p95_ms = 12.5;
    double maximum_hot_edit_server_tick_p99_ms = 16.667;
    double maximum_hot_edit_server_tick_ms = 50.0;
    std::uint32_t maximum_transition_convergence_ticks = 16;
    std::uint32_t maximum_backlog_recovery_ticks = 2;
    double minimum_shared_snapshot_reuse_ratio = 2.0;
    double maximum_disjoint_snapshot_reuse_ratio = 1.05;
    std::uint64_t maximum_snapshot_serialization_time_us_per_tick = 4'000;
    std::uint64_t maximum_snapshot_serialization_time_overshoot_us = 1'000;
    std::uint64_t maximum_wire_bytes_per_client_per_tick = 320u * 1024u;
    std::uint64_t maximum_hot_edit_wire_bytes_per_client_per_tick = 2u * 1024u;
    double maximum_soak_private_memory_slope_bytes_per_cycle = 64.0 * 1024.0;
    std::uint64_t maximum_soak_private_memory_growth_bytes = 8u * 1024u * 1024u;
    bool require_precise_process_memory = false;

    [[nodiscard]] core::Status validate() const;
};

struct MultiplayerChunkClientTickTraffic {
    core::NetId client_id;
    std::uint32_t reliable_messages = 0;
    std::uint32_t unreliable_messages = 0;
    std::uint32_t chunk_snapshot_slice_messages = 0;
    std::uint32_t chunk_removal_messages = 0;
    std::uint32_t command_result_messages = 0;
    std::uint32_t world_event_messages = 0;
    std::uint32_t world_delta_messages = 0;
    std::uint64_t reliable_wire_bytes = 0;
    std::uint64_t unreliable_wire_bytes = 0;
    std::uint64_t chunk_snapshot_wire_bytes = 0;
    std::uint64_t chunk_removal_wire_bytes = 0;
    std::uint64_t world_event_wire_bytes = 0;
    std::uint64_t world_delta_wire_bytes = 0;
    std::uint32_t completed_chunk_snapshots = 0;
    std::uint32_t applied_chunk_removals = 0;
    std::uint32_t applied_voxel_edits = 0;
};

struct MultiplayerSimulationSystemTickTiming {
    std::string name;
    double time_ms = 0.0;
};

struct MultiplayerChunkSubscriptionTickSample {
    MultiplayerChunkSubscriptionPhase phase = MultiplayerChunkSubscriptionPhase::cluster_transition;
    std::uint32_t phase_ordinal = 0;
    std::uint64_t tick = 0;
    std::uint64_t server_tick_time_us = 0;
    double simulation_time_ms = 0.0;
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
    std::uint32_t spatial_event_count = 0;
    std::uint64_t relevant_spatial_event_delivery_count = 0;
    std::uint64_t filtered_spatial_event_delivery_count = 0;
    std::uint64_t filtered_spatial_event_payload_bytes = 0;
    std::uint32_t replication_delta_message_count = 0;
    std::uint32_t delta_advanced_publication_count = 0;
    std::uint32_t delta_avoided_snapshot_count = 0;
    std::uint32_t delta_publication_gap_count = 0;
    double fluid_topology_time_ms = 0.0;
    double fluid_dirty_collection_time_ms = 0.0;
    std::size_t fluid_active_cell_count = 0;
    std::size_t fluid_processed_cell_count = 0;
    std::uint32_t verified_hot_edit_client_state_count = 0;
    std::uint64_t verified_hot_edit_cross_region_exclusion_count = 0;
    std::size_t pending_reliable_message_count = 0;
    std::uint64_t pending_reliable_bytes = 0;
    std::uint32_t disconnected_client_count = 0;
    std::vector<MultiplayerSimulationSystemTickTiming> system_timings;
    std::vector<MultiplayerChunkClientTickTraffic> clients;
};

struct MultiplayerChunkSubscriptionTransitionSample {
    MultiplayerChunkSubscriptionPhase phase = MultiplayerChunkSubscriptionPhase::cluster_transition;
    std::uint32_t ordinal = 0;
    std::uint32_t ticks_to_converge = 0;
    std::size_t peak_pending_reliable_messages = 0;
    bool converged = false;
};

struct MultiplayerChunkSubscriptionSoakSample {
    std::uint32_t cycle = 0;
    std::uint64_t tick = 0;
    std::size_t server_world_chunk_count = 0;
    std::size_t server_voxel_edit_count = 0;
    std::size_t server_entity_count = 0;
    std::size_t server_collision_body_count = 0;
    std::size_t subscription_count = 0;
    std::size_t total_client_chunk_count = 0;
    std::size_t total_client_owned_record_count = 0;
    std::size_t total_client_partial_snapshot_count = 0;
    std::size_t total_client_retained_command_result_count = 0;
    std::size_t settled_queue_depth = 0;
    std::uint64_t settled_queue_bytes = 0;
    std::optional<std::uint64_t> resident_memory_bytes;
    std::optional<std::uint64_t> proportional_set_size_bytes;
    std::optional<std::uint64_t> private_resident_memory_bytes;
    std::optional<std::size_t> thread_count;
    std::optional<std::size_t> open_file_count;
    bool precise_memory_accounting = false;
};

struct MultiplayerChunkClientTrafficSummary {
    core::NetId client_id;
    std::uint64_t reliable_messages = 0;
    std::uint64_t unreliable_messages = 0;
    std::uint64_t chunk_snapshot_slice_messages = 0;
    std::uint64_t chunk_removal_messages = 0;
    std::uint64_t command_result_messages = 0;
    std::uint64_t world_event_messages = 0;
    std::uint64_t world_delta_messages = 0;
    std::uint64_t reliable_wire_bytes = 0;
    std::uint64_t unreliable_wire_bytes = 0;
    std::uint64_t chunk_snapshot_wire_bytes = 0;
    std::uint64_t chunk_removal_wire_bytes = 0;
    std::uint64_t world_event_wire_bytes = 0;
    std::uint64_t world_delta_wire_bytes = 0;
    std::uint64_t applied_voxel_edits = 0;
    std::uint64_t maximum_wire_bytes_per_tick = 0;
};

struct MultiplayerChunkSubscriptionBenchmarkSummary {
    std::uint64_t measured_tick_count = 0;
    double server_tick_p50_ms = 0.0;
    double server_tick_p95_ms = 0.0;
    double server_tick_p99_ms = 0.0;
    double maximum_server_tick_ms = 0.0;
    std::uint64_t hot_edit_tick_count = 0;
    double hot_edit_server_tick_p50_ms = 0.0;
    double hot_edit_server_tick_p95_ms = 0.0;
    double hot_edit_server_tick_p99_ms = 0.0;
    double maximum_hot_edit_server_tick_ms = 0.0;
    std::uint64_t hot_edit_command_count = 0;
    std::uint64_t hot_edit_command_result_message_count = 0;
    std::uint64_t hot_edit_world_event_message_count = 0;
    std::uint64_t hot_edit_replication_delta_message_count = 0;
    std::uint64_t hot_edit_delta_advanced_publication_count = 0;
    std::uint64_t hot_edit_delta_avoided_snapshot_count = 0;
    std::uint64_t hot_edit_delta_publication_gap_count = 0;
    std::uint64_t hot_edit_relevant_spatial_event_delivery_count = 0;
    std::uint64_t hot_edit_filtered_spatial_event_delivery_count = 0;
    std::uint64_t hot_edit_filtered_spatial_event_payload_bytes = 0;
    std::uint64_t hot_edit_world_event_wire_bytes = 0;
    std::uint64_t hot_edit_world_delta_wire_bytes = 0;
    std::uint64_t hot_edit_applied_voxel_edit_count = 0;
    std::uint64_t maximum_hot_edit_wire_bytes_per_client_per_tick = 0;
    std::uint64_t verified_hot_edit_client_states = 0;
    std::uint64_t expected_hot_edit_client_states = 0;
    std::uint64_t verified_hot_edit_cross_region_exclusions = 0;
    std::uint64_t expected_hot_edit_cross_region_exclusions = 0;
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
    std::uint64_t soak_tick_count = 0;
    double soak_server_tick_p50_ms = 0.0;
    double soak_server_tick_p95_ms = 0.0;
    double soak_server_tick_p99_ms = 0.0;
    double maximum_soak_server_tick_ms = 0.0;
    std::uint64_t soak_edit_tick_count = 0;
    std::uint64_t soak_verified_client_states = 0;
    std::uint64_t soak_expected_client_states = 0;
    std::uint64_t soak_verified_cross_region_exclusions = 0;
    std::uint64_t soak_expected_cross_region_exclusions = 0;
    std::uint32_t maximum_soak_transition_convergence_ticks = 0;
    std::uint32_t soak_observed_backlog_burst_count = 0;
    std::uint32_t maximum_soak_backlog_recovery_ticks = 0;
    std::size_t peak_soak_pending_reliable_messages = 0;
    std::uint64_t peak_soak_pending_reliable_bytes = 0;
    std::size_t maximum_soak_partial_snapshot_count = 0;
    std::size_t maximum_soak_stale_publication_count = 0;
    std::uint32_t soak_disconnected_client_count = 0;
    bool soak_process_memory_available = false;
    std::uint64_t soak_baseline_resident_memory_bytes = 0;
    std::uint64_t soak_final_resident_memory_bytes = 0;
    std::uint64_t soak_peak_resident_memory_bytes = 0;
    std::uint64_t soak_baseline_private_memory_bytes = 0;
    std::uint64_t soak_final_private_memory_bytes = 0;
    std::uint64_t soak_peak_private_memory_bytes = 0;
    std::int64_t soak_private_memory_growth_bytes = 0;
    double soak_private_memory_slope_bytes_per_cycle = 0.0;
    std::size_t soak_baseline_server_world_chunk_count = 0;
    std::size_t soak_final_server_world_chunk_count = 0;
    std::size_t soak_peak_server_world_chunk_count = 0;
    std::size_t soak_baseline_server_voxel_edit_count = 0;
    std::size_t soak_final_server_voxel_edit_count = 0;
    std::size_t soak_peak_server_voxel_edit_count = 0;
    std::size_t soak_baseline_total_client_chunk_count = 0;
    std::size_t soak_final_total_client_chunk_count = 0;
    std::size_t soak_peak_total_client_chunk_count = 0;
    std::size_t soak_baseline_total_client_owned_record_count = 0;
    std::size_t soak_final_total_client_owned_record_count = 0;
    std::size_t soak_peak_total_client_owned_record_count = 0;
    std::size_t maximum_soak_settled_queue_depth = 0;
    std::uint64_t maximum_soak_settled_queue_bytes = 0;
    std::int64_t soak_thread_count_growth = 0;
    std::int64_t soak_open_file_count_growth = 0;
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
    static constexpr std::uint32_t schema_version = 3;

    MultiplayerChunkSubscriptionBenchmarkConfig config;
    profiling::RuntimeMetadata runtime;
    MultiplayerChunkSubscriptionBenchmarkSummary summary;
    std::vector<MultiplayerChunkSubscriptionTransitionSample> transitions;
    std::vector<MultiplayerChunkSubscriptionTickSample> raw_ticks;
    std::vector<std::uint64_t> soak_tick_times_us;
    std::vector<MultiplayerChunkSubscriptionSoakSample> soak_samples;
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
