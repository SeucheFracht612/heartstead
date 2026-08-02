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
    std::uint32_t max_outbound_bytes_per_client_per_second = 256u * 1024u;
    std::uint32_t max_pending_reliable_messages = 8'192;
    std::uint64_t max_pending_reliable_bytes = 64u * 1024u * 1024u;
    std::uint32_t max_pending_reliable_messages_per_client = 1'024;
    std::uint64_t max_pending_reliable_bytes_per_client = 8u * 1024u * 1024u;
    std::uint32_t max_reliable_delivery_messages_per_tick = 512;
    std::uint64_t max_reliable_delivery_bytes_per_tick = 1u * 1024u * 1024u;
    std::uint32_t max_reliable_delivery_messages_per_client_per_tick = 128;
    std::uint64_t max_reliable_delivery_bytes_per_client_per_tick = 256u * 1024u;

    [[nodiscard]] core::Status validate() const noexcept;
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
    std::size_t initial_pending_message_count = 0;
    std::uint64_t initial_pending_bytes = 0;
    std::uint32_t attempted_message_count = 0;
    std::uint64_t attempted_bytes = 0;
    std::uint32_t delivered_message_count = 0;
    std::uint64_t delivered_bytes = 0;
    std::uint32_t retry_attempt_count = 0;
    std::uint32_t failed_attempt_count = 0;
    std::size_t pending_message_count = 0;
    std::uint64_t pending_bytes = 0;
    std::uint32_t blocked_client_count = 0;
    std::uint32_t budget_deferred_message_count = 0;
    std::uint32_t tick_budget_deferred_message_count = 0;
    std::uint32_t overload_disconnected_client_count = 0;
    std::vector<core::NetId> overload_disconnected_clients;
    std::vector<HostSessionOutboundDeliveryFailure> failures;
};

struct HostSessionTickResult {
    std::uint32_t transport_retransmission_count = 0;
    std::uint32_t transport_dropped_reliable_message_count = 0;
    std::uint32_t transport_malformed_datagram_count = 0;
    std::uint32_t transport_rejected_datagram_count = 0;
    std::uint32_t transport_rate_limited_datagram_count = 0;
    std::uint64_t transport_client_to_server_bytes = 0;
    std::uint64_t transport_server_to_client_bytes = 0;
    std::uint32_t transport_client_to_server_message_count = 0;
    std::uint32_t transport_server_to_client_message_count = 0;
    std::uint32_t transport_simulated_dropped_unreliable_message_count = 0;
    std::uint32_t transport_pending_impaired_message_count = 0;
    std::uint32_t outbound_budget_dropped_unreliable_message_count = 0;
    std::uint32_t discarded_disconnected_message_count = 0;
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
    [[nodiscard]] std::size_t pending_outbound_message_count(core::NetId client_id) const noexcept;
    [[nodiscard]] std::uint64_t pending_outbound_bytes() const noexcept;
    [[nodiscard]] std::uint64_t pending_outbound_bytes(core::NetId client_id) const noexcept;
    [[nodiscard]] const ReplicationRelevancePolicy& replication_relevance_policy() const noexcept;

    [[nodiscard]] core::Status start();
    [[nodiscard]] core::Status stop();
    void set_replication_relevance_policy(ReplicationRelevancePolicy policy);

    [[nodiscard]] core::Result<core::NetId> connect_client();
    [[nodiscard]] core::Status disconnect_client(core::NetId client_id);
    [[nodiscard]] core::Status send_client_command(core::NetId client_id, CommandEnvelope envelope);
    [[nodiscard]] core::Status send_client_control(core::NetId client_id, TransportMessage message);
    [[nodiscard]] core::Status send_replication_message(core::NetId client_id,
                                                        TransportMessage message);
    [[nodiscard]] core::Result<std::vector<TransportEnvelope>>
    drain_client_messages(core::NetId client_id);

    [[nodiscard]] core::Result<HostSessionTickResult>
    tick(const ServerCommandDispatcher& dispatcher, CommandExecutionContext context);
    [[nodiscard]] core::Status flush_outbound(HostSessionTickResult& tick_result);
    [[nodiscard]] core::Status
    flush_local_client_bootstrap(core::NetId client_id,
                                 HostSessionOutboundDeliveryReport& delivery_report);

