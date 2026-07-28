#include "engine/net/transport_packet.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace heartstead::net {

namespace {

constexpr std::string_view packet_magic_v1 = "heartstead.transport.v1";
constexpr std::string_view packet_magic_v2 = "heartstead.transport.v2";
constexpr std::string_view fragment_magic = "heartstead.transport.fragment.v1";
constexpr std::string_view payload_marker = "\npayload:\n";

[[nodiscard]] core::Result<std::uint64_t>
parse_u64(std::string_view value, std::string_view field_name,
          std::string_view error_prefix = "transport_packet") {
    std::uint64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return core::Result<std::uint64_t>::failure(std::string(error_prefix) + ".invalid_number",
                                                    "invalid numeric packet field: " +
                                                        std::string(field_name));
    }
    return core::Result<std::uint64_t>::success(parsed);
}

[[nodiscard]] core::Result<std::int64_t>
parse_i64(std::string_view value, std::string_view field_name,
          std::string_view error_prefix = "transport_packet") {
    std::int64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return core::Result<std::int64_t>::failure(std::string(error_prefix) + ".invalid_number",
                                                   "invalid numeric packet field: " +
                                                       std::string(field_name));
    }
    return core::Result<std::int64_t>::success(parsed);
}

[[nodiscard]] core::Result<std::uint32_t>
parse_u32(std::string_view value, std::string_view field_name, std::string_view error_prefix) {
    auto parsed = parse_u64(value, field_name, error_prefix);
    if (!parsed) {
        return core::Result<std::uint32_t>::failure(parsed.error().code, parsed.error().message);
    }
    if (parsed.value() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<std::uint32_t>::failure(
            std::string(error_prefix) + ".number_out_of_range",
            "numeric packet field exceeds uint32 range: " + std::string(field_name));
    }
    return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(parsed.value()));
}

[[nodiscard]] core::Result<TransportMessageKind> parse_kind(std::string_view value) {
    if (value == transport_message_kind_name(TransportMessageKind::command)) {
        return core::Result<TransportMessageKind>::success(TransportMessageKind::command);
    }
    if (value == transport_message_kind_name(TransportMessageKind::command_result)) {
        return core::Result<TransportMessageKind>::success(TransportMessageKind::command_result);
    }
    if (value == transport_message_kind_name(TransportMessageKind::replication)) {
        return core::Result<TransportMessageKind>::success(TransportMessageKind::replication);
    }
    if (value == transport_message_kind_name(TransportMessageKind::control)) {
        return core::Result<TransportMessageKind>::success(TransportMessageKind::control);
    }
    return core::Result<TransportMessageKind>::failure("transport_packet.invalid_kind",
                                                       "unknown transport message kind");
}

[[nodiscard]] core::Result<TransportChannel> parse_channel(std::string_view value) {
    if (value == transport_channel_name(TransportChannel::reliable)) {
        return core::Result<TransportChannel>::success(TransportChannel::reliable);
    }
    if (value == transport_channel_name(TransportChannel::unreliable)) {
        return core::Result<TransportChannel>::success(TransportChannel::unreliable);
    }
    return core::Result<TransportChannel>::failure("transport_packet.invalid_channel",
                                                   "unknown transport channel");
}

