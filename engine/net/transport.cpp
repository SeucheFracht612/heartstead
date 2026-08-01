#include "engine/net/transport.hpp"

#include "engine/net/command_payload.hpp"
#include "engine/net/transport_handshake.hpp"
#include "engine/net/transport_packet.hpp"
#include "engine/net/transport_reliability.hpp"

#if defined(__unix__) || defined(__APPLE__)
#define HEARTSTEAD_HAS_POSIX_DATAGRAM_TRANSPORT 1
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#define HEARTSTEAD_HAS_POSIX_DATAGRAM_TRANSPORT 0
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <sstream>
#include <utility>

namespace heartstead::net {

namespace {

[[nodiscard]] bool is_valid_payload_type(std::string_view type) noexcept {
    if (type.empty() || type.front() == '.' || type.back() == '.') {
        return false;
    }

    for (const auto character : type) {
        const auto valid = (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') || character == '_' ||
                           character == '-' || character == '.';
        if (!valid) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_valid_endpoint_address(std::string_view address) noexcept {
    if (address.empty()) {
        return false;
    }

    for (const auto character : address) {
        if (character == '\n' || character == '\r' || character == '\t' || character == ' ') {
            return false;
        }
    }
    return true;
}

template <typename T> [[nodiscard]] std::vector<T> drain_queue(std::queue<T>& queue) {
    std::vector<T> result;
    result.reserve(queue.size());
    while (!queue.empty()) {
        result.push_back(std::move(queue.front()));
        queue.pop();
    }
    return result;
}

[[nodiscard]] bool is_reliability_ack_message(const TransportMessage& message) noexcept {
    return message.kind == TransportMessageKind::control &&
           message.payload_type == transport_reliability_ack_payload_type;
}

#if HEARTSTEAD_HAS_POSIX_DATAGRAM_TRANSPORT

[[nodiscard]] std::string socket_error_message(std::string_view action) {
    return std::string(action) + ": " + std::strerror(errno);
}

class PosixDatagramSocket final {
  public:
    PosixDatagramSocket() = default;
    explicit PosixDatagramSocket(int fd) noexcept : fd_(fd) {}

    ~PosixDatagramSocket() {
        close();
    }

    PosixDatagramSocket(const PosixDatagramSocket&) = delete;
    PosixDatagramSocket& operator=(const PosixDatagramSocket&) = delete;

    PosixDatagramSocket(PosixDatagramSocket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    PosixDatagramSocket& operator=(PosixDatagramSocket&& other) noexcept {
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

    [[nodiscard]] bool is_valid() const noexcept {
        return fd_ >= 0;
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

[[nodiscard]] core::Result<PosixDatagramSocket> create_datagram_socket(int family) {
    const auto fd = ::socket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
        return core::Result<PosixDatagramSocket>::failure(
            "transport.socket_failed", socket_error_message("failed to create UDP socket"));
    }

    PosixDatagramSocket socket(fd);
    const int enabled = 1;
    (void)::setsockopt(socket.fd(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    const auto flags = ::fcntl(socket.fd(), F_GETFL, 0);
    if (flags < 0 || ::fcntl(socket.fd(), F_SETFL, flags | O_NONBLOCK) < 0) {
        return core::Result<PosixDatagramSocket>::failure(
            "transport.socket_nonblocking_failed",
            socket_error_message("failed to configure UDP socket as nonblocking"));
    }
    return core::Result<PosixDatagramSocket>::success(std::move(socket));
}

[[nodiscard]] bool posix_datagram_transport_available() noexcept {
    const auto fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }
    (void)::close(fd);
    return true;
}

struct SocketAddress {
    sockaddr_storage storage{};
    socklen_t size = 0;

    [[nodiscard]] int family() const noexcept {
        return storage.ss_family;
    }
};

[[nodiscard]] core::Result<SocketAddress>
numeric_endpoint_address(const TransportEndpoint& endpoint) {
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
        "external transport requires a numeric IPv4 or IPv6 address");
}

[[nodiscard]] SocketAddress loopback_address(int family, std::uint16_t port) noexcept {
    SocketAddress result;
    if (family == AF_INET6) {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(port);
        address.sin6_addr = in6addr_loopback;
        std::memcpy(&result.storage, &address, sizeof(address));
        result.size = sizeof(address);
    } else {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        std::memcpy(&result.storage, &address, sizeof(address));
        result.size = sizeof(address);
    }
    return result;
}

[[nodiscard]] core::Result<SocketAddress> socket_bound_address(int fd) {
    SocketAddress address;
    address.size = sizeof(address.storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address.storage), &address.size) != 0) {
        return core::Result<SocketAddress>::failure(
            "transport.socket_address_failed",
            socket_error_message("failed to query UDP socket address"));
    }
    return core::Result<SocketAddress>::success(address);
}

[[nodiscard]] std::uint16_t port_from_address(const SocketAddress& address) noexcept {
    if (address.family() == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&address.storage)->sin6_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in*>(&address.storage)->sin_port);
}

[[nodiscard]] bool same_socket_endpoint(const SocketAddress& lhs,
                                        const SocketAddress& rhs) noexcept {
    if (lhs.family() != rhs.family() || port_from_address(lhs) != port_from_address(rhs)) {
        return false;
    }
    if (lhs.family() == AF_INET6) {
        const auto* left = reinterpret_cast<const sockaddr_in6*>(&lhs.storage);
        const auto* right = reinterpret_cast<const sockaddr_in6*>(&rhs.storage);
        return left->sin6_scope_id == right->sin6_scope_id &&
               std::memcmp(&left->sin6_addr, &right->sin6_addr, sizeof(in6_addr)) == 0;
    }
    const auto* left = reinterpret_cast<const sockaddr_in*>(&lhs.storage);
    const auto* right = reinterpret_cast<const sockaddr_in*>(&rhs.storage);
    return left->sin_addr.s_addr == right->sin_addr.s_addr;
}

[[nodiscard]] bool would_block() noexcept {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

[[nodiscard]] std::uint64_t fragment_source_scope(const SocketAddress& address) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&address.storage);
    for (socklen_t index = 0; index < address.size; ++index) {
        hash = (hash ^ bytes[index]) * prime;
    }
    return hash;
}

class PosixDatagramTransportHost final : public ITransportHost {
  private:
    struct ClientEndpoint {
        ClientEndpoint(std::optional<PosixDatagramSocket> socket_value, SocketAddress address_value,
                       TransportPacketFragmentCodecConfig fragment_config, core::NetId server_id,
                       core::NetId client_id, TransportReliabilityConfig reliability_config,
                       TransportSessionToken token_value, bool remote_value,
                       std::int64_t connected_at_ms)
            : socket(std::move(socket_value)), address(address_value), reassembler(fragment_config),
              server_reliability(server_id, client_id, reliability_config),
              client_reliability(client_id, server_id, reliability_config),
              server_command_sequencer(reliability_config.max_in_flight), token(token_value),
              remote(remote_value), last_received_ms(connected_at_ms),
              last_sent_ms(connected_at_ms), inbound_window_started_ms(connected_at_ms) {}

        std::optional<PosixDatagramSocket> socket;
        SocketAddress address{};
        TransportPacketReassembler reassembler;
        TransportReliabilityTracker server_reliability;
        TransportReliabilityTracker client_reliability;
        TransportReliableCommandSequencer server_command_sequencer;
        std::uint64_t last_sent_reliable_command_sequence = 0;
        bool has_sent_reliable_command_sequence = false;
        TransportSessionToken token;
        bool remote = false;
        std::int64_t last_received_ms = 0;
        std::int64_t last_sent_ms = 0;
        std::int64_t inbound_window_started_ms = 0;
        std::uint32_t inbound_message_count = 0;
        std::uint32_t inbound_byte_count = 0;
        bool connected = true;
    };

  public:
    [[nodiscard]] static core::Result<std::unique_ptr<ITransportHost>>
    create(ExternalTransportHostConfig config) {
        auto status = validate_external_transport_host_config(config);
        if (!status) {
            return core::Result<std::unique_ptr<ITransportHost>>::failure(status.error().code,
                                                                          status.error().message);
        }

        auto bind_address = numeric_endpoint_address(config.bind_endpoint);
        if (!bind_address) {
            return core::Result<std::unique_ptr<ITransportHost>>::failure(
                bind_address.error().code, bind_address.error().message);
        }
        auto server_socket = create_datagram_socket(bind_address.value().family());
        if (!server_socket) {
            return core::Result<std::unique_ptr<ITransportHost>>::failure(
                server_socket.error().code, server_socket.error().message);
        }
        if (::bind(server_socket.value().fd(),
                   reinterpret_cast<const sockaddr*>(&bind_address.value().storage),
                   bind_address.value().size) != 0) {
            return core::Result<std::unique_ptr<ITransportHost>>::failure(
                "transport.bind_failed",
                socket_error_message("failed to bind external transport endpoint " +
                                     transport_endpoint_name(config.bind_endpoint)));
        }

        auto actual_address = socket_bound_address(server_socket.value().fd());
        if (!actual_address) {
            return core::Result<std::unique_ptr<ITransportHost>>::failure(
                actual_address.error().code, actual_address.error().message);
        }

        auto security_secret = secure_random_transport_token();
        if (!security_secret) {
            return core::Result<std::unique_ptr<ITransportHost>>::failure(
                security_secret.error().code, security_secret.error().message);
        }
        auto host = std::unique_ptr<ITransportHost>(
            new PosixDatagramTransportHost(std::move(config), std::move(server_socket).value(),
                                           actual_address.value(), security_secret.value()));
        return core::Result<std::unique_ptr<ITransportHost>>::success(std::move(host));
    }

    [[nodiscard]] TransportBackend backend() const noexcept override {
        return TransportBackend::external_library;
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return transport_backend_name(TransportBackend::external_library);
    }

    [[nodiscard]] TransportCapabilities capabilities() const noexcept override {
        return transport_host_capabilities(config_);
    }

    [[nodiscard]] std::optional<TransportEndpoint> local_endpoint() const override {
        auto endpoint = config_.bind_endpoint;
        endpoint.port = port_from_address(server_bound_address_);
        return endpoint;
    }

    [[nodiscard]] core::NetId server_id() const noexcept override {
        return config_.server_id;
    }

    [[nodiscard]] std::size_t connected_client_count() const noexcept override {
        std::size_t count = 0;
        for (const auto& [_, client] : clients_) {
            if (client.connected) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] bool is_client_connected(core::NetId client_id) const noexcept override {
        const auto found = clients_.find(client_id.value());
        return found != clients_.end() && found->second.connected;
    }

    [[nodiscard]] std::vector<core::NetId> connected_client_ids() const override {
        std::vector<core::NetId> result;
        result.reserve(clients_.size());
        for (const auto& [id, client] : clients_) {
            if (client.connected) {
                result.push_back(core::NetId::from_value(id));
            }
        }
        std::ranges::sort(result);
        return result;
    }

    [[nodiscard]] core::Result<core::NetId> connect_client() override {
        if (connected_client_count() >= config_.max_clients) {
            return core::Result<core::NetId>::failure("transport.client_limit_reached",
                                                      "transport client limit has been reached");
        }

        auto client_socket = create_datagram_socket(server_bound_address_.family());
        if (!client_socket) {
            return core::Result<core::NetId>::failure(client_socket.error().code,
                                                      client_socket.error().message);
        }
        auto client_bind_address = loopback_address(server_bound_address_.family(), 0);
        if (::bind(client_socket.value().fd(),
                   reinterpret_cast<const sockaddr*>(&client_bind_address.storage),
                   client_bind_address.size) != 0) {
            return core::Result<core::NetId>::failure(
                "transport.client_bind_failed",
                socket_error_message("failed to bind external transport loopback client"));
        }
        auto client_address = socket_bound_address(client_socket.value().fd());
        if (!client_address) {
            return core::Result<core::NetId>::failure(client_address.error().code,
                                                      client_address.error().message);
        }

        const auto id = next_client_id();
        auto token = secure_random_transport_token();
        if (!token) {
            return core::Result<core::NetId>::failure(token.error().code, token.error().message);
        }
        ClientEndpoint endpoint{
            std::optional<PosixDatagramSocket>{std::move(client_socket).value()},
            client_address.value(),
            fragment_config_,
            config_.server_id,
            id,
            config_.reliability,
            token.value(),
            false,
            current_time_ms_,
        };
        clients_.emplace(id.value(), std::move(endpoint));
        return core::Result<core::NetId>::success(id);
    }

    [[nodiscard]] core::Status disconnect_client(core::NetId client_id) override {
        auto client = find_connected_client(client_id);
        if (!client) {
            return core::Status::failure(client.error().code, client.error().message);
        }
        server_reassembler_.discard_source(fragment_source_scope(client.value()->address));
        client.value()->reassembler.clear();
        client.value()->server_command_sequencer.clear();
        client.value()->connected = false;
        return core::Status::ok();
    }

    [[nodiscard]] core::Status send_client_to_server(core::NetId client_id,
                                                     TransportMessage message) override {
        auto client = find_connected_client(client_id);
        if (!client) {
            return core::Status::failure(client.error().code, client.error().message);
        }
        auto status = validate_transport_message(message, config_.max_payload_bytes);
        if (!status) {
            return status;
        }
        status = validate_local_client_send_sequence(*client.value(), message);
        if (!status) {
            return status;
        }

        if (!client.value()->socket.has_value()) {
            return core::Status::failure(
                "transport.remote_client_requires_endpoint",
                "remote clients must send through their independent transport endpoint");
        }
        TransportEnvelope envelope{client_id, config_.server_id, std::move(message),
                                   client.value()->token};
        if (envelope.message.channel == TransportChannel::reliable) {
            status = client.value()->client_reliability.track_send(envelope, current_time_ms_);
            if (!status) {
                return status;
            }
        }
        auto sent = send_envelope(client.value()->socket->fd(), server_address_for_local_clients_,
                                  envelope, client.value()->token);
        if (!sent) {
            if (envelope.message.channel == TransportChannel::reliable) {
                const auto rollback =
                    client.value()->client_reliability.rollback_tracked_send(envelope);
                if (!rollback) {
                    return core::Status::failure(
                        rollback.error().code,
                        sent.error().message +
                            "; tracking rollback failed: " + rollback.error().message);
                }
            }
            return sent;
        }
        record_local_client_send_sequence(*client.value(), envelope.message);
        return core::Status::ok();
    }

    [[nodiscard]] core::Status send_server_to_client(core::NetId client_id,
                                                     TransportMessage message) override {
        auto client = find_connected_client(client_id);
        if (!client) {
            return core::Status::failure(client.error().code, client.error().message);
        }
        auto status = validate_transport_message(message, config_.max_payload_bytes);
        if (!status) {
            return status;
        }
        TransportEnvelope envelope{config_.server_id, client_id, std::move(message),
                                   client.value()->token};
        if (envelope.message.channel == TransportChannel::reliable) {
            status = client.value()->server_reliability.track_send(envelope, current_time_ms_);
            if (!status) {
                return status;
            }
        }
        status = send_envelope(server_socket_.fd(), client.value()->address, envelope,
                               client.value()->token);
        if (!status) {
            if (envelope.message.channel == TransportChannel::reliable) {
                const auto rollback =
                    client.value()->server_reliability.rollback_tracked_send(envelope);
                if (!rollback) {
                    return core::Status::failure(
                        rollback.error().code,
                        status.error().message +
                            "; tracking rollback failed: " + rollback.error().message);
                }
            }
            return status;
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<TransportMaintenanceResult>
    poll_maintenance(std::int64_t now_ms) override {
        current_time_ms_ = now_ms;
        (void)server_reassembler_.expire(now_ms);
        TransportMaintenanceResult result;
        pump_server_socket();
        result.malformed_datagram_count = pending_malformed_datagram_count_;
        result.rejected_datagram_count = pending_rejected_datagram_count_;
        result.rate_limited_datagram_count = pending_rate_limited_datagram_count_;
        pending_malformed_datagram_count_ = 0;
        pending_rejected_datagram_count_ = 0;
        pending_rate_limited_datagram_count_ = 0;
        result.connected_clients.swap(pending_connected_clients_);
        result.disconnected_clients.swap(pending_disconnected_clients_);
        for (auto& [_, client] : clients_) {
            if (!client.connected) {
                continue;
            }
            (void)client.reassembler.expire(now_ms);

            auto server_poll = client.server_reliability.poll(now_ms);
            for (const auto& envelope : server_poll.retransmissions) {
                auto sent =
                    send_envelope(server_socket_.fd(), client.address, envelope, client.token);
                if (!sent) {
                    return core::Result<TransportMaintenanceResult>::failure(sent.error().code,
                                                                             sent.error().message);
                }
                ++result.retransmission_count;
            }
            for (auto& dropped : server_poll.dropped) {
                result.dropped_reliable_messages.push_back(std::move(dropped.envelope));
                ++result.dropped_reliable_message_count;
            }

            auto client_poll = client.client_reliability.poll(now_ms);
            for (const auto& envelope : client_poll.retransmissions) {
                if (!client.socket.has_value()) {
                    continue;
                }
                auto sent = send_envelope(client.socket->fd(), server_address_for_local_clients_,
                                          envelope, client.token);
                if (!sent) {
                    return core::Result<TransportMaintenanceResult>::failure(sent.error().code,
                                                                             sent.error().message);
                }
                ++result.retransmission_count;
            }
            for (auto& dropped : client_poll.dropped) {
                result.dropped_reliable_messages.push_back(std::move(dropped.envelope));
                ++result.dropped_reliable_message_count;
            }
            if (client.remote && now_ms - client.last_received_ms >=
                                     static_cast<std::int64_t>(config_.idle_timeout_ms)) {
                client.connected = false;
                server_reassembler_.discard_source(fragment_source_scope(client.address));
                result.disconnected_clients.push_back(
                    core::NetId::from_value(client.server_reliability.remote_id().value()));
            }
        }
        result.client_to_server_bytes = pending_client_to_server_bytes_;
        result.server_to_client_bytes = pending_server_to_client_bytes_;
        result.client_to_server_message_count = pending_client_to_server_message_count_;
        result.server_to_client_message_count = pending_server_to_client_message_count_;
        pending_client_to_server_bytes_ = 0;
        pending_server_to_client_bytes_ = 0;
        pending_client_to_server_message_count_ = 0;
        pending_server_to_client_message_count_ = 0;
        return core::Result<TransportMaintenanceResult>::success(std::move(result));
    }

    [[nodiscard]] std::vector<TransportEnvelope> drain_server_messages() override {
        pump_server_socket();
        std::vector<TransportEnvelope> result;
        result.swap(server_messages_);
        return result;
    }

    [[nodiscard]] core::Result<std::vector<TransportEnvelope>>
    drain_client_messages(core::NetId client_id) override {
        auto client = find_client(client_id);
        if (!client) {
            return core::Result<std::vector<TransportEnvelope>>::failure(client.error().code,
                                                                         client.error().message);
        }

        std::vector<TransportEnvelope> result;
        auto* endpoint = client.value();
        if (!endpoint->socket.has_value()) {
            return core::Result<std::vector<TransportEnvelope>>::failure(
                "transport.remote_client_requires_endpoint",
                "remote client messages must be drained through its independent endpoint");
        }
        drain_socket(endpoint->socket->fd(), endpoint->reassembler,
                     [this, client_id, endpoint, &result](TransportEnvelope envelope,
                                                          const SocketAddress& remote) {
                         if (envelope.sender != config_.server_id ||
                             envelope.recipient != client_id ||
                             !same_socket_endpoint(remote, server_address_for_local_clients_) ||
                             envelope.session_token != endpoint->token) {
                             return;
                         }
                         auto status = validate_transport_message(envelope.message,
                                                                  config_.max_payload_bytes);
                         if (!status) {
                             return;
                         }
                         if (is_reliability_ack_message(envelope.message)) {
                             (void)endpoint->client_reliability.accept_acknowledgement(envelope);
                             return;
                         }
                         if (envelope.message.channel == TransportChannel::reliable) {
                             auto reliable = endpoint->client_reliability.accept_reliable_message(
                                 envelope, current_time_ms_);
                             if (!reliable) {
                                 return;
                             }
                             (void)send_envelope(endpoint->socket->fd(), remote,
                                                 reliable.value().acknowledgement, endpoint->token);
                             if (reliable.value().duplicate) {
                                 return;
                             }
                         }
                         result.push_back(std::move(envelope));
                     });
        return core::Result<std::vector<TransportEnvelope>>::success(std::move(result));
    }

  private:
    PosixDatagramTransportHost(ExternalTransportHostConfig config,
                               PosixDatagramSocket server_socket, SocketAddress server_bound_address,
                               TransportSessionToken security_secret)
        : config_(std::move(config)), server_socket_(std::move(server_socket)),
          server_bound_address_(server_bound_address),
          server_address_for_local_clients_(
              loopback_address(server_bound_address.family(),
                               port_from_address(server_bound_address))),
          fragment_config_{datagram_payload_bytes, 1024u * 1024u, 1024},
          server_reassembler_(fragment_config_), security_secret_(security_secret) {}

    [[nodiscard]] core::Result<ClientEndpoint*> find_client(core::NetId client_id) {
        if (!client_id.is_valid()) {
            return core::Result<ClientEndpoint*>::failure("transport.invalid_client_id",
                                                          "client net id must be valid");
        }
        const auto found = clients_.find(client_id.value());
        if (found == clients_.end()) {
            return core::Result<ClientEndpoint*>::failure(
                "transport.client_not_connected", "client is not known to this transport host");
        }
        return core::Result<ClientEndpoint*>::success(&found->second);
    }

    [[nodiscard]] core::Result<ClientEndpoint*> find_connected_client(core::NetId client_id) {
        auto client = find_client(client_id);
        if (!client) {
            return client;
        }
        if (!client.value()->connected) {
            return core::Result<ClientEndpoint*>::failure("transport.client_not_connected",
                                                          "client is not connected");
        }
        return client;
    }

    [[nodiscard]] core::NetId next_client_id() {
        const auto id = core::NetId::from_value(next_client_id_);
        ++next_client_id_;
        if (id == config_.server_id) {
            return next_client_id();
        }
        return id;
    }

    [[nodiscard]] core::Status
    validate_local_client_send_sequence(const ClientEndpoint& client,
                                        const TransportMessage& message) const {
        if (message.kind != TransportMessageKind::command ||
            message.channel != TransportChannel::reliable) {
            return core::Status::ok();
        }
        if (client.has_sent_reliable_command_sequence &&
            message.sequence <= client.last_sent_reliable_command_sequence) {
            return core::Status::failure(
                "transport.reliable_command_replayed",
                "reliable command sequence must be greater than the last sent command sequence");
        }
        return core::Status::ok();
    }

    void record_local_client_send_sequence(ClientEndpoint& client,
                                           const TransportMessage& message) const {
        if (message.kind == TransportMessageKind::command &&
            message.channel == TransportChannel::reliable) {
            client.last_sent_reliable_command_sequence = message.sequence;
            client.has_sent_reliable_command_sequence = true;
        }
    }

    [[nodiscard]] static bool constant_time_token_equal(TransportSessionToken left,
                                                        TransportSessionToken right) noexcept {
        const auto difference = (left.high ^ right.high) | (left.low ^ right.low);
        return difference == 0;
    }

    [[nodiscard]] std::uint64_t handshake_time_bucket() const noexcept {
        const auto non_negative_time = std::max<std::int64_t>(0, current_time_ms_);
        return static_cast<std::uint64_t>(non_negative_time / 1'000) + 1;
    }

    [[nodiscard]] bool admit_handshake() noexcept {
        if (current_time_ms_ < handshake_window_started_ms_ ||
            current_time_ms_ - handshake_window_started_ms_ >= 1'000) {
            handshake_window_started_ms_ = current_time_ms_;
            handshake_count_ = 0;
        }
        if (handshake_count_ >= config_.max_handshakes_per_second) {
            return false;
        }
        ++handshake_count_;
        return true;
    }

    [[nodiscard]] bool admit_client_message(ClientEndpoint& client,
                                            std::size_t approximate_bytes) noexcept {
        if (current_time_ms_ < client.inbound_window_started_ms ||
            current_time_ms_ - client.inbound_window_started_ms >= 1'000) {
            client.inbound_window_started_ms = current_time_ms_;
            client.inbound_message_count = 0;
            client.inbound_byte_count = 0;
        }
        if (client.inbound_message_count >= config_.max_inbound_messages_per_second ||
            approximate_bytes >
                config_.max_inbound_bytes_per_second -
                    std::min(config_.max_inbound_bytes_per_second, client.inbound_byte_count)) {
            return false;
        }
        ++client.inbound_message_count;
        client.inbound_byte_count += static_cast<std::uint32_t>(approximate_bytes);
        return true;
    }

    [[nodiscard]] ClientEndpoint* remote_client_at(const SocketAddress& address) noexcept {
        for (auto& [_, client] : clients_) {
            if (client.connected && client.remote &&
                same_socket_endpoint(client.address, address)) {
                return &client;
            }
        }
        return nullptr;
    }

    [[nodiscard]] core::Status send_handshake(const SocketAddress& recipient,
                                              const TransportHandshakePacket& packet) {
        auto encoded = TransportHandshakeCodec::encode(packet);
        if (!encoded) {
            return core::Status::failure(encoded.error().code, encoded.error().message);
        }
        return send_datagram(server_socket_.fd(), recipient, encoded.value());
    }

    void reject_handshake(const SocketAddress& remote, TransportSessionToken nonce,
                          std::string reason_code) {
        (void)send_handshake(remote, TransportHandshakePacket{
                                         TransportHandshakeKind::server_reject,
                                         transport_handshake_protocol_version,
                                         nonce,
                                         {},
                                         0,
                                         config_.server_id,
                                         {},
                                         {},
                                         {},
                                         std::move(reason_code),
                                     });
    }

    void accept_handshake(const TransportHandshakePacket& packet, const SocketAddress& remote,
                          std::size_t received_bytes) {
        const auto scope = fragment_source_scope(remote);
        if (packet.kind == TransportHandshakeKind::client_hello) {
            const auto bucket = handshake_time_bucket();
            const auto cookie = make_transport_address_cookie(
                security_secret_, scope, packet.client_nonce, packet.content_fingerprint, bucket);
            const TransportHandshakePacket challenge{
                TransportHandshakeKind::server_challenge,
                transport_handshake_protocol_version,
                packet.client_nonce,
                cookie,
                bucket,
                config_.server_id,
                {},
                {},
                config_.content_fingerprint,
                {},
            };
            auto encoded = TransportHandshakeCodec::encode(challenge);
            if (encoded && encoded.value().size() <= received_bytes * 3U) {
                (void)send_datagram(server_socket_.fd(), remote, encoded.value());
            } else {
                ++pending_rejected_datagram_count_;
            }
            return;
        }
        if (packet.kind != TransportHandshakeKind::client_response) {
            return;
        }
        const auto current_bucket = handshake_time_bucket();
        if (packet.cookie_time_bucket > current_bucket ||
            current_bucket - packet.cookie_time_bucket > 1) {
            reject_handshake(remote, packet.client_nonce, "transport_handshake.challenge_expired");
            return;
        }
        const auto expected =
            make_transport_address_cookie(security_secret_, scope, packet.client_nonce,
                                          packet.content_fingerprint, packet.cookie_time_bucket);
        if (!constant_time_token_equal(expected, packet.cookie)) {
            reject_handshake(remote, packet.client_nonce, "transport_handshake.invalid_cookie");
            return;
        }
        if (packet.content_fingerprint != config_.content_fingerprint) {
            reject_handshake(remote, packet.client_nonce, "transport_handshake.content_mismatch");
            return;
        }
        if (auto* existing = remote_client_at(remote); existing != nullptr) {
            (void)send_handshake(remote, TransportHandshakePacket{
                                             TransportHandshakeKind::server_accept,
                                             transport_handshake_protocol_version,
                                             packet.client_nonce,
                                             {},
                                             0,
                                             config_.server_id,
                                             existing->server_reliability.remote_id(),
                                             existing->token,
                                             config_.content_fingerprint,
                                             {},
                                         });
            return;
        }
        if (connected_client_count() >= config_.max_clients) {
            reject_handshake(remote, packet.client_nonce, "transport.client_limit_reached");
            return;
        }
        auto token = secure_random_transport_token();
        if (!token) {
            reject_handshake(remote, packet.client_nonce, "transport_security.random_failed");
            return;
        }
        const auto id = next_client_id();
        ClientEndpoint endpoint{
            std::nullopt,  remote, fragment_config_, config_.server_id, id, config_.reliability,
            token.value(), true,   current_time_ms_,
        };
        clients_.emplace(id.value(), std::move(endpoint));
        auto accepted = send_handshake(remote, TransportHandshakePacket{
                                                   TransportHandshakeKind::server_accept,
                                                   transport_handshake_protocol_version,
                                                   packet.client_nonce,
                                                   {},
                                                   0,
                                                   config_.server_id,
                                                   id,
                                                   token.value(),
                                                   config_.content_fingerprint,
                                                   {},
                                               });
        if (!accepted) {
            clients_.erase(id.value());
            return;
        }
        pending_connected_clients_.push_back(id);
    }

    void accept_server_envelope(TransportEnvelope envelope, const SocketAddress& remote) {
        if (envelope.recipient != config_.server_id) {
            ++pending_rejected_datagram_count_;
            return;
        }
        auto client = find_connected_client(envelope.sender);
        if (!client || !same_socket_endpoint(client.value()->address, remote) ||
            !constant_time_token_equal(envelope.session_token, client.value()->token)) {
            ++pending_rejected_datagram_count_;
            return;
        }
        auto status = validate_transport_message(envelope.message, config_.max_payload_bytes);
        if (!status) {
            ++pending_malformed_datagram_count_;
            return;
        }
        const auto approximate_bytes =
            envelope.message.payload.size() + envelope.message.payload_type.size() + 128U;
        if (!admit_client_message(*client.value(), approximate_bytes)) {
            ++pending_rate_limited_datagram_count_;
            return;
        }
        client.value()->last_received_ms = current_time_ms_;
        if (is_reliability_ack_message(envelope.message)) {
            (void)client.value()->server_reliability.accept_acknowledgement(envelope);
            return;
        }
        if (envelope.message.channel == TransportChannel::reliable) {
            if (envelope.message.kind == TransportMessageKind::command) {
                status = client.value()->server_command_sequencer.preflight(envelope);
                if (!status) {
                    return;
                }
            }
            auto reliable = client.value()->server_reliability.accept_reliable_message(
                envelope, current_time_ms_);
            if (!reliable) {
                return;
            }
            (void)send_envelope(server_socket_.fd(), remote, reliable.value().acknowledgement,
                                client.value()->token);
            if (reliable.value().duplicate) {
                return;
            }
        }
        if (envelope.message.kind == TransportMessageKind::control &&
            envelope.message.payload_type == "control.keepalive") {
            (void)send_envelope(server_socket_.fd(), remote,
                                TransportEnvelope{
                                    config_.server_id,
                                    envelope.sender,
                                    TransportMessage{
                                        TransportMessageKind::control,
                                        TransportChannel::unreliable,
                                        0,
                                        "control.keepalive_ack",
                                        {},
                                        current_time_ms_,
                                    },
                                    client.value()->token,
                                },
                                client.value()->token);
            return;
        }
        if (envelope.message.kind == TransportMessageKind::control &&
            envelope.message.payload_type == "control.client_disconnect") {
            client.value()->connected = false;
            server_reassembler_.discard_source(fragment_source_scope(remote));
            pending_disconnected_clients_.push_back(envelope.sender);
            return;
        }
        if (envelope.message.kind == TransportMessageKind::command &&
            envelope.message.channel == TransportChannel::reliable) {
            auto ordered = client.value()->server_command_sequencer.accept(std::move(envelope));
            if (!ordered) {
                return;
            }
            for (auto& ready : ordered.value()) {
                server_messages_.push_back(std::move(ready));
            }
            return;
        }
        server_messages_.push_back(std::move(envelope));
    }

    void pump_server_socket() {
        std::array<char, max_datagram_bytes> buffer{};
        for (;;) {
            SocketAddress remote;
            remote.size = sizeof(remote.storage);
            const auto received = ::recvfrom(server_socket_.fd(), buffer.data(), buffer.size(), 0,
                                             reinterpret_cast<sockaddr*>(&remote.storage),
                                             &remote.size);
            if (received < 0) {
                if (would_block()) {
                    return;
                }
                return;
            }
            if (received == 0) {
                continue;
            }
            pending_client_to_server_bytes_ += static_cast<std::uint64_t>(received);
            ++pending_client_to_server_message_count_;
            const auto datagram =
                std::string_view(buffer.data(), static_cast<std::size_t>(received));
            auto handshake = TransportHandshakeCodec::decode(datagram);
            if (handshake) {
                if (!admit_handshake()) {
                    ++pending_rate_limited_datagram_count_;
                    continue;
                }
                accept_handshake(handshake.value(), remote, datagram.size());
                continue;
            }
            auto envelope =
                decode_datagram(datagram, server_reassembler_, fragment_source_scope(remote));
            if (envelope) {
                accept_server_envelope(std::move(envelope).value(), remote);
            } else if (!datagram.starts_with("heartstead.transport.fragment.v1")) {
                ++pending_malformed_datagram_count_;
            }
        }
    }

    [[nodiscard]] core::Status send_envelope(int fd, const SocketAddress& recipient,
                                             TransportEnvelope envelope,
                                             TransportSessionToken token) {
        envelope.session_token = token;
        const auto encoded_packet = TransportPacketCodec::encode(envelope);
        if (encoded_packet.size() <= datagram_payload_bytes) {
            return send_datagram(fd, recipient, encoded_packet);
        }

        auto fragments = TransportPacketFragmentCodec::fragment_packet(
            encoded_packet, next_packet_id_, fragment_config_);
        if (!fragments) {
            return core::Status::failure(fragments.error().code, fragments.error().message);
        }
        ++next_packet_id_;
        for (const auto& fragment : fragments.value()) {
            auto sent =
                send_datagram(fd, recipient, TransportPacketFragmentCodec::encode(fragment));
            if (!sent) {
                return sent;
            }
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Status send_datagram(int fd, const SocketAddress& recipient,
                                             std::string_view datagram) {
        const auto sent =
            ::sendto(fd, datagram.data(), datagram.size(), 0,
                     reinterpret_cast<const sockaddr*>(&recipient.storage), recipient.size);
        if (sent < 0 || static_cast<std::size_t>(sent) != datagram.size()) {
            return core::Status::failure(
                "transport.send_failed",
                socket_error_message("failed to send external transport datagram"));
        }
        if (fd == server_socket_.fd()) {
            pending_server_to_client_bytes_ += static_cast<std::uint64_t>(sent);
            ++pending_server_to_client_message_count_;
        }
        return core::Status::ok();
    }

    template <typename Callback>
    void drain_socket(int fd, TransportPacketReassembler& reassembler, Callback&& callback) {
        std::array<char, max_datagram_bytes> buffer{};
        for (;;) {
            SocketAddress remote;
            remote.size = sizeof(remote.storage);
            const auto received = ::recvfrom(fd, buffer.data(), buffer.size(), 0,
                                             reinterpret_cast<sockaddr*>(&remote.storage),
                                             &remote.size);
            if (received < 0) {
                if (would_block()) {
                    return;
                }
                return;
            }
            if (received == 0) {
                continue;
            }

            auto envelope =
                decode_datagram(std::string_view(buffer.data(), static_cast<std::size_t>(received)),
                                reassembler, fragment_source_scope(remote));
            if (envelope) {
                callback(std::move(envelope.value()), remote);
            }
        }
    }

    [[nodiscard]] std::optional<TransportEnvelope>
    decode_datagram(std::string_view datagram, TransportPacketReassembler& reassembler,
                    std::uint64_t source_scope) {
        auto packet = TransportPacketCodec::decode(
            datagram, TransportPacketCodecConfig{config_.max_payload_bytes});
        if (packet) {
            return std::move(packet).value();
        }

        auto fragment = TransportPacketFragmentCodec::decode(datagram, fragment_config_);
        if (!fragment) {
            return std::nullopt;
        }
        auto reassembled = reassembler.accept_fragment(source_scope, std::move(fragment).value(),
                                                       current_time_ms_);
        if (!reassembled || !reassembled.value().complete) {
            return std::nullopt;
        }
        auto decoded = TransportPacketCodec::decode(
            reassembled.value().packet, TransportPacketCodecConfig{config_.max_payload_bytes});
        if (!decoded) {
            return std::nullopt;
        }
        return std::move(decoded).value();
    }

    static constexpr std::uint32_t datagram_payload_bytes = 1200;
    static constexpr std::size_t max_datagram_bytes = 64U * 1024U;

    ExternalTransportHostConfig config_;
    PosixDatagramSocket server_socket_;
    SocketAddress server_bound_address_{};
    SocketAddress server_address_for_local_clients_{};
    TransportPacketFragmentCodecConfig fragment_config_{};
    TransportPacketReassembler server_reassembler_;
    std::uint64_t next_client_id_ = 2;
    std::uint64_t next_packet_id_ = 1;
    std::int64_t current_time_ms_ = 0;
    std::unordered_map<std::uint64_t, ClientEndpoint> clients_;
    TransportSessionToken security_secret_;
    std::vector<TransportEnvelope> server_messages_;
    std::vector<core::NetId> pending_connected_clients_;
    std::vector<core::NetId> pending_disconnected_clients_;
    std::int64_t handshake_window_started_ms_ = 0;
    std::uint32_t handshake_count_ = 0;
    std::uint32_t pending_malformed_datagram_count_ = 0;
    std::uint32_t pending_rejected_datagram_count_ = 0;
    std::uint32_t pending_rate_limited_datagram_count_ = 0;
    std::uint64_t pending_client_to_server_bytes_ = 0;
    std::uint64_t pending_server_to_client_bytes_ = 0;
    std::uint32_t pending_client_to_server_message_count_ = 0;
    std::uint32_t pending_server_to_client_message_count_ = 0;
};

#endif

} // namespace

InMemoryTransportHost::InMemoryTransportHost(InMemoryTransportHostConfig config)
    : config_(config) {}

TransportBackend InMemoryTransportHost::backend() const noexcept {
    return TransportBackend::in_memory;
}

std::string_view InMemoryTransportHost::backend_name() const noexcept {
    return transport_backend_name(TransportBackend::in_memory);
}

TransportCapabilities InMemoryTransportHost::capabilities() const noexcept {
    return transport_host_capabilities(config_);
}

core::NetId InMemoryTransportHost::server_id() const noexcept {
    return config_.server_id;
}

std::size_t InMemoryTransportHost::connected_client_count() const noexcept {
    std::size_t count = 0;
    for (const auto& [_, client] : clients_) {
        if (client.connected) {
            ++count;
        }
    }
    return count;
}

bool InMemoryTransportHost::is_client_connected(core::NetId client_id) const noexcept {
    const auto found = clients_.find(client_id.value());
    return found != clients_.end() && found->second.connected;
}

std::vector<core::NetId> InMemoryTransportHost::connected_client_ids() const {
    std::vector<core::NetId> result;
    result.reserve(clients_.size());
    for (const auto& [id, client] : clients_) {
        if (client.connected) {
            result.push_back(core::NetId::from_value(id));
        }
    }
    std::ranges::sort(result);
    return result;
}

core::Result<core::NetId> InMemoryTransportHost::connect_client() {
    auto status = validate_transport_host_config(config_);
    if (!status) {
        return core::Result<core::NetId>::failure(status.error().code, status.error().message);
    }
    if (connected_client_count() >= config_.max_clients) {
        return core::Result<core::NetId>::failure("transport.client_limit_reached",
                                                  "transport client limit has been reached");
    }

    const auto id = next_client_id();
    clients_.emplace(id.value(), ClientQueues{});
    return core::Result<core::NetId>::success(id);
}

core::Status InMemoryTransportHost::disconnect_client(core::NetId client_id) {
    auto client = find_connected_client(client_id);
    if (!client) {
        return core::Status::failure(client.error().code, client.error().message);
    }
    client.value()->connected = false;
    return core::Status::ok();
}

core::Status InMemoryTransportHost::send_client_to_server(core::NetId client_id,
                                                          TransportMessage message) {
    auto client = find_connected_client(client_id);
    if (!client) {
        return core::Status::failure(client.error().code, client.error().message);
    }
    auto status = validate_message(message);
    if (!status) {
        return status;
    }
    status = validate_client_to_server_sequence(*client.value(), message);
    if (!status) {
        return status;
    }

    record_client_to_server_sequence(*client.value(), message);
    queue_or_deliver(TransportEnvelope{client_id, config_.server_id, std::move(message)}, true);
    return core::Status::ok();
}

core::Status InMemoryTransportHost::send_server_to_client(core::NetId client_id,
                                                          TransportMessage message) {
    auto client = find_connected_client(client_id);
    if (!client) {
        return core::Status::failure(client.error().code, client.error().message);
    }
    auto status = validate_message(message);
    if (!status) {
        return status;
    }

    queue_or_deliver(TransportEnvelope{config_.server_id, client_id, std::move(message)}, false);
    return core::Status::ok();
}

core::Result<TransportMaintenanceResult>
InMemoryTransportHost::poll_maintenance(std::int64_t now_ms) {
    current_time_ms_ = now_ms;
    deliver_pending(now_ms);
    TransportMaintenanceResult result;
    result.client_to_server_bytes = pending_client_to_server_bytes_;
    result.server_to_client_bytes = pending_server_to_client_bytes_;
    result.client_to_server_message_count = pending_client_to_server_message_count_;
    result.server_to_client_message_count = pending_server_to_client_message_count_;
    result.simulated_dropped_unreliable_message_count = pending_simulated_drop_count_;
    result.pending_impaired_message_count = static_cast<std::uint32_t>(pending_deliveries_.size());
    pending_client_to_server_bytes_ = 0;
    pending_server_to_client_bytes_ = 0;
    pending_client_to_server_message_count_ = 0;
    pending_server_to_client_message_count_ = 0;
    pending_simulated_drop_count_ = 0;
    return core::Result<TransportMaintenanceResult>::success(std::move(result));
}

std::vector<TransportEnvelope> InMemoryTransportHost::drain_server_messages() {
    return drain_queue(server_inbox_);
}

core::Result<std::vector<TransportEnvelope>>
InMemoryTransportHost::drain_client_messages(core::NetId client_id) {
    auto client = find_client(client_id);
    if (!client) {
        return core::Result<std::vector<TransportEnvelope>>::failure(client.error().code,
                                                                     client.error().message);
    }
    return core::Result<std::vector<TransportEnvelope>>::success(
        drain_queue(client.value()->inbox));
}

core::Status InMemoryTransportHost::validate_message(const TransportMessage& message) const {
    return validate_transport_message(message, config_.max_payload_bytes);
}

core::Status
InMemoryTransportHost::validate_client_to_server_sequence(const ClientQueues& client,
                                                          const TransportMessage& message) const {
    if (message.kind != TransportMessageKind::command ||
        message.channel != TransportChannel::reliable) {
        return core::Status::ok();
    }
    if (client.has_reliable_command_sequence &&
        message.sequence <= client.last_reliable_command_sequence) {
        return core::Status::failure(
            "transport.reliable_command_replayed",
            "reliable command sequence must be greater than the last accepted command sequence");
    }
    return core::Status::ok();
}

void InMemoryTransportHost::record_client_to_server_sequence(ClientQueues& client,
                                                             const TransportMessage& message) {
    if (message.kind == TransportMessageKind::command &&
        message.channel == TransportChannel::reliable) {
        client.last_reliable_command_sequence = message.sequence;
        client.has_reliable_command_sequence = true;
    }
}

void InMemoryTransportHost::queue_or_deliver(TransportEnvelope envelope, bool to_server) {
    const auto encoded_bytes = encoded_envelope_bytes(envelope);
    if (to_server) {
        pending_client_to_server_bytes_ += encoded_bytes;
        ++pending_client_to_server_message_count_;
    } else {
        pending_server_to_client_bytes_ += encoded_bytes;
        ++pending_server_to_client_message_count_;
    }

    if (!impairment_enabled()) {
        if (to_server) {
            server_inbox_.push(std::move(envelope));
        } else {
            auto client = find_client(envelope.recipient);
            if (client && client.value()->connected) {
                client.value()->inbox.push(std::move(envelope));
            }
        }
        return;
    }

    const auto impairment = next_impairment_value();
    if (envelope.message.channel == TransportChannel::unreliable &&
        impairment % 10'000U < config_.simulated_unreliable_loss_basis_points) {
        ++pending_simulated_drop_count_;
        return;
    }

    const auto jitter_span = static_cast<std::uint64_t>(config_.simulated_jitter_ms) * 2U + 1U;
    const auto jitter_sample = static_cast<std::int64_t>((impairment >> 16U) % jitter_span) -
                               static_cast<std::int64_t>(config_.simulated_jitter_ms);
    auto deliver_at = current_time_ms_ +
                      static_cast<std::int64_t>(config_.simulated_one_way_latency_ms) +
                      jitter_sample;
    deliver_at = std::max(deliver_at, current_time_ms_);
    if (envelope.message.channel == TransportChannel::reliable) {
        auto& last_delivery = to_server
                                  ? last_reliable_server_delivery_ms_
                                  : last_reliable_client_delivery_ms_[envelope.recipient.value()];
        deliver_at = std::max(deliver_at, last_delivery + 1);
        last_delivery = deliver_at;
    }
    pending_deliveries_.push_back(PendingDelivery{deliver_at, delivery_order_++, to_server,
                                                  to_server ? envelope.sender : envelope.recipient,
                                                  std::move(envelope)});
}

void InMemoryTransportHost::deliver_pending(std::int64_t now_ms) {
    std::ranges::sort(pending_deliveries_,
                      [](const PendingDelivery& left, const PendingDelivery& right) {
                          if (left.deliver_at_ms != right.deliver_at_ms) {
                              return left.deliver_at_ms < right.deliver_at_ms;
                          }
                          return left.order < right.order;
                      });
    std::size_t retained = 0;
    for (std::size_t index = 0; index < pending_deliveries_.size(); ++index) {
        auto& pending = pending_deliveries_[index];
        if (pending.deliver_at_ms > now_ms) {
            if (retained != index) {
                pending_deliveries_[retained] = std::move(pending);
            }
            ++retained;
            continue;
        }
        auto client = find_client(pending.client_id);
        if (!client || !client.value()->connected) {
            continue;
        }
        if (pending.to_server) {
            server_inbox_.push(std::move(pending.envelope));
        } else {
            client.value()->inbox.push(std::move(pending.envelope));
        }
    }
    pending_deliveries_.resize(retained);
}

bool InMemoryTransportHost::impairment_enabled() const noexcept {
    return config_.simulated_one_way_latency_ms != 0 || config_.simulated_jitter_ms != 0 ||
           config_.simulated_unreliable_loss_basis_points != 0;
}

std::uint64_t InMemoryTransportHost::next_impairment_value() noexcept {
    auto value = config_.impairment_seed + (++impairment_counter_ * 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::size_t InMemoryTransportHost::encoded_envelope_bytes(const TransportEnvelope& envelope) const {
    return TransportPacketCodec::encode(envelope).size();
}

core::Result<InMemoryTransportHost::ClientQueues*>
InMemoryTransportHost::find_client(core::NetId client_id) {
    if (!client_id.is_valid()) {
        return core::Result<ClientQueues*>::failure("transport.invalid_client_id",
                                                    "client net id must be valid");
    }
    const auto found = clients_.find(client_id.value());
    if (found == clients_.end()) {
        return core::Result<ClientQueues*>::failure("transport.client_not_connected",
                                                    "client is not known to this transport host");
    }
    return core::Result<ClientQueues*>::success(&found->second);
}

core::Result<InMemoryTransportHost::ClientQueues*>
InMemoryTransportHost::find_connected_client(core::NetId client_id) {
    auto client = find_client(client_id);
    if (!client) {
        return client;
    }
    if (!client.value()->connected) {
        return core::Result<ClientQueues*>::failure("transport.client_not_connected",
                                                    "client is not connected");
    }
    return client;
}

core::NetId InMemoryTransportHost::next_client_id() {
    const auto id = core::NetId::from_value(next_client_id_);
    ++next_client_id_;
    if (id == config_.server_id) {
        return next_client_id();
    }
    return id;
}

core::Result<std::unique_ptr<ITransportHost>> create_transport_host(TransportHostDesc desc) {
    auto status = validate_transport_host_desc(desc);
    if (!status) {
        return core::Result<std::unique_ptr<ITransportHost>>::failure(status.error().code,
                                                                      status.error().message);
    }

    switch (desc.backend) {
    case TransportBackend::in_memory:
        return core::Result<std::unique_ptr<ITransportHost>>::success(
            std::make_unique<InMemoryTransportHost>(desc.in_memory));
    case TransportBackend::external_library:
#if HEARTSTEAD_HAS_POSIX_DATAGRAM_TRANSPORT
        if (!posix_datagram_transport_available()) {
            return core::Result<std::unique_ptr<ITransportHost>>::failure(
                "transport.external_unavailable",
                "POSIX UDP socket transport is not available in this environment for " +
                    transport_endpoint_name(desc.external.bind_endpoint));
        }
        return PosixDatagramTransportHost::create(std::move(desc.external));
#else
        return core::Result<std::unique_ptr<ITransportHost>>::failure(
            "transport.external_unavailable",
            "external network transport is not compiled in yet for " +
                transport_endpoint_name(desc.external.bind_endpoint));
#endif
    }

    return core::Result<std::unique_ptr<ITransportHost>>::failure("transport.unknown_backend",
                                                                  "unknown transport backend");
}

core::Status validate_transport_host_desc(const TransportHostDesc& desc) {
    switch (desc.backend) {
    case TransportBackend::in_memory:
        return validate_transport_host_config(desc.in_memory);
    case TransportBackend::external_library:
        return validate_external_transport_host_config(desc.external);
    }
    return core::Status::failure("transport.unknown_backend", "unknown transport backend");
}

core::Status validate_transport_host_config(const InMemoryTransportHostConfig& config) {
    if (!config.server_id.is_valid()) {
        return core::Status::failure("transport.invalid_server_id", "server net id must be valid");
    }
    if (config.max_payload_bytes == 0) {
        return core::Status::failure("transport.invalid_max_payload",
                                     "max payload bytes must be non-zero");
    }
    if (config.max_clients == 0) {
        return core::Status::failure("transport.invalid_max_clients",
                                     "max client count must be non-zero");
    }
    if (config.simulated_one_way_latency_ms > 60'000 || config.simulated_jitter_ms > 60'000 ||
        config.simulated_unreliable_loss_basis_points > 10'000) {
        return core::Status::failure(
            "transport.invalid_impairment",
            "simulated latency/jitter must be at most 60 seconds and loss at most 100 percent");
    }
    return core::Status::ok();
}

core::Status validate_external_transport_host_config(const ExternalTransportHostConfig& config) {
    if (!config.server_id.is_valid()) {
        return core::Status::failure("transport.invalid_server_id", "server net id must be valid");
    }
    auto endpoint = validate_transport_endpoint(config.bind_endpoint);
    if (!endpoint) {
        return endpoint;
    }
    if (config.max_payload_bytes == 0) {
        return core::Status::failure("transport.invalid_max_payload",
                                     "max payload bytes must be non-zero");
    }
    if (config.max_clients == 0) {
        return core::Status::failure("transport.invalid_max_clients",
                                     "max client count must be non-zero");
    }
    auto reliability = validate_transport_reliability_config(config.reliability);
    if (!reliability) {
        return reliability;
    }
    if (config.content_fingerprint.size() > 192 ||
        std::ranges::any_of(config.content_fingerprint, [](char value) {
            return static_cast<unsigned char>(value) < 0x21U ||
                   static_cast<unsigned char>(value) > 0x7eU || value == '=' || value == '%';
        })) {
        return core::Status::failure("transport.invalid_content_fingerprint",
                                     "transport content fingerprint is invalid");
    }
    if (config.handshake_timeout_ms == 0 || config.idle_timeout_ms == 0 ||
        config.keepalive_interval_ms == 0 ||
        config.keepalive_interval_ms >= config.idle_timeout_ms) {
        return core::Status::failure("transport.invalid_timeouts",
                                     "transport handshake, keepalive, or idle timeout is invalid");
    }
    if (config.max_inbound_messages_per_second == 0 ||
        config.max_inbound_bytes_per_second < config.max_payload_bytes ||
        config.max_handshakes_per_second == 0) {
        return core::Status::failure(
            "transport.invalid_rate_limits",
            "transport inbound message, byte, and handshake limits are invalid");
    }
    return core::Status::ok();
}

core::Status validate_transport_endpoint(const TransportEndpoint& endpoint) {
    if (!is_valid_endpoint_address(endpoint.address)) {
        return core::Status::failure("transport.invalid_endpoint_address",
                                     "transport endpoint address must not be empty or contain "
                                     "whitespace");
    }
    return core::Status::ok();
}

core::Result<TransportEndpoint> parse_transport_endpoint(std::string_view endpoint,
                                                         std::uint16_t default_port) {
    if (endpoint.empty()) {
        return core::Result<TransportEndpoint>::failure("transport.invalid_endpoint_address",
                                                        "transport endpoint must not be empty");
    }
    std::string_view address = endpoint;
    std::string_view port_text;
    if (endpoint.front() == '[') {
        const auto closing = endpoint.find(']');
        if (closing == std::string_view::npos || closing == 1) {
            return core::Result<TransportEndpoint>::failure(
                "transport.invalid_endpoint_address", "bracketed transport endpoint is invalid");
        }
        address = endpoint.substr(1, closing - 1);
        if (closing + 1 < endpoint.size()) {
            if (endpoint[closing + 1] != ':') {
                return core::Result<TransportEndpoint>::failure(
                    "transport.invalid_endpoint_address",
                    "bracketed transport endpoint port separator is invalid");
            }
            port_text = endpoint.substr(closing + 2);
        }
    } else {
        const auto separator = endpoint.rfind(':');
        if (separator != std::string_view::npos && endpoint.find(':') == separator) {
            address = endpoint.substr(0, separator);
            port_text = endpoint.substr(separator + 1);
        }
    }
    std::uint16_t port = default_port;
    if (!port_text.empty()) {
        std::uint32_t parsed = 0;
        const auto [end, error] =
            std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed);
        if (error != std::errc{} || end != port_text.data() + port_text.size() ||
            parsed > UINT16_MAX) {
            return core::Result<TransportEndpoint>::failure(
                "transport.invalid_endpoint_port",
                "transport endpoint port must be between 0 and 65535");
        }
        port = static_cast<std::uint16_t>(parsed);
    }
    TransportEndpoint result{std::string(address), port};
    auto status = validate_transport_endpoint(result);
    if (!status) {
        return core::Result<TransportEndpoint>::failure(status.error().code,
                                                        status.error().message);
    }
    return core::Result<TransportEndpoint>::success(std::move(result));
}

core::Status validate_transport_message(const TransportMessage& message,
                                        std::uint32_t max_payload_bytes) {
    if (!is_valid_payload_type(message.payload_type)) {
        return core::Status::failure("transport.invalid_payload_type",
                                     "transport payload type must contain lowercase letters, "
                                     "digits, underscores, dashes, or dots");
    }
    if (message.payload.size() > max_payload_bytes) {
        return core::Status::failure("transport.payload_too_large",
                                     "transport payload exceeds max payload bytes");
    }
    return core::Status::ok();
}

core::Result<TransportCapabilities> transport_host_capabilities(const TransportHostDesc& desc) {
    auto status = validate_transport_host_desc(desc);
    if (!status) {
        return core::Result<TransportCapabilities>::failure(status.error().code,
                                                            status.error().message);
    }

    switch (desc.backend) {
    case TransportBackend::in_memory:
        return core::Result<TransportCapabilities>::success(
            transport_host_capabilities(desc.in_memory));
    case TransportBackend::external_library:
        return core::Result<TransportCapabilities>::success(
            transport_host_capabilities(desc.external));
    }
    return core::Result<TransportCapabilities>::failure("transport.unknown_backend",
                                                        "unknown transport backend");
}

TransportCapabilities
transport_host_capabilities(const InMemoryTransportHostConfig& config) noexcept {
    return TransportCapabilities{
        true, true, true, true, config.max_payload_bytes, config.max_clients,
    };
}

TransportCapabilities
transport_host_capabilities(const ExternalTransportHostConfig& config) noexcept {
    return TransportCapabilities{
        true, config.enable_unreliable_channel, true,
        true, config.max_payload_bytes,         config.max_clients,
    };
}

core::NetId transport_host_server_id(const TransportHostDesc& desc) noexcept {
    switch (desc.backend) {
    case TransportBackend::in_memory:
        return desc.in_memory.server_id;
    case TransportBackend::external_library:
        return desc.external.server_id;
    }
    return {};
}

TransportMessage make_command_transport_message(const CommandEnvelope& envelope,
                                                TransportChannel channel) {
    auto payload = envelope.payload;
    auto structured = CommandPayloadTextCodec::decode(payload);
    if (structured) {
        payload = CommandPayloadBinaryCodec::encode(structured.value());
    }
    return TransportMessage{TransportMessageKind::command,
                            channel,
                            envelope.sequence,
                            envelope.type,
                            std::move(payload),
                            envelope.client_time_ms};
}

core::Result<CommandEnvelope> command_envelope_from_transport(const TransportEnvelope& envelope) {
    if (envelope.message.kind != TransportMessageKind::command) {
        return core::Result<CommandEnvelope>::failure("transport.not_command_message",
                                                      "transport message is not a command");
    }
    if (!is_valid_payload_type(envelope.message.payload_type)) {
        return core::Result<CommandEnvelope>::failure("transport.invalid_payload_type",
                                                      "transport command type is invalid");
    }

    CommandEnvelope command;
    command.sequence = envelope.message.sequence;
    command.sender = envelope.sender;
    command.type = envelope.message.payload_type;
    if (CommandPayloadBinaryCodec::is_encoded(envelope.message.payload)) {
        auto payload = CommandPayloadBinaryCodec::decode(envelope.message.payload);
        if (!payload) {
            return core::Result<CommandEnvelope>::failure(payload.error().code,
                                                          payload.error().message);
        }
        command.payload = CommandPayloadTextCodec::encode(payload.value());
    } else {
        command.payload = envelope.message.payload;
    }
    command.client_time_ms = envelope.message.timestamp_ms;
    return core::Result<CommandEnvelope>::success(std::move(command));
}

TransportBackendInfo transport_backend_info(TransportBackend backend) noexcept {
    switch (backend) {
    case TransportBackend::in_memory:
        return TransportBackendInfo{
            TransportBackend::in_memory,
            transport_backend_name(TransportBackend::in_memory),
            true,
            "available",
        };
    case TransportBackend::external_library:
#if HEARTSTEAD_HAS_POSIX_DATAGRAM_TRANSPORT
        if (!posix_datagram_transport_available()) {
            return TransportBackendInfo{
                TransportBackend::external_library,
                transport_backend_name(TransportBackend::external_library),
                false,
                "POSIX UDP socket transport is blocked by the current environment",
            };
        }
        return TransportBackendInfo{
            TransportBackend::external_library,
            transport_backend_name(TransportBackend::external_library),
            true,
            "available through POSIX UDP packet transport",
        };
#else
        return TransportBackendInfo{
            TransportBackend::external_library,
            transport_backend_name(TransportBackend::external_library),
            false,
            "external network transport is not compiled in yet",
        };
#endif
    }
    return TransportBackendInfo{backend, "unknown", false, "unknown transport backend"};
}

std::string_view transport_backend_name(TransportBackend backend) noexcept {
    switch (backend) {
    case TransportBackend::in_memory:
        return "in_memory";
    case TransportBackend::external_library:
        return "external_library";
    }
    return "unknown";
}

std::string_view transport_channel_name(TransportChannel channel) noexcept {
    switch (channel) {
    case TransportChannel::reliable:
        return "reliable";
    case TransportChannel::unreliable:
        return "unreliable";
    }
    return "unknown";
}

std::string_view transport_message_kind_name(TransportMessageKind kind) noexcept {
    switch (kind) {
    case TransportMessageKind::command:
        return "command";
    case TransportMessageKind::command_result:
        return "command_result";
    case TransportMessageKind::replication:
        return "replication";
    case TransportMessageKind::control:
        return "control";
    }
    return "unknown";
}

std::string transport_endpoint_name(const TransportEndpoint& endpoint) {
    std::ostringstream output;
    output << endpoint.address << ':' << endpoint.port;
    return output.str();
}

} // namespace heartstead::net
