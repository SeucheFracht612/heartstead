#pragma once

#include "engine/core/result.hpp"
#include "engine/net/transport.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace heartstead::net {

inline constexpr std::uint16_t transport_handshake_protocol_version = 1;
inline constexpr std::size_t transport_handshake_max_datagram_bytes = 1024;

enum class TransportHandshakeKind {
    client_hello,
    server_challenge,
    client_response,
    server_accept,
    server_reject,
};

struct TransportHandshakePacket {
    TransportHandshakeKind kind = TransportHandshakeKind::client_hello;
    std::uint16_t protocol_version = transport_handshake_protocol_version;
    TransportSessionToken client_nonce;
    TransportSessionToken cookie;
    std::uint64_t cookie_time_bucket = 0;
    core::NetId server_id;
    core::NetId assigned_client_id;
    TransportSessionToken session_token;
    std::string content_fingerprint;
    std::string reason_code;
};

class TransportHandshakeCodec {
  public:
    [[nodiscard]] static core::Result<std::string>
    encode(const TransportHandshakePacket& packet);
    [[nodiscard]] static core::Result<TransportHandshakePacket> decode(std::string_view datagram);
};

[[nodiscard]] core::Status
validate_transport_handshake_packet(const TransportHandshakePacket& packet);
[[nodiscard]] core::Result<TransportSessionToken> secure_random_transport_token();
[[nodiscard]] TransportSessionToken
make_transport_address_cookie(TransportSessionToken secret, std::uint64_t endpoint_scope,
                              TransportSessionToken client_nonce,
                              std::string_view content_fingerprint,
                              std::uint64_t time_bucket) noexcept;
[[nodiscard]] std::string transport_session_token_hex(TransportSessionToken token);
[[nodiscard]] core::Result<TransportSessionToken>
transport_session_token_from_hex(std::string_view value);
[[nodiscard]] std::string_view
transport_handshake_kind_name(TransportHandshakeKind kind) noexcept;

} // namespace heartstead::net
