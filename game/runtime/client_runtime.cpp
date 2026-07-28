#include "game/runtime/client_runtime.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace heartstead::game {

ClientRuntime::ClientRuntime(core::NetId expected_client_id, world::WorldStateDesc world_desc,
                             const ReplicationRegistry* replication_registry)
    : world_(std::move(world_desc)), replication_registry_(replication_registry),
      session_(expected_client_id) {}

core::Status ClientRuntime::receive(std::span<const net::TransportEnvelope> messages) {
    for (const auto& message : messages) {
        auto status = session_.receive_server_message(message);
        if (!status) {
            return status;
        }
        ++messages_since_sync_;
    }
    return core::Status::ok();
}

core::Result<ClientRuntimeStats> ClientRuntime::synchronize() {
    player_tombstones_.clear();
    auto completed_chunks = apply_queued_chunk_snapshots();
    if (!completed_chunks) {
        return core::Result<ClientRuntimeStats>::failure(completed_chunks.error().code,
                                                         completed_chunks.error().message);
    }
    auto replication = world::apply_client_queued_replication_deltas(world_, session_);
    if (!replication) {
        return core::Result<ClientRuntimeStats>::failure(replication.error().code,
                                                         replication.error().message);
    }
    ClientReplicationDispatchStats feature_replication;
    if (replication_registry_ != nullptr) {
        auto dispatched =
            replication_registry_->dispatch(replication.value().observed_events, *this);
        if (!dispatched) {
            return core::Result<ClientRuntimeStats>::failure(dispatched.error().code,
                                                             dispatched.error().message);
        }
        feature_replication = dispatched.value();
    } else {
        feature_replication.observed_event_count =
            static_cast<std::uint32_t>(replication.value().observed_events.size());
        feature_replication.unhandled_event_count = feature_replication.observed_event_count;
    }
    auto results = session_.drain_command_results();
    const auto result_count = static_cast<std::uint32_t>(results.size());
    command_results_.insert(command_results_.end(), std::make_move_iterator(results.begin()),
                            std::make_move_iterator(results.end()));
    auto movement_messages =
        session_.drain_replication_messages(movement::movement_snapshot_payload_type);
    auto entity_motion_messages =
        session_.drain_replication_messages(entities::entity_motion_snapshot_payload_type);
    auto entity_motion_removals =
        session_.drain_replication_messages(entities::entity_motion_removal_payload_type);
    auto assignments =
        session_.drain_replication_messages(movement::player_assignment_payload_type);
    auto removals = session_.drain_replication_messages(movement::player_removal_payload_type);
    for (const auto& assignment_message : assignments) {
        auto assignment = movement::player_assignment_from_transport(assignment_message);
        if (!assignment) {
            return core::Result<ClientRuntimeStats>::failure(assignment.error().code,
                                                             assignment.error().message);
        }
        if (local_player_net_id_.is_valid() && local_player_net_id_ != assignment.value()) {
            return core::Result<ClientRuntimeStats>::failure(
                "client_runtime.player_reassigned",
                "connected client received a conflicting local player assignment");
        }
        local_player_net_id_ = assignment.value();
    }
    for (const auto& removal_message : removals) {
        auto removal = movement::player_removal_from_transport(removal_message);
        if (!removal) {
            return core::Result<ClientRuntimeStats>::failure(removal.error().code,
                                                             removal.error().message);
        }
        movement_snapshots_.erase(removal.value().value());
        player_tombstones_.push_back(removal.value());
    }
    std::uint32_t movement_snapshot_count = 0;
    for (const auto& message : movement_messages) {
        auto snapshot = movement::movement_snapshot_from_transport(message);
        if (!snapshot) {
            return core::Result<ClientRuntimeStats>::failure(snapshot.error().code,
                                                             snapshot.error().message);
        }
        const auto player_key = snapshot.value().player_net_id.value();
        const auto found = movement_snapshots_.find(player_key);
        if (found != movement_snapshots_.end() &&
            found->second.state.simulation_tick >= snapshot.value().state.simulation_tick) {
            continue;
        }
        movement_snapshots_.insert_or_assign(player_key, std::move(snapshot).value());
        ++movement_snapshot_count;
    }
    for (const auto& message : entity_motion_removals) {
        auto removal = entities::entity_motion_removal_from_transport(message);
        if (!removal) {
            return core::Result<ClientRuntimeStats>::failure(removal.error().code,
                                                             removal.error().message);
        }
        entity_motion_snapshots_.erase(removal.value().value());
    }
    std::uint32_t entity_motion_snapshot_count = 0;
    for (const auto& message : entity_motion_messages) {
        auto snapshot = entities::entity_motion_snapshot_from_transport(message);
        if (!snapshot) {
            return core::Result<ClientRuntimeStats>::failure(snapshot.error().code,
                                                             snapshot.error().message);
        }
        const auto key = snapshot.value().entity_net_id.value();
        const auto found = entity_motion_snapshots_.find(key);
        if (found != entity_motion_snapshots_.end() &&
            found->second.simulation_tick >= snapshot.value().simulation_tick) {
            continue;
        }
        entity_motion_snapshots_.insert_or_assign(key, std::move(snapshot).value());
        ++entity_motion_snapshot_count;
    }
    ClientRuntimeStats stats;
    stats.received_message_count = messages_since_sync_;
    stats.command_result_count = result_count;
    stats.movement_snapshot_count = movement_snapshot_count;
    stats.entity_motion_snapshot_count = entity_motion_snapshot_count;
    stats.entity_motion_tombstone_count = static_cast<std::uint32_t>(entity_motion_removals.size());
    stats.player_tombstone_count = static_cast<std::uint32_t>(player_tombstones_.size());
    stats.chunk_snapshot_slice_count = completed_chunks.value().slice_count;
    stats.completed_chunk_snapshot_count = completed_chunks.value().completed_chunk_count;
    stats.replication = std::move(replication).value();
    stats.feature_replication = feature_replication;
    messages_since_sync_ = 0;
    return core::Result<ClientRuntimeStats>::success(std::move(stats));
}

