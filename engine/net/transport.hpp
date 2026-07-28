#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/net/server_command.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heartstead::net {

enum class TransportBackend {
    in_memory,
    external_library,
};

enum class TransportChannel {
    reliable,
    unreliable,
};

enum class TransportMessageKind {
    command,
    command_result,
    replication,
    control,
};

struct TransportBackendInfo {
    TransportBackend backend = TransportBackend::in_memory;
    std::string_view name;
    bool available = false;
    std::string_view status;
};

struct TransportEndpoint {
    std::string address = "127.0.0.1";
    std::uint16_t port = 7777;
};

struct TransportSessionToken {
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    [[nodiscard]] bool is_valid() const noexcept {
        return high != 0 || low != 0;
    }

    [[nodiscard]] bool operator==(const TransportSessionToken&) const noexcept = default;
};

struct TransportCapabilities {
    bool supports_reliable = true;
    bool supports_unreliable = true;
    bool supports_server_authoritative_host = true;
    bool enforces_reliable_command_order = false;
    std::uint32_t max_payload_bytes = 0;
    std::uint32_t max_clients = 0;
};

struct TransportReliabilityConfig {
    std::uint32_t retry_delay_ms = 250;
    std::uint32_t max_attempts = 5;
    std::uint32_t max_in_flight = 256;
    std::uint32_t max_tracked_received_messages = 1024;
};

struct TransportMessage {
    TransportMessageKind kind = TransportMessageKind::command;
    TransportChannel channel = TransportChannel::reliable;
    std::uint64_t sequence = 0;
    std::string payload_type;
    std::string payload;
    std::int64_t timestamp_ms = 0;
};

struct TransportEnvelope {
    TransportEnvelope() = default;
    TransportEnvelope(core::NetId sender_value, core::NetId recipient_value,
                      TransportMessage message_value, TransportSessionToken token_value = {})
        : sender(sender_value), recipient(recipient_value), message(std::move(message_value)),
          session_token(token_value) {}

    core::NetId sender;
    core::NetId recipient;
    TransportMessage message;
    TransportSessionToken session_token;
};

struct TransportMaintenanceResult {
    std::uint32_t retransmission_count = 0;
    std::uint32_t dropped_reliable_message_count = 0;
    std::uint32_t malformed_datagram_count = 0;
    std::uint32_t rejected_datagram_count = 0;
    std::uint32_t rate_limited_datagram_count = 0;
    std::uint64_t client_to_server_bytes = 0;
    std::uint64_t server_to_client_bytes = 0;
    std::uint32_t client_to_server_message_count = 0;
    std::uint32_t server_to_client_message_count = 0;
    std::uint32_t simulated_dropped_unreliable_message_count = 0;
    std::uint32_t pending_impaired_message_count = 0;
    std::vector<TransportEnvelope> dropped_reliable_messages;
    std::vector<core::NetId> connected_clients;
    std::vector<core::NetId> disconnected_clients;
};

struct InMemoryTransportHostConfig {
    core::NetId server_id = core::NetId::from_value(1);
    std::uint32_t max_payload_bytes = 64u * 1024u;
    std::uint32_t max_clients = 64;
    std::uint32_t simulated_one_way_latency_ms = 0;
    std::uint32_t simulated_jitter_ms = 0;
    std::uint32_t simulated_unreliable_loss_basis_points = 0;
    std::uint64_t impairment_seed = 0x6a09e667f3bcc909ULL;
};

struct ExternalTransportHostConfig {
    ExternalTransportHostConfig() = default;
    ExternalTransportHostConfig(core::NetId server_id_value, TransportEndpoint bind_endpoint_value,
                                std::uint32_t max_payload_bytes_value,
                                std::uint32_t max_clients_value,
                                bool enable_unreliable_channel_value)
        : server_id(server_id_value), bind_endpoint(std::move(bind_endpoint_value)),
          max_payload_bytes(max_payload_bytes_value), max_clients(max_clients_value),
          enable_unreliable_channel(enable_unreliable_channel_value) {}

