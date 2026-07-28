#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/net/replication.hpp"
#include "engine/net/server_command.hpp"
#include "engine/save/save_snapshot.hpp"
#include "engine/world/operations/world_operation.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::net {
class ClientSession;
class HostSession;
struct HostSessionTickResult;
} // namespace heartstead::net

namespace heartstead::world {

class WorldState;

inline constexpr std::string_view replication_delta_snapshot_payload_type =
    "replication.world_delta_snapshot.bin.v1";
inline constexpr std::string_view replication_delta_snapshot_legacy_payload_type =
    "replication.world_delta_snapshot";

struct WorldReplicationDeltaSubjectPlan {
    core::SaveId subject_id;
    std::uint32_t event_count = 0;
    std::string first_event_type;
    bool has_build_piece = false;
    bool has_entity = false;
    bool has_cargo = false;
    bool has_assembly = false;
    bool has_inventory = false;
    std::uint32_t process_count = 0;
    std::uint32_t materialized_record_count = 0;
    bool missing_subject = false;
    bool has_workpiece = false;
    std::uint32_t private_event_count = 0;
};

struct WorldReplicationDeltaPlan {
    std::uint64_t command_sequence = 0;
    std::string command_type;
    std::uint32_t event_count = 0;
    std::uint32_t global_event_count = 0;
    std::uint32_t subject_event_count = 0;
    std::uint32_t unique_subject_count = 0;
    std::uint32_t missing_subject_count = 0;
    std::uint32_t materialized_record_count = 0;
    bool has_global_events = false;
    bool requires_snapshot_resync = false;
    std::vector<OperationEvent> global_events;
    std::vector<WorldReplicationDeltaSubjectPlan> subjects;
    std::uint64_t replication_sequence = 0;
    core::NetId source_client_id;
};

struct WorldReplicationDeltaSnapshot {
    WorldReplicationDeltaPlan plan;
    std::vector<build::BuildPieceRecord> build_pieces;
    std::vector<save::EntitySaveRecord> entities;
    std::vector<cargo::CargoRecord> cargo_records;
    std::vector<save::InventorySaveRecord> inventories;
    std::vector<save::WorkpieceSaveRecord> workpieces;
    std::vector<assemblies::AssemblyRecord> assemblies;
    std::vector<processes::ProcessInstance> processes;
};

struct WorldReplicationDeltaTickCommand {
    core::NetId client_id;
    std::uint64_t command_sequence = 0;
    std::string command_type;
    bool skipped = false;
    std::string skip_reason;
    std::string error_code;
    std::string error_message;
    net::CommandOperationTrace operation_trace;
    WorldReplicationDeltaSnapshot snapshot;
    std::uint64_t replication_sequence = 0;
};

struct WorldReplicationDeltaTickReport {
    std::uint32_t command_report_count = 0;
    std::uint32_t materialized_command_count = 0;
    std::uint32_t skipped_command_count = 0;
    std::uint32_t total_event_count = 0;
    std::uint32_t total_materialized_record_count = 0;
    bool requires_snapshot_resync = false;
    std::vector<WorldReplicationDeltaTickCommand> commands;
};

struct WorldReplicationDeltaDeliveryCommandReport {
    core::NetId source_client_id;
    std::uint64_t command_sequence = 0;
    std::string command_type;
    std::uint32_t candidate_client_count = 0;
    std::uint32_t relevant_client_count = 0;
    std::uint32_t sent_message_count = 0;
    bool skipped = false;
    std::string skip_reason;
    std::string error_code;
    std::string error_message;
    net::CommandOperationTrace operation_trace;
    std::vector<core::NetId> recipients;
    std::uint64_t replication_sequence = 0;
    std::vector<core::NetId> skipped_recipients;
    std::vector<std::string> recipient_diagnostics;
};

struct WorldReplicationDeltaDeliveryReport {
    std::uint32_t command_delta_count = 0;
    std::uint32_t materialized_command_count = 0;
    std::uint32_t skipped_command_count = 0;
    std::uint32_t relevance_report_count = 0;
    std::uint32_t sent_message_count = 0;
    std::uint32_t unmatched_relevance_count = 0;
    std::uint32_t resync_skipped_count = 0;
    std::vector<WorldReplicationDeltaDeliveryCommandReport> commands;
};

struct WorldReplicationDeltaApplyReport {
    std::uint64_t command_sequence = 0;
    std::string command_type;
    std::uint32_t event_count = 0;
    std::uint32_t global_event_count = 0;
    std::uint32_t planned_record_count = 0;
    std::uint32_t applied_record_count = 0;
    std::uint32_t inserted_record_count = 0;
    std::uint32_t updated_record_count = 0;
    std::uint32_t build_pieces_inserted = 0;
    std::uint32_t build_pieces_updated = 0;
    std::uint32_t entities_inserted = 0;
    std::uint32_t entities_updated = 0;
    std::uint32_t cargo_inserted = 0;
    std::uint32_t cargo_updated = 0;
    std::uint32_t inventories_inserted = 0;
    std::uint32_t inventories_updated = 0;
    std::uint32_t workpieces_inserted = 0;
    std::uint32_t workpieces_updated = 0;
    std::uint32_t assemblies_inserted = 0;
    std::uint32_t assemblies_updated = 0;
    std::uint32_t processes_inserted = 0;
    std::uint32_t processes_updated = 0;
    std::uint32_t voxel_edits_applied = 0;
    std::uint32_t dirty_region_count_before = 0;
    std::uint32_t dirty_region_count_after = 0;
    bool applied = false;
    bool requires_snapshot_resync = false;
    std::uint64_t replication_sequence = 0;
    core::NetId source_client_id;
};

struct WorldClientReplicationBatchApplyReport {
    std::uint64_t command_sequence = 0;
    std::string command_type;
    std::uint32_t event_count = 0;
    bool has_global_events = false;
    bool has_subject_events = false;
    bool has_delta_snapshot = false;
    bool applied_delta = false;
    std::string state;
    std::string skip_reason;
    WorldReplicationDeltaApplyReport delta_apply_report;
    std::uint64_t replication_sequence = 0;
    core::NetId source_client_id;
};

struct WorldClientReplicationApplyReport {
    std::uint32_t drained_batch_count = 0;
    std::uint32_t delta_snapshot_count = 0;
    std::uint32_t matched_delta_count = 0;
    std::uint32_t applied_delta_count = 0;
    std::uint32_t pending_delta_count = 0;
    std::uint32_t observed_event_only_count = 0;
    std::uint32_t unmatched_delta_count = 0;
    std::uint32_t total_event_count = 0;
    std::uint32_t total_applied_record_count = 0;
    std::vector<OperationEvent> observed_events;
    std::vector<WorldClientReplicationBatchApplyReport> batches;
};

[[nodiscard]] WorldReplicationDeltaPlan plan_replication_delta(const WorldState& state,
                                                               const net::ReplicationBatch& batch);
[[nodiscard]] WorldReplicationDeltaSnapshot
materialize_replication_delta(const WorldState& state, const net::ReplicationBatch& batch);
[[nodiscard]] core::Result<WorldReplicationDeltaSnapshot>
filter_replication_delta_snapshot(const WorldReplicationDeltaSnapshot& snapshot,
                                  const net::ReplicationRelevancePolicy& policy,
                                  core::NetId recipient);
[[nodiscard]] WorldReplicationDeltaTickReport
materialize_replication_deltas_for_tick(const WorldState& state,
                                        const net::HostSessionTickResult& tick);
[[nodiscard]] core::Result<WorldReplicationDeltaDeliveryReport>
send_replication_delta_snapshots_for_tick(net::HostSession& host,
                                          const WorldReplicationDeltaTickReport& delta_report,
                                          const net::HostSessionTickResult& tick,
                                          std::int64_t server_time_ms);
[[nodiscard]] core::Result<WorldReplicationDeltaApplyReport>
apply_replication_delta(WorldState& state, const WorldReplicationDeltaSnapshot& snapshot);
[[nodiscard]] core::Result<WorldClientReplicationApplyReport> apply_client_replication_deltas(
    WorldState& state, net::ClientSession& client_session,
    std::span<const WorldReplicationDeltaSnapshot> decoded_delta_snapshots);
[[nodiscard]] core::Result<std::vector<WorldReplicationDeltaSnapshot>>
drain_client_replication_delta_snapshots(net::ClientSession& client_session);
[[nodiscard]] core::Result<WorldClientReplicationApplyReport>
apply_client_queued_replication_deltas(WorldState& state, net::ClientSession& client_session);

class WorldReplicationDeltaSnapshotTextCodec {
  public:
    [[nodiscard]] static std::string encode(const WorldReplicationDeltaSnapshot& snapshot);
    [[nodiscard]] static core::Result<WorldReplicationDeltaSnapshot> decode(std::string_view text);
};

class WorldReplicationDeltaSnapshotBinaryCodec {
  public:
    [[nodiscard]] static core::Result<std::string>
    encode(const WorldReplicationDeltaSnapshot& snapshot);
    [[nodiscard]] static core::Result<WorldReplicationDeltaSnapshot> decode(std::string_view bytes);
};

[[nodiscard]] core::Result<net::TransportMessage>
make_replication_delta_transport_message(const WorldReplicationDeltaSnapshot& snapshot,
                                         std::int64_t server_time_ms);
[[nodiscard]] core::Result<WorldReplicationDeltaSnapshot>
replication_delta_snapshot_from_transport(const net::TransportEnvelope& envelope);

} // namespace heartstead::world
