#pragma once

#include "engine/net/replication.hpp"
#include "engine/net/server_command.hpp"
#include "engine/net/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace heartstead::net {

enum class HostSessionState {
    stopped,
    running,
};

struct HostSessionConfig {
    TransportHostDesc transport;
    ReplicationRelevancePolicy replication_relevance;
};

struct HostSessionCommandReport {
    core::NetId client_id;
    std::uint64_t sequence = 0;
    std::string command_type;
    bool success = false;
    bool committed_world_mutation = false;
    std::vector<world::OperationEvent> events;
    std::vector<core::SaveId> reserved_ids;
    CommandOperationTrace operation_trace;
    std::string error_code;
    std::string error_message;
    std::uint64_t replication_sequence = 0;
};

struct HostSessionCommandResult {
    std::uint64_t sequence = 0;
    std::string command_type;
    bool success = false;
    bool committed_world_mutation = false;
    std::uint32_t event_count = 0;
    std::uint32_t reserved_id_count = 0;
    std::string error_code;
    std::string error_message;
};

struct HostSessionOutboundDeliveryFailure {
    core::NetId client_id;
    TransportMessageKind message_kind = TransportMessageKind::command;
    std::uint64_t sequence = 0;
    std::uint64_t attempt_count = 0;
    std::string error_code;
    std::string error_message;
};

struct HostSessionOutboundDeliveryReport {
    std::uint32_t attempted_message_count = 0;
    std::uint32_t delivered_message_count = 0;
    std::uint32_t retry_attempt_count = 0;
    std::uint32_t failed_attempt_count = 0;
    std::size_t pending_message_count = 0;
    std::uint32_t blocked_client_count = 0;
    std::vector<HostSessionOutboundDeliveryFailure> failures;
};

struct HostSessionTickResult {
    std::uint32_t transport_retransmission_count = 0;
    std::uint32_t transport_dropped_reliable_message_count = 0;
    std::uint32_t transport_malformed_datagram_count = 0;
    std::uint32_t transport_rejected_datagram_count = 0;
    std::uint32_t transport_rate_limited_datagram_count = 0;
    std::uint32_t transport_message_count = 0;
    std::uint32_t command_message_count = 0;
    std::uint32_t control_message_count = 0;
    std::uint32_t response_message_count = 0;
    std::uint32_t replication_message_count = 0;
    std::vector<core::NetId> connected_clients;
    std::vector<core::NetId> disconnected_clients;
    HostSessionOutboundDeliveryReport outbound_delivery;
    std::vector<TransportEnvelope> control_messages;
    std::vector<HostSessionCommandReport> command_reports;
    std::vector<ReplicationRelevanceReport> replication_relevance_reports;
};

using HostSessionTransportFactory =
    std::function<core::Result<std::unique_ptr<ITransportHost>>(TransportHostDesc)>;

class HostSession {
  public:
    explicit HostSession(HostSessionConfig config = {});
    HostSession(HostSessionConfig config, HostSessionTransportFactory transport_factory);

    [[nodiscard]] HostSessionState state() const noexcept;
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] core::NetId server_id() const noexcept;
    [[nodiscard]] std::optional<TransportEndpoint> local_endpoint() const;
    [[nodiscard]] std::size_t connected_client_count() const noexcept;
    [[nodiscard]] std::size_t pending_outbound_message_count() const noexcept;
    [[nodiscard]] const ReplicationRelevancePolicy& replication_relevance_policy() const noexcept;

    [[nodiscard]] core::Status start();
    [[nodiscard]] core::Status stop();
    void set_replication_relevance_policy(ReplicationRelevancePolicy policy);

    [[nodiscard]] core::Result<core::NetId> connect_client();
    [[nodiscard]] core::Status disconnect_client(core::NetId client_id);
    [[nodiscard]] core::Status send_client_command(core::NetId client_id, CommandEnvelope envelope);
    [[nodiscard]] core::Status send_client_control(core::NetId client_id,
                                                   TransportMessage message);
    [[nodiscard]] core::Status send_replication_message(core::NetId client_id,
                                                        TransportMessage message);
    [[nodiscard]] core::Result<std::vector<TransportEnvelope>>
    drain_client_messages(core::NetId client_id);

    [[nodiscard]] core::Result<HostSessionTickResult>
    tick(const ServerCommandDispatcher& dispatcher, CommandExecutionContext context);

  private:
    struct PendingOutboundMessage {
        TransportMessage message;
        std::uint64_t attempt_count = 0;
    };

    [[nodiscard]] core::Status require_running() const;
    [[nodiscard]] core::Status send_welcome(core::NetId client_id,
                                            std::int64_t server_time_ms);
    [[nodiscard]] core::Status assign_replication_sequence(HostSessionCommandReport& report);
    void queue_command_response(const HostSessionCommandReport& report);
    [[nodiscard]] ReplicationRelevanceReport
    queue_replication(const HostSessionCommandReport& report, std::int64_t server_time_ms,
                      std::uint32_t& queued_message_count);
    void flush_pending_outbound(HostSessionOutboundDeliveryReport& report);

    HostSessionConfig config_;
    HostSessionTransportFactory transport_factory_;
    HostSessionState state_ = HostSessionState::stopped;
    std::unique_ptr<ITransportHost> transport_;
    std::map<core::NetId, std::deque<PendingOutboundMessage>> pending_outbound_;
    std::uint64_t next_replication_sequence_ = 1;
};

class HostSessionCommandResultTextCodec {
  public:
    [[nodiscard]] static std::string encode(const HostSessionCommandResult& result);
    [[nodiscard]] static core::Result<HostSessionCommandResult> decode(std::string_view text);
};

[[nodiscard]] std::string_view host_session_state_name(HostSessionState state) noexcept;
[[nodiscard]] core::Status
validate_host_session_command_result(const HostSessionCommandResult& result) noexcept;
[[nodiscard]] HostSessionCommandResult
host_session_command_result_from_report(const HostSessionCommandReport& report);
[[nodiscard]] std::string host_session_result_payload(const HostSessionCommandReport& report);
[[nodiscard]] core::Result<HostSessionCommandResult>
host_session_command_result_from_transport(const TransportEnvelope& envelope);

} // namespace heartstead::net