    core::NetId server_id = core::NetId::from_value(1);
    TransportEndpoint bind_endpoint{"0.0.0.0", 7777};
    std::uint32_t max_payload_bytes = 64u * 1024u;
    std::uint32_t max_clients = 8;
    bool enable_unreliable_channel = true;
    TransportReliabilityConfig reliability{};
    std::string content_fingerprint;
    std::uint32_t handshake_timeout_ms = 5'000;
    std::uint32_t idle_timeout_ms = 15'000;
    std::uint32_t keepalive_interval_ms = 1'000;
    std::uint32_t max_inbound_messages_per_second = 240;
    std::uint32_t max_inbound_bytes_per_second = 512u * 1024u;
    std::uint32_t max_handshakes_per_second = 128;
};

struct TransportHostDesc {
    TransportBackend backend = TransportBackend::in_memory;
    InMemoryTransportHostConfig in_memory{};
    ExternalTransportHostConfig external{};
};

class ITransportHost {
  public:
    virtual ~ITransportHost() = default;

    [[nodiscard]] virtual TransportBackend backend() const noexcept = 0;
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
    [[nodiscard]] virtual TransportCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::optional<TransportEndpoint> local_endpoint() const {
        return std::nullopt;
    }
    [[nodiscard]] virtual core::NetId server_id() const noexcept = 0;
    [[nodiscard]] virtual std::size_t connected_client_count() const noexcept = 0;
    [[nodiscard]] virtual bool is_client_connected(core::NetId client_id) const noexcept = 0;
    [[nodiscard]] virtual std::vector<core::NetId> connected_client_ids() const = 0;

    [[nodiscard]] virtual core::Result<core::NetId> connect_client() = 0;
    [[nodiscard]] virtual core::Status disconnect_client(core::NetId client_id) = 0;

    [[nodiscard]] virtual core::Status send_client_to_server(core::NetId client_id,
                                                             TransportMessage message) = 0;
    [[nodiscard]] virtual core::Status send_server_to_client(core::NetId client_id,
                                                             TransportMessage message) = 0;

    [[nodiscard]] virtual core::Result<TransportMaintenanceResult>
    poll_maintenance(std::int64_t now_ms) = 0;
    [[nodiscard]] virtual std::vector<TransportEnvelope> drain_server_messages() = 0;
    [[nodiscard]] virtual core::Result<std::vector<TransportEnvelope>>
    drain_client_messages(core::NetId client_id) = 0;
};

class InMemoryTransportHost final : public ITransportHost {
  public:
    explicit InMemoryTransportHost(InMemoryTransportHostConfig config = {});

    [[nodiscard]] TransportBackend backend() const noexcept override;
    [[nodiscard]] std::string_view backend_name() const noexcept override;
    [[nodiscard]] TransportCapabilities capabilities() const noexcept override;
    [[nodiscard]] core::NetId server_id() const noexcept override;
    [[nodiscard]] std::size_t connected_client_count() const noexcept override;
    [[nodiscard]] bool is_client_connected(core::NetId client_id) const noexcept override;
    [[nodiscard]] std::vector<core::NetId> connected_client_ids() const override;

    [[nodiscard]] core::Result<core::NetId> connect_client() override;
    [[nodiscard]] core::Status disconnect_client(core::NetId client_id) override;

    [[nodiscard]] core::Status send_client_to_server(core::NetId client_id,
                                                     TransportMessage message) override;
    [[nodiscard]] core::Status send_server_to_client(core::NetId client_id,
                                                     TransportMessage message) override;

    [[nodiscard]] core::Result<TransportMaintenanceResult>
    poll_maintenance(std::int64_t now_ms) override;
    [[nodiscard]] std::vector<TransportEnvelope> drain_server_messages() override;
    [[nodiscard]] core::Result<std::vector<TransportEnvelope>>
    drain_client_messages(core::NetId client_id) override;

  private:
    struct ClientQueues {
        std::queue<TransportEnvelope> inbox;
        std::uint64_t last_reliable_command_sequence = 0;
        bool has_reliable_command_sequence = false;
        bool connected = true;
    };

    struct PendingDelivery {
        std::int64_t deliver_at_ms = 0;
        std::uint64_t order = 0;
        bool to_server = false;
        core::NetId client_id;
        TransportEnvelope envelope;
    };