core::Result<net::CommandEnvelope>
ClientRuntime::create_command(std::string type, std::string payload, std::int64_t now_ms) {
    return session_.create_command(std::move(type), std::move(payload), now_ms);
}

bool ClientRuntime::is_connected() const noexcept {
    return session_.is_connected();
}

core::NetId ClientRuntime::client_id() const noexcept {
    return session_.client_id();
}

world::WorldState& ClientRuntime::world() noexcept {
    return world_;
}

const world::WorldState& ClientRuntime::world() const noexcept {
    return world_;
}

net::ClientSession& ClientRuntime::session() noexcept {
    return session_;
}

const net::ClientSession& ClientRuntime::session() const noexcept {
    return session_;
}

std::span<const net::HostSessionCommandResult> ClientRuntime::command_results() const noexcept {
    return command_results_;
}

const movement::PlayerControllerSnapshot*
ClientRuntime::player_snapshot(core::NetId player_net_id) const noexcept {
    const auto found = movement_snapshots_.find(player_net_id.value());
    return found == movement_snapshots_.end() ? nullptr : &found->second;
}

core::NetId ClientRuntime::local_player_net_id() const noexcept {
    return local_player_net_id_;
}

const movement::PlayerControllerSnapshot* ClientRuntime::local_player_snapshot() const noexcept {
    return player_snapshot(local_player_net_id_);
}

std::vector<const movement::PlayerControllerSnapshot*> ClientRuntime::movement_snapshots() const {
    std::vector<const movement::PlayerControllerSnapshot*> result;
    result.reserve(movement_snapshots_.size());
    for (const auto& [_, snapshot] : movement_snapshots_) {
        result.push_back(&snapshot);
    }
    std::ranges::sort(result, [](const auto* lhs, const auto* rhs) {
        return lhs->player_net_id.value() < rhs->player_net_id.value();
    });
    return result;
}

