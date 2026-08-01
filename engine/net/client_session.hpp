#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/net/host_session.hpp"
#include "engine/net/replication.hpp"
#include "engine/net/server_command.hpp"
#include "engine/net/transport.hpp"
#include "engine/net/transport_control.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::net {

enum class ClientSessionState {
    disconnected,
    connected,
};

struct ClientPendingCommand {
    std::uint64_t sequence = 0;
    std::string command_type;
    std::int64_t client_time_ms = 0;
};

struct ClientSessionStats {
    std::size_t pending_command_count = 0;
    std::size_t queued_result_count = 0;
    std::size_t queued_replication_batch_count = 0;
    std::size_t queued_replication_event_count = 0;
    std::size_t queued_replication_message_count = 0;
    std::string first_queued_replication_message_payload_type;
    bool has_replication_sequence = false;
    std::uint64_t last_replication_sequence = 0;
    std::string disconnect_reason_code;
    std::string disconnect_reason_message;
    std::int64_t disconnected_at_ms = 0;
};

class ClientSession {
  public:
    explicit ClientSession(core::NetId expected_client_id = {});

    [[nodiscard]] ClientSessionState state() const noexcept;
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] core::NetId expected_client_id() const noexcept;
    [[nodiscard]] core::NetId client_id() const noexcept;
    [[nodiscard]] core::NetId server_id() const noexcept;
    [[nodiscard]] std::uint64_t next_command_sequence() const noexcept;
    [[nodiscard]] const TransportClientSession* transport_session() const noexcept;
    [[nodiscard]] ClientSessionStats stats() const;
    [[nodiscard]] ReplicationIntakeReport replication_intake_report() const;

    [[nodiscard]] core::Status accept_welcome(const TransportEnvelope& envelope);
    void mark_transport_disconnected(std::string reason_code, std::string reason_message,
                                     std::int64_t disconnected_at_ms);
    [[nodiscard]] core::Result<CommandEnvelope>
    create_command(std::string type, std::string payload, std::int64_t client_time_ms = 0);
    [[nodiscard]] core::Status receive_server_message(const TransportEnvelope& envelope);

    [[nodiscard]] std::vector<HostSessionCommandResult> drain_command_results();
    [[nodiscard]] std::vector<ReplicationBatch> drain_replication_batches();
    [[nodiscard]] std::vector<TransportEnvelope>
    drain_replication_messages(std::string_view payload_type = {},
                               std::size_t maximum_count = std::numeric_limits<std::size_t>::max());

  private:
    [[nodiscard]] core::Status require_connected() const;
    [[nodiscard]] core::Status validate_server_envelope(const TransportEnvelope& envelope) const;
    [[nodiscard]] core::Status receive_command_result(const TransportEnvelope& envelope);
    [[nodiscard]] core::Status receive_replication(const TransportEnvelope& envelope);
    [[nodiscard]] core::Status receive_server_disconnect(const TransportEnvelope& envelope);

    core::NetId expected_client_id_;
    TransportClientSession transport_session_;
    ClientSessionState state_ = ClientSessionState::disconnected;
    std::uint64_t next_command_sequence_ = 1;
    bool has_replication_sequence_ = false;
    std::uint64_t last_replication_sequence_ = 0;
    std::string disconnect_reason_code_;
    std::string disconnect_reason_message_;
    std::int64_t disconnected_at_ms_ = 0;
    std::unordered_map<std::uint64_t, ClientPendingCommand> pending_commands_;
    std::vector<HostSessionCommandResult> command_results_;
    std::vector<ReplicationBatch> replication_batches_;
    std::vector<TransportEnvelope> replication_messages_;
};

[[nodiscard]] std::string_view client_session_state_name(ClientSessionState state) noexcept;

} // namespace heartstead::net