    [[nodiscard]] core::Status validate_message(const TransportMessage& message) const;
    [[nodiscard]] core::Status
    validate_client_to_server_sequence(const ClientQueues& client,
                                       const TransportMessage& message) const;
    void record_client_to_server_sequence(ClientQueues& client, const TransportMessage& message);
    [[nodiscard]] core::Result<ClientQueues*> find_client(core::NetId client_id);
    [[nodiscard]] core::Result<ClientQueues*> find_connected_client(core::NetId client_id);
    [[nodiscard]] core::NetId next_client_id();
    void queue_or_deliver(TransportEnvelope envelope, bool to_server);
    void deliver_pending(std::int64_t now_ms);
    [[nodiscard]] bool impairment_enabled() const noexcept;
    [[nodiscard]] std::uint64_t next_impairment_value() noexcept;
    [[nodiscard]] std::size_t encoded_envelope_bytes(const TransportEnvelope& envelope) const;

    InMemoryTransportHostConfig config_;
    std::uint64_t next_client_id_ = 2;
    std::unordered_map<std::uint64_t, ClientQueues> clients_;
    std::queue<TransportEnvelope> server_inbox_;
    std::vector<PendingDelivery> pending_deliveries_;
    std::unordered_map<std::uint64_t, std::int64_t> last_reliable_client_delivery_ms_;
    std::int64_t last_reliable_server_delivery_ms_ = 0;
    std::int64_t current_time_ms_ = 0;
    std::uint64_t impairment_counter_ = 0;
    std::uint64_t delivery_order_ = 0;
    std::uint64_t pending_client_to_server_bytes_ = 0;
    std::uint64_t pending_server_to_client_bytes_ = 0;
    std::uint32_t pending_client_to_server_message_count_ = 0;
    std::uint32_t pending_server_to_client_message_count_ = 0;
    std::uint32_t pending_simulated_drop_count_ = 0;
};

[[nodiscard]] core::Result<std::unique_ptr<ITransportHost>>
create_transport_host(TransportHostDesc desc);
[[nodiscard]] core::Status validate_transport_host_desc(const TransportHostDesc& desc);
[[nodiscard]] core::Status
validate_transport_host_config(const InMemoryTransportHostConfig& config);
[[nodiscard]] core::Status
validate_external_transport_host_config(const ExternalTransportHostConfig& config);
[[nodiscard]] core::Status validate_transport_endpoint(const TransportEndpoint& endpoint);
[[nodiscard]] core::Result<TransportEndpoint>
parse_transport_endpoint(std::string_view endpoint, std::uint16_t default_port = 7777);
[[nodiscard]] core::Status validate_transport_message(const TransportMessage& message,
                                                      std::uint32_t max_payload_bytes);
[[nodiscard]] core::Result<TransportCapabilities>
transport_host_capabilities(const TransportHostDesc& desc);
[[nodiscard]] TransportCapabilities
transport_host_capabilities(const InMemoryTransportHostConfig& config) noexcept;
[[nodiscard]] TransportCapabilities
transport_host_capabilities(const ExternalTransportHostConfig& config) noexcept;
[[nodiscard]] core::NetId transport_host_server_id(const TransportHostDesc& desc) noexcept;

[[nodiscard]] TransportMessage
make_command_transport_message(const CommandEnvelope& envelope,
                               TransportChannel channel = TransportChannel::reliable);
[[nodiscard]] core::Result<CommandEnvelope>
command_envelope_from_transport(const TransportEnvelope& envelope);

[[nodiscard]] TransportBackendInfo transport_backend_info(TransportBackend backend) noexcept;
[[nodiscard]] std::string_view transport_backend_name(TransportBackend backend) noexcept;
[[nodiscard]] std::string_view transport_channel_name(TransportChannel channel) noexcept;
[[nodiscard]] std::string_view transport_message_kind_name(TransportMessageKind kind) noexcept;
[[nodiscard]] std::string transport_endpoint_name(const TransportEndpoint& endpoint);

} // namespace heartstead::net
