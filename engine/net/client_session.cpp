#include "engine/net/client_session.hpp"

#include <utility>

namespace heartstead::net {

namespace {

template <typename T> [[nodiscard]] std::vector<T> drain_vector(std::vector<T>& values) {
    std::vector<T> drained;
    drained.swap(values);
    return drained;
}

} // namespace

ClientSession::ClientSession(core::NetId expected_client_id)
    : expected_client_id_(expected_client_id) {}

ClientSessionState ClientSession::state() const noexcept {
    return state_;
}

bool ClientSession::is_connected() const noexcept {
    return state_ == ClientSessionState::connected;
}

core::NetId ClientSession::expected_client_id() const noexcept {
    return expected_client_id_;
}

core::NetId ClientSession::client_id() const noexcept {
    return transport_session_.client_id.is_valid() ? transport_session_.client_id
                                                   : expected_client_id_;
}

core::NetId ClientSession::server_id() const noexcept {
    return transport_session_.server_id;
}

std::uint64_t ClientSession::next_command_sequence() const noexcept {
    return next_command_sequence_;
}

const TransportClientSession* ClientSession::transport_session() const noexcept {
    return is_connected() ? &transport_session_ : nullptr;
}

ClientSessionStats ClientSession::stats() const {
    ClientSessionStats stats;
    stats.pending_command_count = pending_commands_.size();
    stats.queued_result_count = command_results_.size();
    stats.queued_replication_batch_count = replication_batches_.size();
    for (const auto& batch : replication_batches_) {
        stats.queued_replication_event_count += batch.events.size();
    }
    stats.queued_replication_message_count = replication_messages_.size();
    if (!replication_messages_.empty()) {
        stats.first_queued_replication_message_payload_type =
            replication_messages_.front().message.payload_type;
    }
    stats.has_replication_sequence = has_replication_sequence_;
    stats.last_replication_sequence = last_replication_sequence_;
    stats.disconnect_reason_code = disconnect_reason_code_;
    stats.disconnect_reason_message = disconnect_reason_message_;
    stats.disconnected_at_ms = disconnected_at_ms_;
    return stats;
}

ReplicationIntakeReport ClientSession::replication_intake_report() const {
    return ReplicationIntake::summarize(replication_batches_);
}

core::Status ClientSession::accept_welcome(const TransportEnvelope& envelope) {
    if (is_connected()) {
        return core::Status::failure("client_session.already_connected",
                                     "client session already accepted a server welcome");
    }

    const auto expected = expected_client_id_.is_valid() ? expected_client_id_ : envelope.recipient;
    auto session = accept_transport_server_welcome(expected, envelope);
    if (!session) {
        return core::Status::failure(session.error().code, session.error().message);
    }

    transport_session_ = session.value();
    expected_client_id_ = transport_session_.client_id;
    has_replication_sequence_ = false;
    last_replication_sequence_ = 0;
    disconnect_reason_code_.clear();
    disconnect_reason_message_.clear();
    disconnected_at_ms_ = 0;
    pending_commands_.clear();
    command_results_.clear();
    replication_batches_.clear();
    replication_messages_.clear();
    state_ = ClientSessionState::connected;
    return core::Status::ok();
}

void ClientSession::mark_transport_disconnected(std::string reason_code,
                                                std::string reason_message,
                                                std::int64_t disconnected_at_ms) {
    disconnect_reason_code_ = std::move(reason_code);
    disconnect_reason_message_ = std::move(reason_message);
    disconnected_at_ms_ = disconnected_at_ms;
    pending_commands_.clear();
    state_ = ClientSessionState::disconnected;
}

core::Result<CommandEnvelope> ClientSession::create_command(std::string type, std::string payload,
                                                            std::int64_t client_time_ms) {
    auto connected = require_connected();
    if (!connected) {
        return core::Result<CommandEnvelope>::failure(connected.error().code,
                                                      connected.error().message);
    }

    CommandEnvelope command;
    command.sequence = next_command_sequence_;
    command.sender = transport_session_.client_id;
    command.type = std::move(type);
    command.payload = std::move(payload);
    command.client_time_ms = client_time_ms;

    auto validation = validate_transport_message(make_command_transport_message(command),
                                                 transport_session_.max_payload_bytes);
    if (!validation) {
        return core::Result<CommandEnvelope>::failure(validation.error().code,
                                                      validation.error().message);
    }

    auto [_, inserted] = pending_commands_.emplace(
        command.sequence,
        ClientPendingCommand{command.sequence, command.type, command.client_time_ms});
    if (!inserted) {
        return core::Result<CommandEnvelope>::failure(
            "client_session.duplicate_pending_command",
            "client session already has a pending command with the next sequence");
    }

    ++next_command_sequence_;
    return core::Result<CommandEnvelope>::success(std::move(command));
}

core::Status ClientSession::receive_server_message(const TransportEnvelope& envelope) {
    if (!is_connected() && envelope.message.kind == TransportMessageKind::control) {
        return accept_welcome(envelope);
    }

    auto connected = require_connected();
    if (!connected) {
        return connected;
    }
    auto server = validate_server_envelope(envelope);
    if (!server) {
        return server;
    }

    switch (envelope.message.kind) {
    case TransportMessageKind::command_result:
        return receive_command_result(envelope);
    case TransportMessageKind::replication:
        return receive_replication(envelope);
    case TransportMessageKind::control:
        if (envelope.message.payload_type == transport_server_disconnect_payload_type) {
            return receive_server_disconnect(envelope);
        }
        return core::Status::failure(
            "client_session.unexpected_control_message",
            "connected client session received an unexpected control message");
    case TransportMessageKind::command:
        return core::Status::failure("client_session.unexpected_command_message",
                                     "client session must not receive command messages from the "
                                     "server");
    }

    return core::Status::failure("client_session.unknown_message_kind",
                                 "client session received an unknown transport message kind");
}

std::vector<HostSessionCommandResult> ClientSession::drain_command_results() {
    return drain_vector(command_results_);
}

std::vector<ReplicationBatch> ClientSession::drain_replication_batches() {
    return drain_vector(replication_batches_);
}

std::vector<TransportEnvelope>
ClientSession::drain_replication_messages(std::string_view payload_type) {
    if (payload_type.empty()) {
        return drain_vector(replication_messages_);
    }

    std::vector<TransportEnvelope> drained;
    std::vector<TransportEnvelope> remaining;
    remaining.reserve(replication_messages_.size());
    for (auto& envelope : replication_messages_) {
        if (envelope.message.payload_type == payload_type) {
            drained.push_back(std::move(envelope));
        } else {
            remaining.push_back(std::move(envelope));
        }
    }
    replication_messages_ = std::move(remaining);
    return drained;
}

core::Status ClientSession::require_connected() const {
    if (!is_connected()) {
        return core::Status::failure("client_session.not_connected",
                                     "client session has not accepted a server welcome");
    }
    return core::Status::ok();
}

core::Status ClientSession::validate_server_envelope(const TransportEnvelope& envelope) const {
    if (envelope.sender != transport_session_.server_id) {
        return core::Status::failure("client_session.server_id_mismatch",
                                     "server message sender does not match accepted server id");
    }
    if (envelope.recipient != transport_session_.client_id) {
        return core::Status::failure("client_session.recipient_mismatch",
                                     "server message recipient does not match client id");
    }
    return core::Status::ok();
}

core::Status ClientSession::receive_command_result(const TransportEnvelope& envelope) {
    auto result = host_session_command_result_from_transport(envelope);
    if (!result) {
        return core::Status::failure(result.error().code, result.error().message);
    }

    const auto pending = pending_commands_.find(result.value().sequence);
    if (pending == pending_commands_.end()) {
        return core::Status::failure("client_session.unknown_command_result",
                                     "command result does not match any pending command");
    }
    if (pending->second.command_type != result.value().command_type) {
        return core::Status::failure("client_session.command_type_mismatch",
                                     "command result type does not match pending command type");
    }

    pending_commands_.erase(pending);
    command_results_.push_back(std::move(result).value());
    return core::Status::ok();
}

core::Status ClientSession::receive_replication(const TransportEnvelope& envelope) {
    if (envelope.message.payload_type != replication_world_events_payload_type) {
        replication_messages_.push_back(envelope);
        return core::Status::ok();
    }
    if (envelope.message.channel != TransportChannel::reliable) {
        return core::Status::failure(
            "client_session.unreliable_world_replication",
            "authoritative world replication batches require reliable delivery");
    }

    auto batch = replication_batch_from_transport(envelope);
    if (!batch) {
        return core::Status::failure(batch.error().code, batch.error().message);
    }
    const auto stream_sequence = replication_stream_sequence(batch.value());
    if (has_replication_sequence_ && stream_sequence <= last_replication_sequence_) {
        return core::Status::failure(
            "client_session.replayed_replication",
            "replication batch sequence must be greater than the last accepted replication "
            "sequence");
    }

    last_replication_sequence_ = stream_sequence;
    has_replication_sequence_ = true;
    replication_batches_.push_back(std::move(batch).value());
    return core::Status::ok();
}

core::Status ClientSession::receive_server_disconnect(const TransportEnvelope& envelope) {
    auto disconnect = accept_transport_server_disconnect(transport_session_.server_id,
                                                         transport_session_.client_id, envelope);
    if (!disconnect) {
        return core::Status::failure(disconnect.error().code, disconnect.error().message);
    }

    disconnect_reason_code_ = disconnect.value().reason_code;
    disconnect_reason_message_ = disconnect.value().reason_message;
    disconnected_at_ms_ = envelope.message.timestamp_ms;
    pending_commands_.clear();
    state_ = ClientSessionState::disconnected;
    return core::Status::ok();
}

std::string_view client_session_state_name(ClientSessionState state) noexcept {
    switch (state) {
    case ClientSessionState::disconnected:
        return "disconnected";
    case ClientSessionState::connected:
        return "connected";
    }
    return "unknown";
}

} // namespace heartstead::net