std::vector<const entities::EntityMotionSnapshot*> ClientRuntime::entity_motion_snapshots() const {
    std::vector<const entities::EntityMotionSnapshot*> result;
    result.reserve(entity_motion_snapshots_.size());
    for (const auto& [_, snapshot] : entity_motion_snapshots_) {
        result.push_back(&snapshot);
    }
    std::ranges::sort(result, [](const auto* lhs, const auto* rhs) {
        return lhs->entity_net_id.value() < rhs->entity_net_id.value();
    });
    return result;
}

std::span<const core::NetId> ClientRuntime::player_tombstones() const noexcept {
    return player_tombstones_;
}

void ClientRuntime::clear_command_results() noexcept {
    command_results_.clear();
}

core::Result<ClientRuntime::ChunkSnapshotApplyStats> ClientRuntime::apply_queued_chunk_snapshots() {
    auto messages = session_.drain_replication_messages(world::chunk_snapshot_slice_payload_type);
    std::uint32_t completed_count = 0;
    for (const auto& message : messages) {
        auto slice = world::chunk_snapshot_slice_from_transport(message);
        if (!slice) {
            return core::Result<ChunkSnapshotApplyStats>::failure(slice.error().code,
                                                                  slice.error().message);
        }
        const auto coordinate = slice.value().identity.coordinate;
        if (const auto resident = remote_chunks_.find(coordinate);
            resident != remote_chunks_.end() &&
            (resident->second.first.load_generation > slice.value().identity.load_generation ||
             (resident->second.first == slice.value().identity &&
              resident->second.second >= slice.value().content_revision))) {
            continue;
        }
        auto [found, inserted] = chunk_snapshot_assemblies_.try_emplace(coordinate);
        auto& assembly = found->second;
        if (inserted || assembly.identity != slice.value().identity ||
            assembly.content_revision != slice.value().content_revision) {
            if (!inserted &&
                (assembly.identity.load_generation > slice.value().identity.load_generation ||
                 (assembly.identity == slice.value().identity &&
                  assembly.content_revision > slice.value().content_revision))) {
                continue;
            }
            assembly = {};
            assembly.identity = slice.value().identity;
            assembly.content_revision = slice.value().content_revision;
        }
        const auto y = static_cast<std::size_t>(slice.value().slice_y);
        if (assembly.received[y]) {
            if (assembly.slices[y] != slice.value().cells) {
                return core::Result<ChunkSnapshotApplyStats>::failure(
                    "client_runtime.conflicting_chunk_slice",
                    "duplicate chunk snapshot slice contains different cells");
            }
            continue;
        }
        assembly.slices[y] = std::move(slice).value().cells;
        assembly.received[y] = true;
        if (!std::ranges::all_of(assembly.received, [](bool received) { return received; })) {
            continue;
        }

        std::vector<world::VoxelCell> cells(world::VoxelChunk::total_cells);
        constexpr auto edge = static_cast<std::size_t>(world::VoxelChunk::edge_length);
        for (std::size_t slice_y = 0; slice_y < edge; ++slice_y) {
            for (std::size_t z = 0; z < edge; ++z) {
                for (std::size_t x = 0; x < edge; ++x) {
                    cells[z * edge * edge + slice_y * edge + x] =
                        assembly.slices[slice_y][z * edge + x];
                }
            }
        }
        auto& chunk = world_.chunks().get_or_create(coordinate);
        auto status = chunk.load_generated_cells(std::move(cells));
        if (!status) {
            return core::Result<ChunkSnapshotApplyStats>::failure(status.error().code,
                                                                  status.error().message);
        }
        remote_chunks_.insert_or_assign(coordinate,
                                        std::pair{assembly.identity, assembly.content_revision});
        chunk_snapshot_assemblies_.erase(coordinate);
        ++completed_count;
    }
    if (messages.size() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<ChunkSnapshotApplyStats>::failure(
            "client_runtime.chunk_snapshot_count_overflow",
            "chunk snapshot synchronization count exceeds one frame's diagnostic range");
    }
    return core::Result<ChunkSnapshotApplyStats>::success(
        {static_cast<std::uint32_t>(messages.size()), completed_count});
}

} // namespace heartstead::game