[[nodiscard]] core::Result<std::map<std::string, std::string>>
parse_header_fields(std::string_view header, std::string_view expected_magic,
                    std::string_view error_prefix, std::string_view packet_name) {
    std::size_t line_start = 0;
    bool saw_magic = false;
    std::map<std::string, std::string> fields;

    while (line_start <= header.size()) {
        const auto line_end = header.find('\n', line_start);
        auto line = line_end == std::string_view::npos
                        ? header.substr(line_start)
                        : header.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (!saw_magic) {
            if (line != expected_magic) {
                return core::Result<std::map<std::string, std::string>>::failure(
                    std::string(error_prefix) + ".invalid_magic",
                    std::string(packet_name) + " does not start with expected magic");
            }
            saw_magic = true;
        } else if (!line.empty()) {
            const auto separator = line.find('=');
            if (separator == std::string_view::npos || separator == 0) {
                return core::Result<std::map<std::string, std::string>>::failure(
                    std::string(error_prefix) + ".invalid_line",
                    std::string(packet_name) + " header line must use key=value syntax");
            }
            auto key = std::string(line.substr(0, separator));
            auto value = std::string(line.substr(separator + 1));
            const auto [_, inserted] = fields.emplace(std::move(key), std::move(value));
            if (!inserted) {
                return core::Result<std::map<std::string, std::string>>::failure(
                    std::string(error_prefix) + ".duplicate_key",
                    std::string(packet_name) + " header key is duplicated");
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }

    if (!saw_magic) {
        return core::Result<std::map<std::string, std::string>>::failure(
            std::string(error_prefix) + ".invalid_magic",
            std::string(packet_name) + " is missing magic");
    }
    return core::Result<std::map<std::string, std::string>>::success(std::move(fields));
}

[[nodiscard]] core::Result<std::string_view>
require_field(const std::map<std::string, std::string>& fields, std::string_view key,
              std::string_view error_prefix, std::string_view packet_name) {
    const auto found = fields.find(std::string(key));
    if (found == fields.end() || found->second.empty()) {
        return core::Result<std::string_view>::failure(
            std::string(error_prefix) + ".missing_key",
            std::string(packet_name) + " is missing required header key: " + std::string(key));
    }
    return core::Result<std::string_view>::success(found->second);
}

} // namespace

std::string TransportPacketCodec::encode(const TransportEnvelope& envelope) {
    std::ostringstream output;
    output << (envelope.session_token.is_valid() ? packet_magic_v2 : packet_magic_v1) << '\n';
    output << "sender=" << envelope.sender.value() << '\n';
    output << "recipient=" << envelope.recipient.value() << '\n';
    if (envelope.session_token.is_valid()) {
        output << "token_high=" << envelope.session_token.high << '\n';
        output << "token_low=" << envelope.session_token.low << '\n';
    }
    output << "kind=" << transport_message_kind_name(envelope.message.kind) << '\n';
    output << "channel=" << transport_channel_name(envelope.message.channel) << '\n';
    output << "sequence=" << envelope.message.sequence << '\n';
    output << "payload_type=" << envelope.message.payload_type << '\n';
    output << "timestamp=" << envelope.message.timestamp_ms << '\n';
    output << "payload_size=" << envelope.message.payload.size() << '\n';
    output << "payload:\n";
    auto packet = output.str();
    packet.append(envelope.message.payload);
    return packet;
}

core::Result<TransportEnvelope> TransportPacketCodec::decode(std::string_view packet,
                                                             TransportPacketCodecConfig config) {
    if (config.max_payload_bytes == 0 ||
        packet.size() > static_cast<std::size_t>(config.max_payload_bytes) + 4096U) {
        return core::Result<TransportEnvelope>::failure(
            "transport_packet.packet_too_large",
            "transport packet exceeds its payload and bounded header budget");
    }
    const auto marker = packet.find(payload_marker);
    if (marker == std::string_view::npos) {
        return core::Result<TransportEnvelope>::failure(
            "transport_packet.missing_payload_marker",
            "transport packet is missing payload marker");
    }

    const auto header = packet.substr(0, marker);
    const auto is_v2 = header.starts_with(packet_magic_v2);
    auto fields = parse_header_fields(header, is_v2 ? packet_magic_v2 : packet_magic_v1,
                                      "transport_packet", "transport packet");
    if (!fields) {
        return core::Result<TransportEnvelope>::failure(fields.error().code,
                                                        fields.error().message);
    }
    const auto payload = packet.substr(marker + payload_marker.size());
    if (payload.size() > config.max_payload_bytes) {
        return core::Result<TransportEnvelope>::failure("transport.payload_too_large",
                                                        "transport payload exceeds max bytes");
    }

    auto sender_value =
        require_field(fields.value(), "sender", "transport_packet", "transport packet");
    auto recipient_value =
        require_field(fields.value(), "recipient", "transport_packet", "transport packet");
    core::Result<std::string_view> token_high_value =
        core::Result<std::string_view>::success({});
    core::Result<std::string_view> token_low_value =
        core::Result<std::string_view>::success({});
    if (is_v2) {
        token_high_value =
            require_field(fields.value(), "token_high", "transport_packet", "transport packet");
        token_low_value =
            require_field(fields.value(), "token_low", "transport_packet", "transport packet");
    }
    auto kind_value = require_field(fields.value(), "kind", "transport_packet", "transport packet");
    auto channel_value =
        require_field(fields.value(), "channel", "transport_packet", "transport packet");
    auto sequence_value =
        require_field(fields.value(), "sequence", "transport_packet", "transport packet");
    auto payload_type_value =
        require_field(fields.value(), "payload_type", "transport_packet", "transport packet");
    auto timestamp_value =
        require_field(fields.value(), "timestamp", "transport_packet", "transport packet");
    auto payload_size_value =
        require_field(fields.value(), "payload_size", "transport_packet", "transport packet");
    if (!sender_value || !recipient_value || !token_high_value || !token_low_value || !kind_value ||
        !channel_value || !sequence_value || !payload_type_value || !timestamp_value ||
        !payload_size_value) {
        return core::Result<TransportEnvelope>::failure(
            "transport_packet.missing_key", "transport packet is missing required header keys");
    }

    auto sender_id = parse_u64(sender_value.value(), "sender");
    auto recipient_id = parse_u64(recipient_value.value(), "recipient");
    auto token_high = core::Result<std::uint64_t>::success(0);
    auto token_low = core::Result<std::uint64_t>::success(0);
    if (is_v2) {
        token_high = parse_u64(token_high_value.value(), "token_high");
        token_low = parse_u64(token_low_value.value(), "token_low");
    }
    auto kind = parse_kind(kind_value.value());
    auto channel = parse_channel(channel_value.value());
    auto sequence = parse_u64(sequence_value.value(), "sequence");
    auto timestamp = parse_i64(timestamp_value.value(), "timestamp");
    auto payload_size = parse_u64(payload_size_value.value(), "payload_size");
    if (!sender_id) {
        return core::Result<TransportEnvelope>::failure(sender_id.error().code,
                                                        sender_id.error().message);
    }
    if (!recipient_id) {
        return core::Result<TransportEnvelope>::failure(recipient_id.error().code,
                                                        recipient_id.error().message);
    }
    if (!token_high) {
        return core::Result<TransportEnvelope>::failure(token_high.error().code,
                                                        token_high.error().message);
    }
    if (!token_low) {
        return core::Result<TransportEnvelope>::failure(token_low.error().code,
                                                        token_low.error().message);
    }
    if (!kind) {
        return core::Result<TransportEnvelope>::failure(kind.error().code, kind.error().message);
    }
    if (!channel) {
        return core::Result<TransportEnvelope>::failure(channel.error().code,
                                                        channel.error().message);
    }
    if (!sequence) {
        return core::Result<TransportEnvelope>::failure(sequence.error().code,
                                                        sequence.error().message);
    }
    if (!timestamp) {
        return core::Result<TransportEnvelope>::failure(timestamp.error().code,
                                                        timestamp.error().message);
    }
    if (!payload_size) {
        return core::Result<TransportEnvelope>::failure(payload_size.error().code,
                                                        payload_size.error().message);
    }
    if (payload_size.value() != static_cast<std::uint64_t>(payload.size())) {
        return core::Result<TransportEnvelope>::failure(
            "transport_packet.payload_size_mismatch",
            "transport packet payload size does not match header");
    }

    TransportEnvelope envelope;
    envelope.sender = core::NetId::from_value(sender_id.value());
    envelope.recipient = core::NetId::from_value(recipient_id.value());
    envelope.session_token = {token_high.value(), token_low.value()};
    envelope.message.kind = kind.value();
    envelope.message.channel = channel.value();
    envelope.message.sequence = sequence.value();
    envelope.message.payload_type = std::string(payload_type_value.value());
    envelope.message.payload = std::string(payload);
    envelope.message.timestamp_ms = timestamp.value();

    if (!envelope.sender.is_valid()) {
        return core::Result<TransportEnvelope>::failure("transport_packet.invalid_sender",
                                                        "transport packet sender id is invalid");
    }
    if (!envelope.recipient.is_valid()) {
        return core::Result<TransportEnvelope>::failure("transport_packet.invalid_recipient",
                                                        "transport packet recipient id is invalid");
    }
    if (is_v2 && !envelope.session_token.is_valid()) {
        return core::Result<TransportEnvelope>::failure(
            "transport_packet.invalid_session_token",
            "version 2 transport packets require a non-zero session token");
    }

    auto status = validate_transport_message(envelope.message, config.max_payload_bytes);
    if (!status) {
        return core::Result<TransportEnvelope>::failure(status.error().code,
                                                        status.error().message);
    }
    return core::Result<TransportEnvelope>::success(std::move(envelope));
}

core::Result<std::vector<TransportPacketFragment>>
TransportPacketFragmentCodec::fragment_packet(std::string_view packet, std::uint64_t packet_id,
                                              TransportPacketFragmentCodecConfig config) {
    auto status = validate_config(config);
    if (!status) {
        return core::Result<std::vector<TransportPacketFragment>>::failure(status.error().code,
                                                                           status.error().message);
    }
    if (packet_id == 0) {
        return core::Result<std::vector<TransportPacketFragment>>::failure(
            "transport_fragment.invalid_packet_id", "fragment packet id must be non-zero");
    }
    if (packet.empty()) {
        return core::Result<std::vector<TransportPacketFragment>>::failure(
            "transport_fragment.empty_packet", "transport packet must not be empty");
    }
    if (packet.size() > static_cast<std::size_t>(config.max_packet_bytes)) {
        return core::Result<std::vector<TransportPacketFragment>>::failure(
            "transport_fragment.packet_too_large", "transport packet exceeds max packet bytes");
    }

    const auto packet_size = static_cast<std::uint64_t>(packet.size());
    const auto max_fragment_payload = static_cast<std::uint64_t>(config.max_fragment_payload_bytes);
    const auto fragment_count_u64 =
        (packet_size + max_fragment_payload - 1u) / max_fragment_payload;
    if (fragment_count_u64 == 0 ||
        fragment_count_u64 > static_cast<std::uint64_t>(config.max_fragment_count) ||
        fragment_count_u64 > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<std::vector<TransportPacketFragment>>::failure(
            "transport_fragment.fragment_count_exceeded",
            "transport packet requires too many fragments");
    }

    const auto fragment_count = static_cast<std::uint32_t>(fragment_count_u64);
    std::vector<TransportPacketFragment> fragments;
    fragments.reserve(static_cast<std::size_t>(fragment_count));

    for (std::uint32_t index = 0; index < fragment_count; ++index) {
        const auto start = static_cast<std::size_t>(index) *
                           static_cast<std::size_t>(config.max_fragment_payload_bytes);
        const auto remaining = packet.size() - start;
        const auto chunk_size =
            std::min(remaining, static_cast<std::size_t>(config.max_fragment_payload_bytes));

        TransportPacketFragment fragment;
        fragment.packet_id = packet_id;
        fragment.fragment_index = index;
        fragment.fragment_count = fragment_count;
        fragment.total_packet_bytes = packet_size;
        fragment.payload = std::string(packet.substr(start, chunk_size));
        fragments.push_back(std::move(fragment));
    }

    return core::Result<std::vector<TransportPacketFragment>>::success(std::move(fragments));
}

std::string TransportPacketFragmentCodec::encode(const TransportPacketFragment& fragment) {
    std::ostringstream output;
    output << fragment_magic << '\n';
    output << "packet_id=" << fragment.packet_id << '\n';
    output << "fragment_index=" << fragment.fragment_index << '\n';
    output << "fragment_count=" << fragment.fragment_count << '\n';
    output << "total_packet_bytes=" << fragment.total_packet_bytes << '\n';
    output << "payload_size=" << fragment.payload.size() << '\n';
    output << "payload:\n";
    auto packet = output.str();
    packet.append(fragment.payload);
    return packet;
}

core::Result<TransportPacketFragment>
TransportPacketFragmentCodec::decode(std::string_view fragment_packet,
                                     TransportPacketFragmentCodecConfig config) {
    const auto marker = fragment_packet.find(payload_marker);
    if (marker == std::string_view::npos) {
        return core::Result<TransportPacketFragment>::failure(
            "transport_fragment.missing_payload_marker",
            "transport fragment is missing payload marker");
    }

    auto fields = parse_header_fields(fragment_packet.substr(0, marker), fragment_magic,
                                      "transport_fragment", "transport fragment");
    if (!fields) {
        return core::Result<TransportPacketFragment>::failure(fields.error().code,
                                                              fields.error().message);
    }
    const auto payload = fragment_packet.substr(marker + payload_marker.size());

    auto packet_id_value =
        require_field(fields.value(), "packet_id", "transport_fragment", "transport fragment");
    auto fragment_index_value =
        require_field(fields.value(), "fragment_index", "transport_fragment", "transport fragment");
    auto fragment_count_value =
        require_field(fields.value(), "fragment_count", "transport_fragment", "transport fragment");
    auto total_packet_bytes_value = require_field(fields.value(), "total_packet_bytes",
                                                  "transport_fragment", "transport fragment");
    auto payload_size_value =
        require_field(fields.value(), "payload_size", "transport_fragment", "transport fragment");
    if (!packet_id_value || !fragment_index_value || !fragment_count_value ||
        !total_packet_bytes_value || !payload_size_value) {
        return core::Result<TransportPacketFragment>::failure(
            "transport_fragment.missing_key", "transport fragment is missing required header keys");
    }

    auto packet_id = parse_u64(packet_id_value.value(), "packet_id", "transport_fragment");
    auto fragment_index =
        parse_u32(fragment_index_value.value(), "fragment_index", "transport_fragment");
    auto fragment_count =
        parse_u32(fragment_count_value.value(), "fragment_count", "transport_fragment");
    auto total_packet_bytes =
        parse_u64(total_packet_bytes_value.value(), "total_packet_bytes", "transport_fragment");
    auto payload_size = parse_u64(payload_size_value.value(), "payload_size", "transport_fragment");
    if (!packet_id) {
        return core::Result<TransportPacketFragment>::failure(packet_id.error().code,
                                                              packet_id.error().message);
    }
    if (!fragment_index) {
        return core::Result<TransportPacketFragment>::failure(fragment_index.error().code,
                                                              fragment_index.error().message);
    }
    if (!fragment_count) {
        return core::Result<TransportPacketFragment>::failure(fragment_count.error().code,
                                                              fragment_count.error().message);
    }
    if (!total_packet_bytes) {
        return core::Result<TransportPacketFragment>::failure(total_packet_bytes.error().code,
                                                              total_packet_bytes.error().message);
    }
    if (!payload_size) {
        return core::Result<TransportPacketFragment>::failure(payload_size.error().code,
                                                              payload_size.error().message);
    }
    if (payload_size.value() != static_cast<std::uint64_t>(payload.size())) {
        return core::Result<TransportPacketFragment>::failure(
            "transport_fragment.payload_size_mismatch",
            "transport fragment payload size does not match header");
    }

    TransportPacketFragment fragment;
    fragment.packet_id = packet_id.value();
    fragment.fragment_index = fragment_index.value();
    fragment.fragment_count = fragment_count.value();
    fragment.total_packet_bytes = total_packet_bytes.value();
    fragment.payload = std::string(payload);

    auto status = validate_fragment(fragment, config);
    if (!status) {
        return core::Result<TransportPacketFragment>::failure(status.error().code,
                                                              status.error().message);
    }
    return core::Result<TransportPacketFragment>::success(std::move(fragment));
}

core::Status
TransportPacketFragmentCodec::validate_config(const TransportPacketFragmentCodecConfig& config) {
    if (config.max_fragment_payload_bytes == 0) {
        return core::Status::failure("transport_fragment.invalid_config",
                                     "max fragment payload bytes must be non-zero");
    }
    if (config.max_packet_bytes == 0) {
        return core::Status::failure("transport_fragment.invalid_config",
                                     "max packet bytes must be non-zero");
    }
    if (config.max_fragment_count == 0) {
        return core::Status::failure("transport_fragment.invalid_config",
                                     "max fragment count must be non-zero");
    }
    if (config.max_pending_packets == 0 || config.max_pending_packet_bytes == 0 ||
        config.max_pending_packet_bytes < config.max_packet_bytes) {
        return core::Status::failure(
            "transport_fragment.invalid_config",
            "reassembly pending budgets must hold at least one maximum-size packet");
    }
    if (config.pending_packet_timeout_ms == 0) {
        return core::Status::failure("transport_fragment.invalid_config",
                                     "reassembly pending packet timeout must be non-zero");
    }
    return core::Status::ok();
}

core::Status
TransportPacketFragmentCodec::validate_fragment(const TransportPacketFragment& fragment,
                                                TransportPacketFragmentCodecConfig config) {
    auto config_status = validate_config(config);
    if (!config_status) {
        return config_status;
    }
    if (fragment.packet_id == 0) {
        return core::Status::failure("transport_fragment.invalid_packet_id",
                                     "fragment packet id must be non-zero");
    }
    if (fragment.fragment_count == 0) {
        return core::Status::failure("transport_fragment.invalid_fragment_count",
                                     "fragment count must be non-zero");
    }
    if (fragment.fragment_count > config.max_fragment_count) {
        return core::Status::failure("transport_fragment.fragment_count_exceeded",
                                     "fragment count exceeds configured max fragment count");
    }
    if (fragment.fragment_index >= fragment.fragment_count) {
        return core::Status::failure("transport_fragment.invalid_fragment_index",
                                     "fragment index must be less than fragment count");
    }
    if (fragment.total_packet_bytes == 0) {
        return core::Status::failure("transport_fragment.invalid_total_size",
                                     "fragment total packet bytes must be non-zero");
    }
    if (fragment.total_packet_bytes > static_cast<std::uint64_t>(config.max_packet_bytes)) {
        return core::Status::failure("transport_fragment.packet_too_large",
                                     "fragment total packet bytes exceeds max packet bytes");
    }
    if (fragment.payload.empty()) {
        return core::Status::failure("transport_fragment.empty_payload",
                                     "fragment payload must not be empty");
    }
    if (fragment.payload.size() > static_cast<std::size_t>(config.max_fragment_payload_bytes)) {
        return core::Status::failure("transport_fragment.payload_too_large",
                                     "fragment payload exceeds max fragment payload bytes");
    }
    if (static_cast<std::uint64_t>(fragment.payload.size()) > fragment.total_packet_bytes) {
        return core::Status::failure("transport_fragment.payload_too_large",
                                     "fragment payload exceeds total packet bytes");
    }
    if (fragment.total_packet_bytes < static_cast<std::uint64_t>(fragment.fragment_count)) {
        return core::Status::failure(
            "transport_fragment.impossible_size",
            "fragment metadata cannot represent a packet with non-empty fragments");
    }
    const auto max_reassembled_size = static_cast<std::uint64_t>(fragment.fragment_count) *
                                      static_cast<std::uint64_t>(config.max_fragment_payload_bytes);
    if (fragment.total_packet_bytes > max_reassembled_size) {
        return core::Status::failure(
            "transport_fragment.impossible_size",
            "fragment metadata cannot cover total packet bytes with configured payload size");
    }
    return core::Status::ok();
}

TransportPacketReassembler::TransportPacketReassembler(TransportPacketFragmentCodecConfig config)
    : config_(config) {}

std::size_t TransportPacketReassembler::PendingPacketKeyHash::operator()(
    const PendingPacketKey& key) const noexcept {
    const auto source_hash = std::hash<std::uint64_t>{}(key.source_scope);
    const auto packet_hash = std::hash<std::uint64_t>{}(key.packet_id);
    return source_hash ^ (packet_hash + 0x9e3779b9U + (source_hash << 6U) + (source_hash >> 2U));
}

core::Result<TransportPacketReassemblyResult>
TransportPacketReassembler::accept_fragment(TransportPacketFragment fragment) {
    return accept_fragment(0, std::move(fragment), 0);
}

core::Result<TransportPacketReassemblyResult>
TransportPacketReassembler::accept_fragment(std::uint64_t source_scope,
                                            TransportPacketFragment fragment, std::int64_t now_ms) {
    (void)expire(now_ms);
    auto status = TransportPacketFragmentCodec::validate_fragment(fragment, config_);
    if (!status) {
        return core::Result<TransportPacketReassemblyResult>::failure(status.error().code,
                                                                      status.error().message);
    }

    const PendingPacketKey key{source_scope, fragment.packet_id};
    auto [iterator, inserted] = pending_.try_emplace(key);
    auto& pending = iterator->second;
    if (inserted) {
        if (pending_.size() > config_.max_pending_packets ||
            fragment.total_packet_bytes >
                config_.max_pending_packet_bytes -
                    std::min(config_.max_pending_packet_bytes, pending_packet_bytes_)) {
            pending_.erase(iterator);
            return core::Result<TransportPacketReassemblyResult>::failure(
                "transport_fragment.pending_budget_exceeded",
                "too many incomplete fragmented packets are pending");
        }
        pending.total_packet_bytes = fragment.total_packet_bytes;
        pending.fragment_count = fragment.fragment_count;
        pending.fragments.resize(static_cast<std::size_t>(fragment.fragment_count));
        pending.received.resize(static_cast<std::size_t>(fragment.fragment_count), false);
        pending.last_fragment_ms = now_ms;
        pending_packet_bytes_ += fragment.total_packet_bytes;
    } else if (pending.total_packet_bytes != fragment.total_packet_bytes ||
               pending.fragment_count != fragment.fragment_count) {
        pending_packet_bytes_ -= pending.total_packet_bytes;
        pending_.erase(iterator);
        return core::Result<TransportPacketReassemblyResult>::failure(
            "transport_fragment.mismatched_packet_metadata",
            "fragment metadata does not match the pending packet");
    }
    pending.last_fragment_ms = now_ms;

    const auto fragment_index = static_cast<std::size_t>(fragment.fragment_index);
    if (pending.received[fragment_index]) {
        if (pending.fragments[fragment_index] != fragment.payload) {
            pending_packet_bytes_ -= pending.total_packet_bytes;
            pending_.erase(iterator);
            return core::Result<TransportPacketReassemblyResult>::failure(
                "transport_fragment.conflicting_duplicate",
                "duplicate fragment payload does not match the pending packet");
        }
        return core::Result<TransportPacketReassemblyResult>::success(
            TransportPacketReassemblyResult{false,
                                            fragment.packet_id,
                                            pending.received_fragment_count,
                                            pending.fragment_count,
                                            {}});
    }

    pending.fragments[fragment_index] = std::move(fragment.payload);
    pending.received[fragment_index] = true;
    ++pending.received_fragment_count;

    if (pending.received_fragment_count != pending.fragment_count) {
        return core::Result<TransportPacketReassemblyResult>::success(
            TransportPacketReassemblyResult{false,
                                            fragment.packet_id,
                                            pending.received_fragment_count,
                                            pending.fragment_count,
                                            {}});
    }

    std::string packet;
    packet.reserve(static_cast<std::size_t>(pending.total_packet_bytes));
    for (const auto& piece : pending.fragments) {
        packet.append(piece);
    }
    const auto expected_size = pending.total_packet_bytes;
    const auto received_count = pending.received_fragment_count;
    const auto expected_count = pending.fragment_count;
    pending_packet_bytes_ -= pending.total_packet_bytes;
    pending_.erase(iterator);

    if (static_cast<std::uint64_t>(packet.size()) != expected_size) {
        return core::Result<TransportPacketReassemblyResult>::failure(
            "transport_fragment.reassembled_size_mismatch",
            "reassembled packet size does not match fragment metadata");
    }

    return core::Result<TransportPacketReassemblyResult>::success(TransportPacketReassemblyResult{
        true, fragment.packet_id, received_count, expected_count, std::move(packet)});
}

std::size_t TransportPacketReassembler::expire(std::int64_t now_ms) {
    std::size_t expired_count = 0;
    for (auto iterator = pending_.begin(); iterator != pending_.end();) {
        const auto last_fragment_ms = iterator->second.last_fragment_ms;
        const auto elapsed_ms =
            static_cast<std::uint64_t>(now_ms) - static_cast<std::uint64_t>(last_fragment_ms);
        const bool expired =
            now_ms >= last_fragment_ms && elapsed_ms >= config_.pending_packet_timeout_ms;
        if (!expired) {
            ++iterator;
            continue;
        }
        pending_packet_bytes_ -= iterator->second.total_packet_bytes;
        iterator = pending_.erase(iterator);
        ++expired_count;
    }
    return expired_count;
}

std::size_t TransportPacketReassembler::pending_packet_count() const noexcept {
    return pending_.size();
}

std::uint64_t TransportPacketReassembler::pending_packet_bytes() const noexcept {
    return pending_packet_bytes_;
}

void TransportPacketReassembler::discard(std::uint64_t packet_id) {
    discard(0, packet_id);
}

void TransportPacketReassembler::discard(std::uint64_t source_scope, std::uint64_t packet_id) {
    const auto found = pending_.find(PendingPacketKey{source_scope, packet_id});
    if (found != pending_.end()) {
        pending_packet_bytes_ -= found->second.total_packet_bytes;
        pending_.erase(found);
    }
}

void TransportPacketReassembler::discard_source(std::uint64_t source_scope) {
    for (auto iterator = pending_.begin(); iterator != pending_.end();) {
        if (iterator->first.source_scope != source_scope) {
            ++iterator;
            continue;
        }
        pending_packet_bytes_ -= iterator->second.total_packet_bytes;
        iterator = pending_.erase(iterator);
    }
}

void TransportPacketReassembler::clear() {
    pending_.clear();
    pending_packet_bytes_ = 0;
}

} // namespace heartstead::net
