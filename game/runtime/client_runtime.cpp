#include "game/runtime/client_runtime.hpp"

#include "engine/core/logging.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

namespace heartstead::game {

ClientRuntime::ClientRuntime(core::NetId expected_client_id, world::WorldStateDesc world_desc,
                             const ReplicationRegistry* replication_registry,
                             const world::VoxelPalette* movement_palette)
    : world_(std::move(world_desc)), replication_registry_(replication_registry),
      session_(expected_client_id) {
    if (movement_palette != nullptr) {
        prediction_collision_ = std::make_unique<movement::VoxelCharacterCollisionWorld>(
            world_.chunks(), *movement_palette);
    }
}

core::Status ClientRuntime::receive(std::span<const net::TransportEnvelope> messages) {
    for (const auto& message : messages) {
        // Unreliable snapshots may overtake the reliable welcome on an impaired path. They are
        // superseding state and will be sent again, so discard them until session identity has
        // been established instead of turning benign packet reordering into a terminal fault.
        if (!session_.is_connected() &&
            message.message.kind != net::TransportMessageKind::control) {
            continue;
        }
        auto status = session_.receive_server_message(message);
        if (!status) {
            return status;
        }
        ++messages_since_sync_;
    }
    return core::Status::ok();
}

core::Result<ClientRuntimeStats> ClientRuntime::synchronize(std::uint64_t render_tick) {
    std::uint32_t reconciled_input_count = 0;
    std::uint32_t acknowledged_input_count = 0;
    std::uint32_t hard_correction_count = 0;
    std::uint32_t collision_revision_change_count = 0;
    std::uint32_t interpolated_player_count = 0;
    double maximum_correction_distance = 0.0;
    player_tombstones_.clear();
    accepted_voxel_edits_.clear();
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
    for (const auto& event : replication.value().observed_events) {
        if (event.type != world::voxel_changed_event_type) {
            continue;
        }
        auto change = world::VoxelChangeTextCodec::decode(event.message);
        if (!change) {
            return core::Result<ClientRuntimeStats>::failure(change.error().code,
                                                             change.error().message);
        }
        auto status = record_accepted_voxel_edit(std::move(change).value());
        if (!status) {
            return core::Result<ClientRuntimeStats>::failure(status.error().code,
                                                             status.error().message);
        }
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
    std::uint32_t dropped_command_result_count = 0;
    if (command_results_.size() > client_command_result_history_capacity) {
        const auto dropped = command_results_.size() - client_command_result_history_capacity;
        dropped_command_result_count = static_cast<std::uint32_t>(dropped);
        command_results_.erase(command_results_.begin(), command_results_.begin() + dropped);
    }
    auto movement_messages =
        session_.drain_replication_messages(movement::movement_snapshot_payload_type);
    auto legacy_movement_messages =
        session_.drain_replication_messages(movement::legacy_movement_snapshot_payload_type);
    movement_messages.insert(movement_messages.end(),
                             std::make_move_iterator(legacy_movement_messages.begin()),
                             std::make_move_iterator(legacy_movement_messages.end()));
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
        authoritative_movement_ticks_.erase(removal.value().value());
        remote_player_interpolators_.erase(removal.value().value());
        if (removal.value() == local_player_net_id_) {
            predicted_local_snapshot_.reset();
            last_prediction_diagnostics_.reset();
            prediction_buffer_.clear();
        }
        player_tombstones_.push_back(removal.value());
    }
    std::uint32_t movement_snapshot_count = 0;
    for (const auto& message : movement_messages) {
        auto snapshot = movement::movement_snapshot_from_transport(message);
        if (!snapshot) {
            return core::Result<ClientRuntimeStats>::failure(snapshot.error().code,
                                                             snapshot.error().message);
        }
        auto authoritative = std::move(snapshot).value();
        const auto player_key = authoritative.player_net_id.value();
        const auto previous_tick = authoritative_movement_ticks_.find(player_key);
        if (previous_tick != authoritative_movement_ticks_.end() &&
            previous_tick->second >= authoritative.state.simulation_tick) {
            continue;
        }
        authoritative_movement_ticks_.insert_or_assign(player_key,
                                                       authoritative.state.simulation_tick);
        if (authoritative.player_net_id == local_player_net_id_) {
            if (predicted_local_snapshot_.has_value() && prediction_collision_ != nullptr) {
                const auto predicted_before = predicted_local_snapshot_->state;
                auto reconciled = prediction_buffer_.reconcile(
                    predicted_local_snapshot_->state, authoritative, prediction_controller_, {},
                    *prediction_collision_);
                if (!reconciled) {
                    return core::Result<ClientRuntimeStats>::failure(reconciled.error().code,
                                                                     reconciled.error().message);
                }
                reconciled_input_count +=
                    static_cast<std::uint32_t>(reconciled.value().replayed_input_count);
                acknowledged_input_count +=
                    static_cast<std::uint32_t>(reconciled.value().acknowledged_input_count);
                maximum_correction_distance =
                    std::max(maximum_correction_distance, reconciled.value().correction_distance);
                if (reconciled.value().hard_correction) {
                    ++hard_correction_count;
                    const auto predicted_position = predicted_before.position.approximate_global();
                    const auto authoritative_position =
                        authoritative.state.position.approximate_global();
                    const auto reconciled_position =
                        reconciled.value().state.position.approximate_global();
                    std::ostringstream diagnostic;
                    diagnostic << "movement hard correction distance="
                               << reconciled.value().correction_distance << " predicted="
                               << predicted_position.x << ',' << predicted_position.y << ','
                               << predicted_position.z << " authoritative="
                               << authoritative_position.x << ',' << authoritative_position.y << ','
                               << authoritative_position.z << " reconciled="
                               << reconciled_position.x << ',' << reconciled_position.y << ','
                               << reconciled_position.z << " acknowledged="
                               << reconciled.value().acknowledged_input_count << " replayed="
                               << reconciled.value().replayed_input_count << " collision_revision="
                               << prediction_buffer_.collision_world_revision() << "->"
                               << authoritative.collision_world_revision;
                    core::log(core::LogLevel::warning, diagnostic.str());
                }
                if (reconciled.value().collision_world_revision_changed) {
                    ++collision_revision_change_count;
                }
                predicted_local_snapshot_ = authoritative;
                predicted_local_snapshot_->state = std::move(reconciled).value().state;
                predicted_local_snapshot_->last_processed_input_sequence =
                    predicted_local_snapshot_->state.last_input_sequence;
            } else {
                predicted_local_snapshot_ = authoritative;
            }
            prediction_buffer_.set_collision_world_revision(authoritative.collision_world_revision);
            movement_snapshots_.insert_or_assign(player_key, *predicted_local_snapshot_);
        } else {
            auto [interpolator, _] = remote_player_interpolators_.try_emplace(player_key);
            auto status = interpolator->second.push(authoritative);
            if (!status) {
                return core::Result<ClientRuntimeStats>::failure(status.error().code,
                                                                 status.error().message);
            }
            movement_snapshots_.insert_or_assign(player_key, std::move(authoritative));
        }
        ++movement_snapshot_count;
    }
    if (render_tick == 0) {
        for (const auto& [_, tick] : authoritative_movement_ticks_) {
            render_tick = std::max(render_tick, tick);
        }
    }
    for (const auto& [player_key, interpolator] : remote_player_interpolators_) {
        auto found = movement_snapshots_.find(player_key);
        if (found == movement_snapshots_.end()) {
            continue;
        }
        auto sampled = interpolator.sample(render_tick);
        if (!sampled) {
            return core::Result<ClientRuntimeStats>::failure(sampled.error().code,
                                                             sampled.error().message);
        }
        found->second.state = std::move(sampled).value();
        ++interpolated_player_count;
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
    stats.retained_command_result_count = static_cast<std::uint32_t>(command_results_.size());
    stats.dropped_command_result_count = dropped_command_result_count;
    stats.movement_snapshot_count = movement_snapshot_count;
    stats.entity_motion_snapshot_count = entity_motion_snapshot_count;
    stats.entity_motion_tombstone_count = static_cast<std::uint32_t>(entity_motion_removals.size());
    stats.player_tombstone_count = static_cast<std::uint32_t>(player_tombstones_.size());
    stats.predicted_input_count = predicted_inputs_since_sync_;
    stats.reconciled_input_count = reconciled_input_count;
    stats.acknowledged_input_count = acknowledged_input_count;
    stats.hard_correction_count = hard_correction_count;
    stats.collision_revision_change_count = collision_revision_change_count;
    stats.interpolated_player_count = interpolated_player_count;
    stats.maximum_correction_distance = maximum_correction_distance;
    stats.chunk_snapshot_slice_count = completed_chunks.value().slice_count;
    stats.completed_chunk_snapshot_count = completed_chunks.value().completed_chunk_count;
    stats.replication = std::move(replication).value();
    stats.feature_replication = feature_replication;
    messages_since_sync_ = 0;
    predicted_inputs_since_sync_ = 0;
    return core::Result<ClientRuntimeStats>::success(std::move(stats));
}

core::Result<net::CommandEnvelope>
ClientRuntime::create_command(std::string type, std::string payload, std::int64_t now_ms) {
    return session_.create_command(std::move(type), std::move(payload), now_ms);
}

core::Result<movement::PlayerInputBundle>
ClientRuntime::movement_input_bundle(const movement::PlayerInputFrame& input) const {
    auto normalized = input;
    auto status = normalized.validate();
    if (!status) {
        return core::Result<movement::PlayerInputBundle>::failure(status.error().code,
                                                                  status.error().message);
    }
    movement::PlayerInputBundle bundle;
    bundle.frames = prediction_buffer_.unacknowledged();
    if (!bundle.frames.empty() && normalized.tick <= bundle.frames.back().tick) {
        if (bundle.frames.back().tick == std::numeric_limits<std::uint64_t>::max()) {
            return core::Result<movement::PlayerInputBundle>::failure(
                "client_runtime.movement_tick_exhausted",
                "movement input history exhausted its tick space");
        }
        normalized.tick = bundle.frames.back().tick + 1;
    }
    bundle.frames.push_back(normalized);
    if (bundle.frames.size() > 4) {
        bundle.frames.erase(bundle.frames.begin(), bundle.frames.end() - 4);
    }
    status = bundle.validate();
    if (!status) {
        return core::Result<movement::PlayerInputBundle>::failure(status.error().code,
                                                                  status.error().message);
    }
    return core::Result<movement::PlayerInputBundle>::success(std::move(bundle));
}

core::Status ClientRuntime::predict_local_input(const movement::PlayerInputFrame& input) {
    if (!local_player_net_id_.is_valid() || !predicted_local_snapshot_.has_value()) {
        return core::Status::failure(
            "client_runtime.local_player_unavailable",
            "movement prediction requires an assigned local player snapshot");
    }
    if (prediction_collision_ == nullptr) {
        return core::Status::failure("client_runtime.prediction_collision_unavailable",
                                     "movement prediction requires a client voxel collision world");
    }
    auto status = prediction_buffer_.record(input);
    if (!status) {
        return status;
    }
    auto predicted = prediction_controller_.tick(predicted_local_snapshot_->state, input, {},
                                                 *prediction_collision_);
    if (!predicted) {
        prediction_buffer_.clear();
        return core::Status::failure(predicted.error().code, predicted.error().message);
    }
    auto predicted_tick = std::move(predicted).value();
    predicted_local_snapshot_->state = std::move(predicted_tick.state);
    last_prediction_diagnostics_ = predicted_tick.diagnostics;
    predicted_local_snapshot_->last_processed_input_sequence =
        predicted_local_snapshot_->state.last_input_sequence;
    movement_snapshots_.insert_or_assign(local_player_net_id_.value(), *predicted_local_snapshot_);
    ++predicted_inputs_since_sync_;
    return core::Status::ok();
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

const movement::PlayerControllerTickDiagnostics*
ClientRuntime::last_prediction_diagnostics() const noexcept {
    return last_prediction_diagnostics_.has_value() ? &*last_prediction_diagnostics_ : nullptr;
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

std::span<const world::VoxelChangeRecord> ClientRuntime::accepted_voxel_edits() const noexcept {
    return accepted_voxel_edits_;
}

core::Status ClientRuntime::record_accepted_voxel_edit(world::VoxelChangeRecord change) {
    auto status = change.validate();
    if (!status) {
        return status;
    }
    const auto address = world::block_to_chunk_local(change.position);
    auto current = world_.chunks().get(address.chunk, address.local);
    if (!current) {
        return core::Status::failure(
            "client_runtime.accepted_voxel_chunk_missing",
            "accepted voxel edit was dispatched before its client chunk existed");
    }
    if (current.value() != change.current) {
        return core::Status::failure(
            "client_runtime.accepted_voxel_not_applied",
            "accepted voxel edit was dispatched before its client world mutation");
    }
    accepted_voxel_edits_.push_back(std::move(change));
    return core::Status::ok();
}

void ClientRuntime::clear_command_results() noexcept {
    command_results_.clear();
}

core::Result<ClientRuntime::ChunkSnapshotApplyStats> ClientRuntime::apply_queued_chunk_snapshots() {
    auto messages = session_.drain_replication_messages(world::chunk_snapshot_slice_payload_type);
    auto legacy_messages =
        session_.drain_replication_messages(world::legacy_chunk_snapshot_slice_payload_type);
    messages.insert(messages.end(), std::make_move_iterator(legacy_messages.begin()),
                    std::make_move_iterator(legacy_messages.end()));
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
