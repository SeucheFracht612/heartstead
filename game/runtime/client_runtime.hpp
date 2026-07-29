#pragma once

#include "engine/entities/entity_motion_snapshot.hpp"
#include "engine/movement/character_collision.hpp"
#include "engine/movement/movement_prediction.hpp"
#include "engine/movement/remote_player_interpolation.hpp"
#include "engine/net/client_session.hpp"
#include "engine/world/chunks/chunk_replication.hpp"
#include "engine/world/replication_delta.hpp"
#include "engine/world/voxel_change.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "engine/world/world_state.hpp"
#include "game/framework/gameplay_module.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace heartstead::game {

inline constexpr std::size_t client_command_result_history_capacity = 256;

struct ClientRuntimeStats {
    std::uint32_t received_message_count = 0;
    std::uint32_t command_result_count = 0;
    std::uint32_t retained_command_result_count = 0;
    std::uint32_t dropped_command_result_count = 0;
    std::uint32_t movement_snapshot_count = 0;
    std::uint32_t entity_motion_snapshot_count = 0;
    std::uint32_t entity_motion_tombstone_count = 0;
    std::uint32_t player_tombstone_count = 0;
    std::uint32_t predicted_input_count = 0;
    std::uint32_t reconciled_input_count = 0;
    std::uint32_t acknowledged_input_count = 0;
    std::uint32_t hard_correction_count = 0;
    std::uint32_t collision_revision_change_count = 0;
    std::uint32_t interpolated_player_count = 0;
    double maximum_correction_distance = 0.0;
    std::uint32_t chunk_snapshot_slice_count = 0;
    std::uint32_t completed_chunk_snapshot_count = 0;
    world::WorldClientReplicationApplyReport replication;
    ClientReplicationDispatchStats feature_replication;
};

class ClientRuntime final {
  public:
    ClientRuntime(core::NetId expected_client_id, world::WorldStateDesc world_desc,
                  const ReplicationRegistry* replication_registry = nullptr,
                  const world::VoxelPalette* movement_palette = nullptr);

    [[nodiscard]] core::Status receive(std::span<const net::TransportEnvelope> messages);
    [[nodiscard]] core::Result<ClientRuntimeStats> synchronize(std::uint64_t render_tick = 0);
    [[nodiscard]] core::Result<net::CommandEnvelope>
    create_command(std::string type, std::string payload, std::int64_t now_ms);
    [[nodiscard]] core::Result<movement::PlayerInputBundle>
    movement_input_bundle(const movement::PlayerInputFrame& input) const;
    [[nodiscard]] core::Status predict_local_input(const movement::PlayerInputFrame& input);

    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] core::NetId client_id() const noexcept;
    [[nodiscard]] world::WorldState& world() noexcept;
    [[nodiscard]] const world::WorldState& world() const noexcept;
    [[nodiscard]] net::ClientSession& session() noexcept;
    [[nodiscard]] const net::ClientSession& session() const noexcept;
    [[nodiscard]] std::span<const net::HostSessionCommandResult> command_results() const noexcept;
    [[nodiscard]] const movement::PlayerControllerSnapshot*
    player_snapshot(core::NetId player_net_id) const noexcept;
    [[nodiscard]] core::NetId local_player_net_id() const noexcept;
    [[nodiscard]] const movement::PlayerControllerSnapshot* local_player_snapshot() const noexcept;
    [[nodiscard]] const movement::PlayerControllerTickDiagnostics*
    last_prediction_diagnostics() const noexcept;
    [[nodiscard]] std::vector<const movement::PlayerControllerSnapshot*> movement_snapshots() const;
    [[nodiscard]] std::vector<const entities::EntityMotionSnapshot*>
    entity_motion_snapshots() const;
    [[nodiscard]] std::span<const core::NetId> player_tombstones() const noexcept;
    [[nodiscard]] std::span<const world::VoxelChangeRecord> accepted_voxel_edits() const noexcept;
    [[nodiscard]] core::Status record_accepted_voxel_edit(world::VoxelChangeRecord change);
    void clear_command_results() noexcept;

  private:
    struct ChunkSnapshotApplyStats {
        std::uint32_t slice_count = 0;
        std::uint32_t completed_chunk_count = 0;
    };

    struct ChunkSnapshotAssembly {
        world::ChunkIdentity identity;
        std::uint64_t content_revision = 0;
        std::array<std::vector<world::VoxelCell>, world::VoxelChunk::edge_length> slices;
        std::array<bool, world::VoxelChunk::edge_length> received{};
    };

    [[nodiscard]] core::Result<ChunkSnapshotApplyStats> apply_queued_chunk_snapshots();

    world::WorldState world_;
    const ReplicationRegistry* replication_registry_ = nullptr;
    net::ClientSession session_;
    std::vector<net::HostSessionCommandResult> command_results_;
    std::unordered_map<std::uint64_t, movement::PlayerControllerSnapshot> movement_snapshots_;
    std::unordered_map<std::uint64_t, std::uint64_t> authoritative_movement_ticks_;
    std::unordered_map<std::uint64_t, movement::RemotePlayerInterpolator>
        remote_player_interpolators_;
    std::unordered_map<std::uint64_t, entities::EntityMotionSnapshot> entity_motion_snapshots_;
    std::vector<core::NetId> player_tombstones_;
    std::vector<world::VoxelChangeRecord> accepted_voxel_edits_;
    core::NetId local_player_net_id_;
    std::map<world::ChunkCoord, ChunkSnapshotAssembly> chunk_snapshot_assemblies_;
    std::map<world::ChunkCoord, std::pair<world::ChunkIdentity, std::uint64_t>> remote_chunks_;
    std::uint32_t messages_since_sync_ = 0;
    movement::PlayerController prediction_controller_;
    movement::MovementPredictionBuffer prediction_buffer_;
    std::unique_ptr<movement::VoxelCharacterCollisionWorld> prediction_collision_;
    std::optional<movement::PlayerControllerSnapshot> predicted_local_snapshot_;
    std::optional<movement::PlayerControllerTickDiagnostics> last_prediction_diagnostics_;
    std::uint32_t predicted_inputs_since_sync_ = 0;
};

} // namespace heartstead::game
