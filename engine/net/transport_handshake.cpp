#include "engine/net/transport_handshake.hpp"

#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#endif

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace heartstead::net {

namespace {

constexpr std::string_view handshake_magic = "heartstead.handshake.v1";
constexpr std::size_t maximum_fingerprint_bytes = 192;
constexpr std::size_t maximum_reason_code_bytes = 96;

[[nodiscard]] std::uint64_t rotate_left(std::uint64_t value, unsigned bits) noexcept {
    return (value << bits) | (value >> (64U - bits));
}

void sip_round(std::uint64_t& v0, std::uint64_t& v1, std::uint64_t& v2,
               std::uint64_t& v3) noexcept {
    v0 += v1;
    v1 = rotate_left(v1, 13);
    v1 ^= v0;
    v0 = rotate_left(v0, 32);
    v2 += v3;
    v3 = rotate_left(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = rotate_left(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = rotate_left(v1, 17);
    v1 ^= v2;
    v2 = rotate_left(v2, 32);
}

[[nodiscard]] std::uint64_t read_little_u64(const std::byte* bytes) noexcept {
    std::uint64_t result = 0;
    for (unsigned index = 0; index < 8; ++index) {
        result |= std::to_integer<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return result;
}

[[nodiscard]] std::uint64_t
siphash24(TransportSessionToken key, std::span<const std::byte> bytes) noexcept {
    std::uint64_t v0 = 0x736f6d6570736575ULL ^ key.high;
    std::uint64_t v1 = 0x646f72616e646f6dULL ^ key.low;
    std::uint64_t v2 = 0x6c7967656e657261ULL ^ key.high;
    std::uint64_t v3 = 0x7465646279746573ULL ^ key.low;

    std::size_t offset = 0;
    while (offset + 8 <= bytes.size()) {
        const auto word = read_little_u64(bytes.data() + offset);
        v3 ^= word;
        sip_round(v0, v1, v2, v3);
        sip_round(v0, v1, v2, v3);
        v0 ^= word;
        offset += 8;
    }

    std::uint64_t tail = static_cast<std::uint64_t>(bytes.size()) << 56U;
    for (std::size_t index = 0; offset + index < bytes.size(); ++index) {
        tail |= std::to_integer<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    v3 ^= tail;
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    v0 ^= tail;
    v2 ^= 0xffU;
    for (unsigned round = 0; round < 4; ++round) {
        sip_round(v0, v1, v2, v3);
    }
    return v0 ^ v1 ^ v2 ^ v3;
}

[[nodiscard]] bool valid_simple_text(std::string_view value, std::size_t maximum,
                                     bool allow_empty) noexcept {
    if ((!allow_empty && value.empty()) || value.size() > maximum) {
        return false;
    }
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x21U || byte > 0x7eU || character == '=' || character == '%') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] core::Result<std::uint64_t> parse_u64(std::string_view value,
                                                    std::string_view field) {
    std::uint64_t result = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return core::Result<std::uint64_t>::failure(
            "transport_handshake.invalid_number",
            "handshake numeric field is invalid: " + std::string(field));
    }
    return core::Result<std::uint64_t>::success(result);
}

[[nodiscard]] core::Result<std::map<std::string, std::string>>
parse_fields(std::string_view datagram) {
    if (datagram.empty() || datagram.size() > transport_handshake_max_datagram_bytes) {
        return core::Result<std::map<std::string, std::string>>::failure(
            "transport_handshake.invalid_size", "handshake datagram size is invalid");
    }
    std::map<std::string, std::string> fields;
    std::size_t line_start = 0;
    bool saw_magic = false;
    bool saw_end = false;
    while (line_start <= datagram.size()) {
        const auto line_end = datagram.find('\n', line_start);
        const auto line = datagram.substr(
            line_start, line_end == std::string_view::npos ? datagram.size() - line_start
                                                            : line_end - line_start);
        if (!saw_magic) {
            if (line != handshake_magic) {
                return core::Result<std::map<std::string, std::string>>::failure(
                    "transport_handshake.invalid_magic",
                    "handshake datagram has invalid magic");
            }
            saw_magic = true;
        } else if (line == "end") {
            saw_end = true;
            break;
        } else if (!line.empty()) {
            const auto separator = line.find('=');
            if (separator == std::string_view::npos || separator == 0) {
                return core::Result<std::map<std::string, std::string>>::failure(
                    "transport_handshake.invalid_line",
                    "handshake fields must use key=value syntax");
            }
            const auto [_, inserted] = fields.emplace(
                std::string(line.substr(0, separator)),
                std::string(line.substr(separator + 1)));
            if (!inserted) {
                return core::Result<std::map<std::string, std::string>>::failure(
                    "transport_handshake.duplicate_key",
                    "handshake datagram contains a duplicate field");
            }
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }
    if (!saw_magic || !saw_end) {
        return core::Result<std::map<std::string, std::string>>::failure(
            "transport_handshake.incomplete", "handshake datagram is incomplete");
    }
    return core::Result<std::map<std::string, std::string>>::success(std::move(fields));
}

[[nodiscard]] core::Result<std::string_view>
required(const std::map<std::string, std::string>& fields, std::string_view key) {
    const auto found = fields.find(std::string(key));
    if (found == fields.end() || found->second.empty()) {
        return core::Result<std::string_view>::failure(
            "transport_handshake.missing_field",
            "handshake datagram is missing field: " + std::string(key));
    }
    return core::Result<std::string_view>::success(found->second);
}

[[nodiscard]] core::Result<TransportHandshakeKind> parse_kind(std::string_view value) {
    constexpr std::array kinds{
        TransportHandshakeKind::client_hello,
        TransportHandshakeKind::server_challenge,
        TransportHandshakeKind::client_response,
        TransportHandshakeKind::server_accept,
        TransportHandshakeKind::server_reject,
    };
    for (const auto kind : kinds) {
        if (value == transport_handshake_kind_name(kind)) {
            return core::Result<TransportHandshakeKind>::success(kind);
        }
    }
    return core::Result<TransportHandshakeKind>::failure(
        "transport_handshake.invalid_kind", "handshake kind is invalid");
}

} // namespace

core::Result<std::string>
TransportHandshakeCodec::encode(const TransportHandshakePacket& packet) {
    auto status = validate_transport_handshake_packet(packet);
    if (!status) {
        return core::Result<std::string>::failure(status.error().code, status.error().message);
    }
    std::ostringstream output;
    output << handshake_magic << '\n';
    output << "kind=" << transport_handshake_kind_name(packet.kind) << '\n';
    output << "protocol=" << packet.protocol_version << '\n';
    output << "nonce=" << transport_session_token_hex(packet.client_nonce) << '\n';
    output << "cookie=" << transport_session_token_hex(packet.cookie) << '\n';
    output << "bucket=" << packet.cookie_time_bucket << '\n';
    output << "server_id=" << packet.server_id.value() << '\n';
    output << "client_id=" << packet.assigned_client_id.value() << '\n';
    output << "token=" << transport_session_token_hex(packet.session_token) << '\n';
    output << "fingerprint=" << packet.content_fingerprint << '\n';
    output << "reason=" << packet.reason_code << '\n';
    output << "end\n";
    auto encoded = output.str();
    if (encoded.size() > transport_handshake_max_datagram_bytes) {
        return core::Result<std::string>::failure(
            "transport_handshake.invalid_size", "encoded handshake exceeds datagram budget");
    }
    return core::Result<std::string>::success(std::move(encoded));
}

core::Result<TransportHandshakePacket>
TransportHandshakeCodec::decode(std::string_view datagram) {
    auto fields = parse_fields(datagram);
    if (!fields) {
        return core::Result<TransportHandshakePacket>::failure(fields.error().code,
                                                               fields.error().message);
    }
    auto kind_value = required(fields.value(), "kind");
    auto protocol_value = required(fields.value(), "protocol");
    auto nonce_value = required(fields.value(), "nonce");
    auto cookie_value = required(fields.value(), "cookie");
    auto bucket_value = required(fields.value(), "bucket");
    auto server_id_value = required(fields.value(), "server_id");
    auto client_id_value = required(fields.value(), "client_id");
    auto token_value = required(fields.value(), "token");
    const auto fingerprint = fields.value().find("fingerprint");
    const auto reason = fields.value().find("reason");
    if (!kind_value || !protocol_value || !nonce_value || !cookie_value || !bucket_value ||
        !server_id_value || !client_id_value || !token_value ||
        fingerprint == fields.value().end() || reason == fields.value().end()) {
        return core::Result<TransportHandshakePacket>::failure(
            "transport_handshake.missing_field",
            "handshake datagram is missing required fields");
    }
    auto kind = parse_kind(kind_value.value());
    auto protocol = parse_u64(protocol_value.value(), "protocol");
    auto nonce = transport_session_token_from_hex(nonce_value.value());
    auto cookie = transport_session_token_from_hex(cookie_value.value());
    auto bucket = parse_u64(bucket_value.value(), "bucket");
    auto server_id = parse_u64(server_id_value.value(), "server_id");
    auto client_id = parse_u64(client_id_value.value(), "client_id");
    auto token = transport_session_token_from_hex(token_value.value());
    if (!kind || !protocol || !nonce || !cookie || !bucket || !server_id || !client_id ||
        !token || protocol.value() > std::numeric_limits<std::uint16_t>::max()) {
        const auto* error =
            !kind       ? &kind.error()
            : !protocol ? &protocol.error()
            : !nonce    ? &nonce.error()
            : !cookie   ? &cookie.error()
            : !bucket   ? &bucket.error()
            : !server_id ? &server_id.error()
            : !client_id ? &client_id.error()
            : !token     ? &token.error()
                         : nullptr;
        return core::Result<TransportHandshakePacket>::failure(
            error == nullptr ? "transport_handshake.number_out_of_range" : error->code,
            error == nullptr ? "handshake protocol exceeds uint16 range" : error->message);
    }

    TransportHandshakePacket packet;
    packet.kind = kind.value();
    packet.protocol_version = static_cast<std::uint16_t>(protocol.value());
    packet.client_nonce = nonce.value();
    packet.cookie = cookie.value();
    packet.cookie_time_bucket = bucket.value();
    packet.server_id = core::NetId::from_value(server_id.value());
    packet.assigned_client_id = core::NetId::from_value(client_id.value());
    packet.session_token = token.value();
    packet.content_fingerprint = fingerprint->second;
    packet.reason_code = reason->second;
    auto status = validate_transport_handshake_packet(packet);
    if (!status) {
        return core::Result<TransportHandshakePacket>::failure(status.error().code,
                                                               status.error().message);
    }
    return core::Result<TransportHandshakePacket>::success(std::move(packet));
}

core::Status
validate_transport_handshake_packet(const TransportHandshakePacket& packet) {
    if (packet.protocol_version != transport_handshake_protocol_version) {
        return core::Status::failure("transport_handshake.unsupported_protocol",
                                     "handshake protocol version is unsupported");
    }
    if (!valid_simple_text(packet.content_fingerprint, maximum_fingerprint_bytes, true) ||
        !valid_simple_text(packet.reason_code, maximum_reason_code_bytes, true)) {
        return core::Status::failure("transport_handshake.invalid_text",
                                     "handshake text field is invalid or too long");
    }
    switch (packet.kind) {
    case TransportHandshakeKind::client_hello:
        if (!packet.client_nonce.is_valid()) {
            return core::Status::failure("transport_handshake.invalid_nonce",
                                         "client hello nonce must be non-zero");
        }
        break;
    case TransportHandshakeKind::server_challenge:
    case TransportHandshakeKind::client_response:
        if (!packet.client_nonce.is_valid() || !packet.cookie.is_valid() ||
            packet.cookie_time_bucket == 0) {
            return core::Status::failure("transport_handshake.invalid_challenge",
                                         "handshake challenge fields must be non-zero");
        }
        break;
    case TransportHandshakeKind::server_accept:
        if (!packet.server_id.is_valid() || !packet.assigned_client_id.is_valid() ||
            packet.server_id == packet.assigned_client_id ||
            !packet.session_token.is_valid()) {
            return core::Status::failure("transport_handshake.invalid_accept",
                                         "server accept identity or token is invalid");
        }
        break;
    case TransportHandshakeKind::server_reject:
        if (packet.reason_code.empty()) {
            return core::Status::failure("transport_handshake.invalid_reject",
                                         "server rejection requires a reason code");
        }
        break;
    }
    return core::Status::ok();
}

core::Result<TransportSessionToken> secure_random_transport_token() {
    TransportSessionToken token;
#if defined(__linux__)
    std::array<std::byte, sizeof(token)> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto received =
            ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (received <= 0) {
            return core::Result<TransportSessionToken>::failure(
                "transport_security.random_failed",
                "operating system secure random generation failed");
        }
        offset += static_cast<std::size_t>(received);
    }
    std::memcpy(&token, bytes.data(), sizeof(token));
#elif defined(__APPLE__)
    ::arc4random_buf(&token, sizeof(token));
#else
    return core::Result<TransportSessionToken>::failure(
        "transport_security.random_unavailable",
        "secure random transport tokens are unavailable on this platform");
#endif
    if (!token.is_valid()) {
        return secure_random_transport_token();
    }
    return core::Result<TransportSessionToken>::success(token);
}

TransportSessionToken
make_transport_address_cookie(TransportSessionToken secret, std::uint64_t endpoint_scope,
                              TransportSessionToken client_nonce,
                              std::string_view content_fingerprint,
                              std::uint64_t time_bucket) noexcept {
    std::array<std::byte, 8 * 5 + maximum_fingerprint_bytes + 1> input{};
    std::size_t size = 0;
    const auto append = [&input, &size](std::uint64_t value) {
        for (unsigned index = 0; index < 8; ++index) {
            input[size++] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
        }
    };
    append(endpoint_scope);
    append(client_nonce.high);
    append(client_nonce.low);
    append(time_bucket);
    append(static_cast<std::uint64_t>(content_fingerprint.size()));
    for (const auto character : content_fingerprint) {
        input[size++] = static_cast<std::byte>(static_cast<unsigned char>(character));
    }
    input[size++] = std::byte{0};
    const auto high = siphash24(secret, std::span<const std::byte>(input.data(), size));
    input[size - 1] = std::byte{1};
    const TransportSessionToken second_key{
        secret.high ^ 0xa5a5a5a5a5a5a5a5ULL,
        secret.low ^ 0x5a5a5a5a5a5a5a5aULL,
    };
    const auto low = siphash24(second_key, std::span<const std::byte>(input.data(), size));
    return {high, low};
}

std::string transport_session_token_hex(TransportSessionToken token) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(32, '0');
    const std::array words{token.high, token.low};
    std::size_t offset = 0;
    for (const auto word : words) {
        for (int nibble = 15; nibble >= 0; --nibble) {
            result[offset++] = digits[(word >> (static_cast<unsigned>(nibble) * 4U)) & 0x0fU];
        }
    }
    return result;
}

core::Result<TransportSessionToken>
transport_session_token_from_hex(std::string_view value) {
    if (value.size() != 32) {
        return core::Result<TransportSessionToken>::failure(
            "transport_handshake.invalid_token", "transport token must contain 32 hex digits");
    }
    const auto parse_word = [](std::string_view word) {
        std::uint64_t result = 0;
        const auto [end, error] =
            std::from_chars(word.data(), word.data() + word.size(), result, 16);
        if (error != std::errc{} || end != word.data() + word.size()) {
            return core::Result<std::uint64_t>::failure(
                "transport_handshake.invalid_token",
                "transport token contains invalid hex digits");
        }
        return core::Result<std::uint64_t>::success(result);
    };
    auto high = parse_word(value.substr(0, 16));
    auto low = parse_word(value.substr(16, 16));
    if (!high || !low) {
        const auto& error = !high ? high.error() : low.error();
        return core::Result<TransportSessionToken>::failure(error.code, error.message);
    }
    return core::Result<TransportSessionToken>::success({high.value(), low.value()});
}

std::string_view
transport_handshake_kind_name(TransportHandshakeKind kind) noexcept {
    switch (kind) {
    case TransportHandshakeKind::client_hello:
        return "client_hello";
    case TransportHandshakeKind::server_challenge:
        return "server_challenge";
    case TransportHandshakeKind::client_response:
        return "client_response";
    case TransportHandshakeKind::server_accept:
        return "server_accept";
    case TransportHandshakeKind::server_reject:
        return "server_reject";
    }
    return "unknown";
}

} // namespace heartstead::net
