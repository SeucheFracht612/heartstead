#include "engine/net/transport_client.hpp"

#include "engine/net/transport_handshake.hpp"
#include "engine/net/transport_packet.hpp"
#include "engine/net/transport_reliability.hpp"

#if defined(__unix__) || defined(__APPLE__)
#define HEARTSTEAD_HAS_POSIX_DATAGRAM_CLIENT 1
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#define HEARTSTEAD_HAS_POSIX_DATAGRAM_CLIENT 0
#endif

#include <array>
#include <optional>
#include <utility>

namespace heartstead::net {

namespace {

#if HEARTSTEAD_HAS_POSIX_DATAGRAM_CLIENT

[[nodiscard]] std::string socket_error_message(std::string_view action) {
    return std::string(action) + ": " + std::strerror(errno);
}

class ClientDatagramSocket final {
  public:
    ClientDatagramSocket() = default;
    explicit ClientDatagramSocket(int fd) noexcept : fd_(fd) {}
    ~ClientDatagramSocket() {
        close();
    }
    ClientDatagramSocket(const ClientDatagramSocket&) = delete;
    ClientDatagramSocket& operator=(const ClientDatagramSocket&) = delete;
    ClientDatagramSocket(ClientDatagramSocket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }
    ClientDatagramSocket& operator=(ClientDatagramSocket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    [[nodiscard]] int fd() const noexcept {
        return fd_;
    }

  private:
    void close() noexcept {
        if (fd_ >= 0) {
            (void)::close(fd_);
            fd_ = -1;
        }
    }
    int fd_ = -1;
};

struct SocketAddress {
    sockaddr_storage storage{};
    socklen_t size = 0;