  private:
    struct PendingOutboundMessage {
        TransportMessage message;
        std::uint64_t wire_bytes = 0;
        std::uint64_t attempt_count = 0;
    };

    struct PendingOutboundQueue {
        std::deque<PendingOutboundMessage> messages;
        std::uint64_t wire_bytes = 0;
    };

    struct OutboundBudgetWindow {
        std::int64_t started_ms = 0;
        std::uint64_t used_bytes = 0;
        bool initialized = false;
    };

    [[nodiscard]] core::Status require_running() const;
    [[nodiscard]] core::Status send_welcome(core::NetId client_id, std::int64_t server_time_ms);
    [[nodiscard]] core::Status assign_replication_sequence(HostSessionCommandReport& report);
    [[nodiscard]] core::Status queue_command_response(const HostSessionCommandReport& report);
    [[nodiscard]] core::Result<ReplicationRelevanceReport>
    queue_replication(const HostSessionCommandReport& report, std::int64_t server_time_ms,
                      std::uint32_t& queued_message_count);
    void flush_pending_outbound(HostSessionOutboundDeliveryReport& report);
    [[nodiscard]] core::Status queue_reliable_message(core::NetId client_id,
                                                      TransportMessage message,
                                                      std::uint64_t attempt_count = 0);
    [[nodiscard]] core::Status disconnect_for_reliable_overload(core::NetId client_id);
    void clear_pending_outbound(core::NetId client_id) noexcept;
    void publish_overload_disconnects(HostSessionTickResult& result);
    [[nodiscard]] bool admit_outbound(core::NetId client_id, std::uint64_t wire_bytes);
    [[nodiscard]] std::size_t outbound_wire_bytes(core::NetId client_id,
                                                  const TransportMessage& message) const;

    HostSessionConfig config_;
    HostSessionTransportFactory transport_factory_;
    HostSessionState state_ = HostSessionState::stopped;
    std::unique_ptr<ITransportHost> transport_;
    std::map<core::NetId, PendingOutboundQueue> pending_outbound_;
    std::size_t pending_outbound_message_count_ = 0;
    std::uint64_t pending_outbound_bytes_ = 0;
    std::map<core::NetId, OutboundBudgetWindow> outbound_budget_windows_;
    std::map<core::NetId, std::uint32_t> reliable_delivery_attempts_by_client_;
    std::map<core::NetId, std::uint64_t> reliable_delivery_bytes_by_client_;
    std::vector<core::NetId> reliable_delivery_blocked_clients_;
    std::vector<core::NetId> reliable_delivery_tick_limited_clients_;
    std::vector<core::NetId> pending_overload_disconnects_;
    std::vector<core::NetId> pending_local_bootstrap_clients_;
    std::uint64_t reliable_delivery_client_cursor_ = 0;
    std::uint64_t current_reliable_delivery_rotation_ = 0;
    std::uint64_t next_replication_sequence_ = 1;
    std::int64_t current_server_time_ms_ = 0;
    std::uint32_t pending_budget_dropped_unreliable_message_count_ = 0;
};

class HostSessionCommandResultTextCodec {
  public:
    [[nodiscard]] static std::string encode(const HostSessionCommandResult& result);
    [[nodiscard]] static core::Result<HostSessionCommandResult> decode(std::string_view text);
};

class HostSessionCommandResultBinaryCodec {
  public:
    [[nodiscard]] static std::string encode(const HostSessionCommandResult& result);
    [[nodiscard]] static core::Result<HostSessionCommandResult> decode(std::string_view bytes);
    [[nodiscard]] static bool is_encoded(std::string_view bytes) noexcept;
};

[[nodiscard]] std::string_view host_session_state_name(HostSessionState state) noexcept;
[[nodiscard]] core::Status validate_host_session_outbound_delivery_report(
    const HostSessionOutboundDeliveryReport& report) noexcept;
[[nodiscard]] core::Status
validate_host_session_command_result(const HostSessionCommandResult& result) noexcept;
[[nodiscard]] HostSessionCommandResult
host_session_command_result_from_report(const HostSessionCommandReport& report);
[[nodiscard]] std::string host_session_result_payload(const HostSessionCommandReport& report);
[[nodiscard]] core::Result<HostSessionCommandResult>
host_session_command_result_from_transport(const TransportEnvelope& envelope);

} // namespace heartstead::net
