#pragma once

#include "engine/core/result.hpp"
#include "engine/net/transport.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::net {

enum class TransportClientState {
    disconnected,
    connecting,
    connected,
};

struct ExternalTransportClientConfig {
    TransportEndpoint server_endpoint{"127.0.0.1", 7777};
    TransportEndpoint bind_endpoint{"0.0.0.0", 0};
    std::uint32_t max_payload_bytes = 64u * 1024u;
    std::string content_fingerprint;
    TransportReliabilityConfig reliability{};
    std::uint32_t handshake_retry_ms = 250;
    std::uint32_t handshake_timeout_ms = 5'000;
    std::uint32_t idle_timeout_ms = 15'000;
    std::uint32_t keepalive_interval_ms = 1'000;
};

struct TransportClientMaintenanceResult {
    std::uint32_t retransmission_count = 0;
    std::uint32_t dropped_reliable_message_count = 0;
    std::uint32_t handshake_retransmission_count = 0;
    bool connected = false;
    bool disconnected = false;
    std::string disconnect_reason_code;
};

class ITransportClient {
  public:
    virtual ~ITransportClient() = default;

    [[nodiscard]] virtual TransportBackend backend() const noexcept = 0;
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
    [[nodiscard]] virtual TransportClientState state() const noexcept = 0;
    [[nodiscard]] virtual core::NetId server_id() const noexcept = 0;
    [[nodiscard]] virtual core::NetId client_id() const noexcept = 0;
    [[nodiscard]] virtual TransportSessionToken session_token() const noexcept = 0;

    [[nodiscard]] virtual core::Status connect(std::int64_t now_ms) = 0;
    [[nodiscard]] virtual core::Status disconnect(std::int64_t now_ms) = 0;
    [[nodiscard]] virtual core::Status send_to_server(TransportMessage message) = 0;
    [[nodiscard]] virtual core::Result<TransportClientMaintenanceResult>
    poll_maintenance(std::int64_t now_ms) = 0;
    [[nodiscard]] virtual std::vector<TransportEnvelope> drain_server_messages() = 0;
};

[[nodiscard]] core::Result<std::unique_ptr<ITransportClient>>
create_external_transport_client(ExternalTransportClientConfig config);
[[nodiscard]] core::Status
validate_external_transport_client_config(const ExternalTransportClientConfig& config);
[[nodiscard]] std::string_view transport_client_state_name(TransportClientState state) noexcept;

} // namespace heartstead::net