    [[nodiscard]] int family() const noexcept {
        return storage.ss_family;
    }
};

[[nodiscard]] core::Result<SocketAddress>
numeric_address(const TransportEndpoint& endpoint) {
    SocketAddress result;
    sockaddr_in ipv4{};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(endpoint.port);
    if (::inet_pton(AF_INET, endpoint.address.c_str(), &ipv4.sin_addr) == 1) {
        std::memcpy(&result.storage, &ipv4, sizeof(ipv4));
        result.size = sizeof(ipv4);
        return core::Result<SocketAddress>::success(result);
    }

    sockaddr_in6 ipv6{};
    ipv6.sin6_family = AF_INET6;
    ipv6.sin6_port = htons(endpoint.port);
    if (::inet_pton(AF_INET6, endpoint.address.c_str(), &ipv6.sin6_addr) == 1) {
        std::memcpy(&result.storage, &ipv6, sizeof(ipv6));
        result.size = sizeof(ipv6);
        return core::Result<SocketAddress>::success(result);
    }
    return core::Result<SocketAddress>::failure(
        "transport.invalid_endpoint_address",
        "POSIX UDP client requires a numeric IPv4 or IPv6 endpoint");
}

[[nodiscard]] std::uint16_t port_from_address(const SocketAddress& address) noexcept {
    if (address.family() == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&address.storage)->sin6_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in*>(&address.storage)->sin_port);
}

[[nodiscard]] bool same_endpoint(const SocketAddress& left,
                                 const SocketAddress& right) noexcept {
    if (left.family() != right.family() ||
        port_from_address(left) != port_from_address(right)) {
        return false;
    }
    if (left.family() == AF_INET6) {
        const auto* lhs = reinterpret_cast<const sockaddr_in6*>(&left.storage);
        const auto* rhs = reinterpret_cast<const sockaddr_in6*>(&right.storage);
        return lhs->sin6_scope_id == rhs->sin6_scope_id &&
               std::memcmp(&lhs->sin6_addr, &rhs->sin6_addr, sizeof(in6_addr)) == 0;
    }
    const auto* lhs = reinterpret_cast<const sockaddr_in*>(&left.storage);
    const auto* rhs = reinterpret_cast<const sockaddr_in*>(&right.storage);
    return lhs->sin_addr.s_addr == rhs->sin_addr.s_addr;
}

[[nodiscard]] bool would_block() noexcept {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

class PosixDatagramTransportClient final : public ITransportClient {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<ITransportClient>>
    create(ExternalTransportClientConfig config) {
        auto status = validate_external_transport_client_config(config);
        if (!status) {
            return core::Result<std::unique_ptr<ITransportClient>>::failure(
                status.error().code, status.error().message);
        }
        auto server_address = numeric_address(config.server_endpoint);
        if (!server_address) {
            return core::Result<std::unique_ptr<ITransportClient>>::failure(
                server_address.error().code, server_address.error().message);
        }
        if (server_address.value().family() == AF_INET6 &&
            config.bind_endpoint.address == "0.0.0.0") {
            config.bind_endpoint.address = "::";
        }
        auto bind_address = numeric_address(config.bind_endpoint);
        if (!bind_address) {
            return core::Result<std::unique_ptr<ITransportClient>>::failure(
                bind_address.error().code, bind_address.error().message);
        }
        if (bind_address.value().family() != server_address.value().family()) {
            return core::Result<std::unique_ptr<ITransportClient>>::failure(
                "transport.address_family_mismatch",
                "client bind and server endpoints must use the same address family");
        }
        const auto fd = ::socket(server_address.value().family(), SOCK_DGRAM, 0);
        if (fd < 0) {
            return core::Result<std::unique_ptr<ITransportClient>>::failure(
                "transport.socket_failed",
                socket_error_message("failed to create remote UDP client socket"));
        }
        ClientDatagramSocket socket(fd);
        const auto flags = ::fcntl(socket.fd(), F_GETFL, 0);
        if (flags < 0 || ::fcntl(socket.fd(), F_SETFL, flags | O_NONBLOCK) < 0) {
            return core::Result<std::unique_ptr<ITransportClient>>::failure(
                "transport.socket_nonblocking_failed",
                socket_error_message("failed to configure remote UDP client socket"));
        }
        if (::bind(socket.fd(),
                   reinterpret_cast<const sockaddr*>(&bind_address.value().storage),
                   bind_address.value().size) != 0) {
            return core::Result<std::unique_ptr<ITransportClient>>::failure(
                "transport.client_bind_failed",
                socket_error_message("failed to bind remote UDP client socket"));
        }
        auto nonce = secure_random_transport_token();
        if (!nonce) {
            return core::Result<std::unique_ptr<ITransportClient>>::failure(
                nonce.error().code, nonce.error().message);
        }
        auto client = std::unique_ptr<ITransportClient>(new PosixDatagramTransportClient(
            std::move(config), std::move(socket), server_address.value(), nonce.value()));
        return core::Result<std::unique_ptr<ITransportClient>>::success(std::move(client));
    }

    [[nodiscard]] TransportBackend backend() const noexcept override {
        return TransportBackend::external_library;
    }
    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return transport_backend_name(TransportBackend::external_library);
    }
    [[nodiscard]] TransportClientState state() const noexcept override {
        return state_;
    }
    [[nodiscard]] core::NetId server_id() const noexcept override {
        return server_id_;
    }
    [[nodiscard]] core::NetId client_id() const noexcept override {
        return client_id_;
    }
    [[nodiscard]] TransportSessionToken session_token() const noexcept override {
        return session_token_;
    }

    [[nodiscard]] core::Status connect(std::int64_t now_ms) override {
        if (state_ != TransportClientState::disconnected) {
            return core::Status::failure("transport_client.already_started",
                                         "remote client is already connecting or connected");
        }
        state_ = TransportClientState::connecting;
        connect_started_ms_ = now_ms;
        last_received_ms_ = now_ms;
        return send_handshake(
            TransportHandshakePacket{
                TransportHandshakeKind::client_hello,
                transport_handshake_protocol_version,
                client_nonce_,
                {},
                0,
                {},
                {},
                {},
                config_.content_fingerprint,
                {},
            },
            now_ms);
    }

    [[nodiscard]] core::Status disconnect(std::int64_t now_ms) override {
        if (state_ == TransportClientState::disconnected) {
            return core::Status::ok();
        }
        if (state_ == TransportClientState::connected) {
            TransportMessage message{
                TransportMessageKind::control,
                TransportChannel::reliable,
                0,
                "control.client_disconnect",
                {},
                now_ms,
            };
            (void)send_to_server(std::move(message));
        }
        clear_connection();
        return core::Status::ok();
    }

    [[nodiscard]] core::Status send_to_server(TransportMessage message) override {
        if (state_ != TransportClientState::connected || !client_reliability_) {
            return core::Status::failure("transport_client.not_connected",
                                         "remote client is not connected");
        }
        auto status = validate_transport_message(message, config_.max_payload_bytes);
        if (!status) {
            return status;
        }
        TransportEnvelope envelope{client_id_, server_id_, std::move(message), session_token_};
        if (envelope.message.channel == TransportChannel::reliable) {
            status = client_reliability_->track_send(envelope, current_time_ms_);
            if (!status) {
                return status;
            }
        }
        status = send_envelope(envelope);
        if (!status && envelope.message.channel == TransportChannel::reliable) {
            (void)client_reliability_->rollback_tracked_send(envelope);
        }
        return status;
    }

    [[nodiscard]] core::Result<TransportClientMaintenanceResult>
    poll_maintenance(std::int64_t now_ms) override {
        current_time_ms_ = now_ms;
        TransportClientMaintenanceResult result;
        drain_socket(result);
        if (state_ == TransportClientState::connecting) {
            if (now_ms - connect_started_ms_ >=
                static_cast<std::int64_t>(config_.handshake_timeout_ms)) {
                result.disconnected = true;
                result.disconnect_reason_code = "transport_client.handshake_timeout";
                clear_connection();
                return core::Result<TransportClientMaintenanceResult>::success(
                    std::move(result));
            }
            if (now_ms - last_handshake_sent_ms_ >=
                static_cast<std::int64_t>(config_.handshake_retry_ms)) {
                auto status = send_datagram(last_handshake_datagram_);
                if (!status) {
                    return core::Result<TransportClientMaintenanceResult>::failure(
                        status.error().code, status.error().message);
                }
                last_handshake_sent_ms_ = now_ms;
                ++result.handshake_retransmission_count;
            }
        } else if (state_ == TransportClientState::connected && client_reliability_) {
            if (now_ms - last_received_ms_ >=
                static_cast<std::int64_t>(config_.idle_timeout_ms)) {
                result.disconnected = true;
                result.disconnect_reason_code = "transport_client.idle_timeout";
                clear_connection();
                return core::Result<TransportClientMaintenanceResult>::success(
                    std::move(result));
            }
            auto poll = client_reliability_->poll(now_ms);
            for (const auto& envelope : poll.retransmissions) {
                auto status = send_envelope(envelope);
                if (!status) {
                    return core::Result<TransportClientMaintenanceResult>::failure(
                        status.error().code, status.error().message);
                }
                ++result.retransmission_count;
            }
            result.dropped_reliable_message_count =
                static_cast<std::uint32_t>(poll.dropped.size());
            if (now_ms - last_sent_ms_ >=
                static_cast<std::int64_t>(config_.keepalive_interval_ms)) {
                auto status = send_to_server(
                    {TransportMessageKind::control, TransportChannel::unreliable, 0,
                     "control.keepalive", {}, now_ms});
                if (!status) {
                    return core::Result<TransportClientMaintenanceResult>::failure(
                        status.error().code, status.error().message);
                }
            }
        }
        result.connected = connected_this_poll_;
        connected_this_poll_ = false;
        return core::Result<TransportClientMaintenanceResult>::success(std::move(result));
    }

    [[nodiscard]] std::vector<TransportEnvelope> drain_server_messages() override {
        std::vector<TransportEnvelope> result;
        result.swap(server_messages_);
        return result;
    }

  private:
    PosixDatagramTransportClient(ExternalTransportClientConfig config,
                                 ClientDatagramSocket socket, SocketAddress server_address,
                                 TransportSessionToken client_nonce)
        : config_(std::move(config)), socket_(std::move(socket)),
          server_address_(server_address), client_nonce_(client_nonce),
          fragment_config_{1200, 1024u * 1024u, 1024, 16, 4u * 1024u * 1024u, 10'000},
          reassembler_(fragment_config_) {}

    [[nodiscard]] core::Status send_handshake(TransportHandshakePacket packet,
                                              std::int64_t now_ms) {
        auto encoded = TransportHandshakeCodec::encode(packet);
        if (!encoded) {
            return core::Status::failure(encoded.error().code, encoded.error().message);
        }
        auto status = send_datagram(encoded.value());
        if (!status) {
            return status;
        }
        last_handshake_datagram_ = std::move(encoded).value();
        last_handshake_sent_ms_ = now_ms;
        return core::Status::ok();
    }

    [[nodiscard]] core::Status send_datagram(std::string_view datagram) {
        const auto sent =
            ::sendto(socket_.fd(), datagram.data(), datagram.size(), 0,
                     reinterpret_cast<const sockaddr*>(&server_address_.storage),
                     server_address_.size);
        if (sent < 0 || static_cast<std::size_t>(sent) != datagram.size()) {
            return core::Status::failure(
                "transport.send_failed",
                socket_error_message("failed to send remote UDP client datagram"));
        }
        last_sent_ms_ = current_time_ms_;
        return core::Status::ok();
    }

    [[nodiscard]] core::Status send_envelope(TransportEnvelope envelope) {
        envelope.session_token = session_token_;
        const auto packet = TransportPacketCodec::encode(envelope);
        if (packet.size() <= 1200) {
            return send_datagram(packet);
        }
        auto fragments = TransportPacketFragmentCodec::fragment_packet(
            packet, next_packet_id_++, fragment_config_);
        if (!fragments) {
            return core::Status::failure(fragments.error().code, fragments.error().message);
        }
        for (const auto& fragment : fragments.value()) {
            auto status = send_datagram(TransportPacketFragmentCodec::encode(fragment));
            if (!status) {
                return status;
            }
        }
        return core::Status::ok();
    }

    void drain_socket(TransportClientMaintenanceResult& result) {
        std::array<char, 64u * 1024u> buffer{};
        for (;;) {
            SocketAddress remote;
            remote.size = sizeof(remote.storage);
            const auto received =
                ::recvfrom(socket_.fd(), buffer.data(), buffer.size(), 0,
                           reinterpret_cast<sockaddr*>(&remote.storage), &remote.size);
            if (received < 0) {
                if (would_block()) {
                    return;
                }
                return;
            }
            if (received == 0 || !same_endpoint(remote, server_address_)) {
                continue;
            }
            const auto datagram =
                std::string_view(buffer.data(), static_cast<std::size_t>(received));
            if (state_ == TransportClientState::connecting) {
                auto handshake = TransportHandshakeCodec::decode(datagram);
                if (handshake) {
                    accept_handshake(handshake.value(), result);
                    continue;
                }
            }
            if (state_ != TransportClientState::connected || !client_reliability_) {
                continue;
            }
            auto envelope = decode_envelope(datagram);
            if (!envelope || envelope->sender != server_id_ ||
                envelope->recipient != client_id_ ||
                envelope->session_token != session_token_) {
                continue;
            }
            last_received_ms_ = current_time_ms_;
            if (envelope->message.payload_type == "control.keepalive_ack") {
                continue;
            }
            if (envelope->message.payload_type == transport_reliability_ack_payload_type) {
                (void)client_reliability_->accept_acknowledgement(*envelope);
                continue;
            }
            if (envelope->message.channel == TransportChannel::reliable) {
                auto accepted =
                    client_reliability_->accept_reliable_message(*envelope, current_time_ms_);
                if (!accepted) {
                    continue;
                }
                (void)send_envelope(accepted.value().acknowledgement);
                if (accepted.value().duplicate) {
                    continue;
                }
            }
            server_messages_.push_back(std::move(*envelope));
        }
    }

    void accept_handshake(const TransportHandshakePacket& packet,
                          TransportClientMaintenanceResult& result) {
        if (packet.client_nonce != client_nonce_) {
            return;
        }
        if (packet.kind == TransportHandshakeKind::server_reject) {
            result.disconnected = true;
            result.disconnect_reason_code = packet.reason_code;
            clear_connection();
            return;
        }
        if (packet.kind == TransportHandshakeKind::server_challenge) {
            (void)send_handshake(
                TransportHandshakePacket{
                    TransportHandshakeKind::client_response,
                    transport_handshake_protocol_version,
                    client_nonce_,
                    packet.cookie,
                    packet.cookie_time_bucket,
                    {},
                    {},
                    {},
                    config_.content_fingerprint,
                    {},
                },
                current_time_ms_);
            return;
        }
        if (packet.kind != TransportHandshakeKind::server_accept ||
            !packet.server_id.is_valid() || !packet.assigned_client_id.is_valid() ||
            !packet.session_token.is_valid()) {
            return;
        }
        server_id_ = packet.server_id;
        client_id_ = packet.assigned_client_id;
        session_token_ = packet.session_token;
        client_reliability_.emplace(client_id_, server_id_, config_.reliability);
        state_ = TransportClientState::connected;
        last_received_ms_ = current_time_ms_;
        connected_this_poll_ = true;
        result.connected = true;
    }

    [[nodiscard]] std::optional<TransportEnvelope>
    decode_envelope(std::string_view datagram) {
        auto packet = TransportPacketCodec::decode(
            datagram, TransportPacketCodecConfig{config_.max_payload_bytes});
        if (packet) {
            return std::move(packet).value();
        }
        auto fragment = TransportPacketFragmentCodec::decode(datagram, fragment_config_);
        if (!fragment) {
            return std::nullopt;
        }
        auto reassembled =
            reassembler_.accept_fragment(1, std::move(fragment).value(), current_time_ms_);
        if (!reassembled || !reassembled.value().complete) {
            return std::nullopt;
        }
        auto decoded = TransportPacketCodec::decode(
            reassembled.value().packet,
            TransportPacketCodecConfig{config_.max_payload_bytes});
        return decoded ? std::optional<TransportEnvelope>(std::move(decoded).value())
                       : std::nullopt;
    }

    void clear_connection() {
        state_ = TransportClientState::disconnected;
        server_id_ = {};
        client_id_ = {};
        session_token_ = {};
        client_reliability_.reset();
        reassembler_.clear();
        server_messages_.clear();
        last_handshake_datagram_.clear();
    }

    ExternalTransportClientConfig config_;
    ClientDatagramSocket socket_;
    SocketAddress server_address_{};
    TransportClientState state_ = TransportClientState::disconnected;
    TransportSessionToken client_nonce_;
    core::NetId server_id_;
    core::NetId client_id_;
    TransportSessionToken session_token_;
    std::optional<TransportReliabilityTracker> client_reliability_;
    TransportPacketFragmentCodecConfig fragment_config_;
    TransportPacketReassembler reassembler_;
    std::vector<TransportEnvelope> server_messages_;
    std::string last_handshake_datagram_;
    std::uint64_t next_packet_id_ = 1;
    std::int64_t connect_started_ms_ = 0;
    std::int64_t last_handshake_sent_ms_ = 0;
    std::int64_t last_received_ms_ = 0;
    std::int64_t last_sent_ms_ = 0;
    std::int64_t current_time_ms_ = 0;
    bool connected_this_poll_ = false;
};

#endif

} // namespace

core::Result<std::unique_ptr<ITransportClient>>
create_external_transport_client(ExternalTransportClientConfig config) {
#if HEARTSTEAD_HAS_POSIX_DATAGRAM_CLIENT
    return PosixDatagramTransportClient::create(std::move(config));
#else
    (void)config;
    return core::Result<std::unique_ptr<ITransportClient>>::failure(
        "transport.external_unavailable",
        "remote POSIX UDP client transport is unavailable on this platform");
#endif
}

core::Status
validate_external_transport_client_config(const ExternalTransportClientConfig& config) {
    auto status = validate_transport_endpoint(config.server_endpoint);
    if (!status || config.server_endpoint.port == 0) {
        return !status ? status
                       : core::Status::failure("transport_client.invalid_server_port",
                                               "remote server port must be non-zero");
    }
    status = validate_transport_endpoint(config.bind_endpoint);
    if (!status) {
        return status;
    }
    if (config.max_payload_bytes == 0 || config.content_fingerprint.size() > 192) {
        return core::Status::failure(
            "transport_client.invalid_config",
            "remote client payload budget or content fingerprint is invalid");
    }
    status = validate_transport_reliability_config(config.reliability);
    if (!status) {
        return status;
    }
    if (config.handshake_retry_ms == 0 || config.handshake_timeout_ms == 0 ||
        config.handshake_retry_ms >= config.handshake_timeout_ms ||
        config.idle_timeout_ms == 0 || config.keepalive_interval_ms == 0 ||
        config.keepalive_interval_ms >= config.idle_timeout_ms) {
        return core::Status::failure(
            "transport_client.invalid_timeouts",
            "remote client handshake, keepalive, or idle timeout is invalid");
    }
    return core::Status::ok();
}

std::string_view transport_client_state_name(TransportClientState state) noexcept {
    switch (state) {
    case TransportClientState::disconnected:
        return "disconnected";
    case TransportClientState::connecting:
        return "connecting";
    case TransportClientState::connected:
        return "connected";
    }
    return "unknown";
}

} // namespace heartstead::net
