#include "engine/net/transport_reliability.hpp"

#include "engine/net/command_payload.hpp"

#include <charconv>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace heartstead::net {

namespace {

[[nodiscard]] core::Result<std::uint64_t> parse_u64(std::string_view value,
                                                    std::string_view field_name) {
    std::uint64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return core::Result<std::uint64_t>::failure(
            "transport_reliability.invalid_number",
            "invalid reliability acknowledgement number field: " + std::string(field_name));
    }
    return core::Result<std::uint64_t>::success(parsed);
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
    return core::Result<TransportMessageKind>::failure(
        "transport_reliability.invalid_kind", "unknown reliability acknowledgement message kind");
}

[[nodiscard]] core::Result<TransportReliableMessageKey> fail_key_from(const core::Error& error) {
    return core::Result<TransportReliableMessageKey>::failure(error.code, error.message);
}

[[nodiscard]] bool is_ack_message(const TransportMessage& message) noexcept {
    return message.kind == TransportMessageKind::control &&
           message.payload_type == transport_reliability_ack_payload_type;
}

[[nodiscard]] bool allows_zero_sequence(const TransportReliableMessageKey& key) noexcept {
    return key.kind == TransportMessageKind::control;
}

} // namespace

std::string TransportReliabilityAckTextCodec::encode(const TransportReliableMessageKey& message) {
    CommandPayload payload;
    (void)payload.set("sender", std::to_string(message.sender.value()));
    (void)payload.set("recipient", std::to_string(message.recipient.value()));
    (void)payload.set("kind", std::string(transport_message_kind_name(message.kind)));
    (void)payload.set("sequence", std::to_string(message.sequence));
    (void)payload.set("payload_type", message.payload_type);
    return CommandPayloadTextCodec::encode(payload);
}

core::Result<TransportReliableMessageKey>
TransportReliabilityAckTextCodec::decode(std::string_view text) {
    auto payload = CommandPayloadTextCodec::decode(text);
    if (!payload) {
        return core::Result<TransportReliableMessageKey>::failure(payload.error().code,
                                                                  payload.error().message);
    }

    auto sender_value = payload.value().require("sender");
    auto recipient_value = payload.value().require("recipient");
    auto kind_value = payload.value().require("kind");
    auto sequence_value = payload.value().require("sequence");
    auto payload_type_value = payload.value().require("payload_type");
    if (!sender_value || !recipient_value || !kind_value || !sequence_value ||
        !payload_type_value) {
        return core::Result<TransportReliableMessageKey>::failure(
            "transport_reliability.missing_key",
            "reliability acknowledgement payload is missing required fields");
    }

    auto sender = parse_u64(sender_value.value(), "sender");
    auto recipient = parse_u64(recipient_value.value(), "recipient");
    auto kind = parse_kind(kind_value.value());
    auto sequence = parse_u64(sequence_value.value(), "sequence");
    if (!sender) {
        return fail_key_from(sender.error());
    }
    if (!recipient) {
        return fail_key_from(recipient.error());
    }
    if (!kind) {
        return fail_key_from(kind.error());
    }
    if (!sequence) {
        return fail_key_from(sequence.error());
    }

    TransportReliableMessageKey key;
    key.sender = core::NetId::from_value(sender.value());
    key.recipient = core::NetId::from_value(recipient.value());
    key.kind = kind.value();
    key.sequence = sequence.value();
    key.payload_type = std::string(payload_type_value.value());

    auto status = validate_transport_reliable_message_key(key);
    if (!status) {
        return core::Result<TransportReliableMessageKey>::failure(status.error().code,
                                                                  status.error().message);
    }
    return core::Result<TransportReliableMessageKey>::success(std::move(key));
}

TransportReliableCommandSequencer::TransportReliableCommandSequencer(
    std::uint32_t max_pending_commands)
    : max_pending_commands_(max_pending_commands) {}

core::Status TransportReliableCommandSequencer::preflight(const TransportEnvelope& envelope) const {
    if (max_pending_commands_ == 0) {
        return core::Status::failure("transport_reliability.invalid_sequence_buffer_limit",
                                     "reliable command sequence buffer limit must be non-zero");
    }
    if (envelope.message.kind != TransportMessageKind::command ||
        envelope.message.channel != TransportChannel::reliable || envelope.message.sequence == 0) {
        return core::Status::failure(
            "transport_reliability.invalid_sequenced_command",
            "ordered reliable delivery accepts non-zero reliable command messages only");
    }
    if (next_expected_sequence_ == 0) {
        return core::Status::failure(
            "transport_reliability.sequence_exhausted",
            "ordered reliable command delivery exhausted its sequence space");
    }

    const auto sequence = envelope.message.sequence;
    if (sequence <= next_expected_sequence_ || pending_.contains(sequence)) {
        return core::Status::ok();
    }
    if (sequence - next_expected_sequence_ > max_pending_commands_) {
        return core::Status::failure(
            "transport_reliability.sequence_window_exceeded",
            "reliable command sequence is beyond the bounded reorder window");
    }
    if (pending_.size() >= static_cast<std::size_t>(max_pending_commands_)) {
        return core::Status::failure("transport_reliability.sequence_buffer_full",
                                     "reliable command reorder buffer reached its capacity");
    }
    return core::Status::ok();
}

core::Result<std::vector<TransportEnvelope>>
TransportReliableCommandSequencer::accept(TransportEnvelope envelope) {
    auto status = preflight(envelope);
    if (!status) {
        return core::Result<std::vector<TransportEnvelope>>::failure(status.error().code,
                                                                     status.error().message);
    }

    const auto sequence = envelope.message.sequence;
    if (sequence < next_expected_sequence_ || pending_.contains(sequence)) {
        return core::Result<std::vector<TransportEnvelope>>::success(
            std::vector<TransportEnvelope>{});
    }
    pending_.emplace(sequence, std::move(envelope));

    std::vector<TransportEnvelope> ready;
    while (next_expected_sequence_ != 0) {
        auto pending = pending_.find(next_expected_sequence_);
        if (pending == pending_.end()) {
            break;
        }
        ready.push_back(std::move(pending->second));
        pending_.erase(pending);
        next_expected_sequence_ =
            next_expected_sequence_ == std::numeric_limits<std::uint64_t>::max()
                ? 0
                : next_expected_sequence_ + 1;
    }
    return core::Result<std::vector<TransportEnvelope>>::success(std::move(ready));
}

std::uint64_t TransportReliableCommandSequencer::next_expected_sequence() const noexcept {
    return next_expected_sequence_;
}

std::size_t TransportReliableCommandSequencer::pending_count() const noexcept {
    return pending_.size();
}

void TransportReliableCommandSequencer::clear() {
    pending_.clear();
    next_expected_sequence_ = 1;
}

TransportReliabilityTracker::TransportReliabilityTracker(core::NetId local_id,
                                                         core::NetId remote_id,
                                                         TransportReliabilityConfig config)
    : local_id_(local_id), remote_id_(remote_id), config_(config) {}

core::NetId TransportReliabilityTracker::local_id() const noexcept {
    return local_id_;
}

core::NetId TransportReliabilityTracker::remote_id() const noexcept {
    return remote_id_;
}

const TransportReliabilityConfig& TransportReliabilityTracker::config() const noexcept {
    return config_;
}

std::size_t TransportReliabilityTracker::pending_count() const noexcept {
    return pending_.size();
}

std::size_t TransportReliabilityTracker::tracked_received_count() const noexcept {
    return received_.size();
}

core::Status TransportReliabilityTracker::track_send(TransportEnvelope envelope,
                                                     std::int64_t now_ms) {
    auto status = validate_endpoint_ids();
    if (!status) {
        return status;
    }
    status = validate_transport_reliability_config(config_);
    if (!status) {
        return status;
    }
    if (envelope.sender != local_id_ || envelope.recipient != remote_id_) {
        return core::Status::failure(
            "transport_reliability.endpoint_mismatch",
            "reliable send envelope endpoints must match the tracker local and remote ids");
    }
    if (is_ack_message(envelope.message)) {
        return core::Status::failure(
            "transport_reliability.ack_not_tracked",
            "reliability acknowledgement messages are sent best-effort and must not be tracked");
    }

    auto key = transport_reliable_message_key_from_envelope(envelope);
    if (!key) {
        return core::Status::failure(key.error().code, key.error().message);
    }
    const auto id = key_id(key.value());
    if (pending_.contains(id)) {
        return core::Status::failure("transport_reliability.duplicate_pending",
                                     "reliable transport message is already pending");
    }
    if (pending_.size() >= static_cast<std::size_t>(config_.max_in_flight)) {
        return core::Status::failure("transport_reliability.in_flight_limit_reached",
                                     "reliable transport in-flight message limit was reached");
    }

    pending_.emplace(id, TransportReliablePendingMessage{std::move(envelope), 1, now_ms, now_ms});
    return core::Status::ok();
}

core::Status TransportReliabilityTracker::rollback_tracked_send(const TransportEnvelope& envelope) {
    if (envelope.sender != local_id_ || envelope.recipient != remote_id_) {
        return core::Status::failure(
            "transport_reliability.endpoint_mismatch",
            "reliable send rollback endpoints must match the tracker local and remote ids");
    }
    auto key = transport_reliable_message_key_from_envelope(envelope);
    if (!key) {
        return core::Status::failure(key.error().code, key.error().message);
    }
    if (pending_.erase(key_id(key.value())) == 0) {
        return core::Status::failure(
            "transport_reliability.rollback_missing",
            "reliable send rollback did not match a tracked in-flight message");
    }
    return core::Status::ok();
}

core::Result<TransportReliableReceiveResult>
TransportReliabilityTracker::accept_reliable_message(const TransportEnvelope& envelope,
                                                     std::int64_t now_ms) {
    auto status = validate_endpoint_ids();
    if (!status) {
        return core::Result<TransportReliableReceiveResult>::failure(status.error().code,
                                                                     status.error().message);
    }
    status = validate_transport_reliability_config(config_);
    if (!status) {
        return core::Result<TransportReliableReceiveResult>::failure(status.error().code,
                                                                     status.error().message);
    }
    if (envelope.sender != remote_id_ || envelope.recipient != local_id_) {
        return core::Result<TransportReliableReceiveResult>::failure(
            "transport_reliability.endpoint_mismatch",
            "received reliable envelope endpoints must match the tracker remote and local ids");
    }
    if (is_ack_message(envelope.message)) {
        return core::Result<TransportReliableReceiveResult>::failure(
            "transport_reliability.unexpected_ack",
            "acknowledgement envelopes must be accepted through accept_acknowledgement");
    }

    auto key = transport_reliable_message_key_from_envelope(envelope);
    if (!key) {
        return core::Result<TransportReliableReceiveResult>::failure(key.error().code,
                                                                     key.error().message);
    }
    auto acknowledgement = make_transport_reliability_ack_envelope(envelope, now_ms);
    if (!acknowledgement) {
        return core::Result<TransportReliableReceiveResult>::failure(
            acknowledgement.error().code, acknowledgement.error().message);
    }

    const auto id = key_id(key.value());
    TransportReliableReceiveResult result;
    result.acknowledgement = std::move(acknowledgement).value();
    if (received_.contains(id)) {
        result.duplicate = true;
        return core::Result<TransportReliableReceiveResult>::success(std::move(result));
    }

    remember_received(id);
    result.accepted = true;
    return core::Result<TransportReliableReceiveResult>::success(std::move(result));
}

core::Result<TransportReliableAckResult>
TransportReliabilityTracker::accept_acknowledgement(const TransportEnvelope& envelope) {
    auto status = validate_endpoint_ids();
    if (!status) {
        return core::Result<TransportReliableAckResult>::failure(status.error().code,
                                                                 status.error().message);
    }
    if (envelope.sender != remote_id_ || envelope.recipient != local_id_) {
        return core::Result<TransportReliableAckResult>::failure(
            "transport_reliability.endpoint_mismatch",
            "acknowledgement envelope endpoints must match the tracker remote and local ids");
    }
    if (envelope.message.kind != TransportMessageKind::control ||
        envelope.message.channel != TransportChannel::unreliable ||
        envelope.message.payload_type != transport_reliability_ack_payload_type) {
        return core::Result<TransportReliableAckResult>::failure(
            "transport_reliability.invalid_ack_envelope",
            "reliability acknowledgement must be an unreliable transport control message");
    }

    auto key = TransportReliabilityAckTextCodec::decode(envelope.message.payload);
    if (!key) {
        return core::Result<TransportReliableAckResult>::failure(key.error().code,
                                                                 key.error().message);
    }
    if (key.value().sender != local_id_ || key.value().recipient != remote_id_) {
        return core::Result<TransportReliableAckResult>::failure(
            "transport_reliability.ack_message_mismatch",
            "reliability acknowledgement does not describe a message sent by this tracker");
    }

    const auto id = key_id(key.value());
    const auto erased = pending_.erase(id);

    TransportReliableAckResult result;
    result.matched_pending = erased > 0;
    result.message = std::move(key).value();
    return core::Result<TransportReliableAckResult>::success(std::move(result));
}

TransportReliabilityPollResult TransportReliabilityTracker::poll(std::int64_t now_ms) {
    TransportReliabilityPollResult result;
    std::vector<std::string> erase_ids;

    for (auto& [id, pending] : pending_) {
        const auto elapsed = now_ms - pending.last_sent_ms;
        if (elapsed < static_cast<std::int64_t>(config_.retry_delay_ms)) {
            continue;
        }
        if (pending.attempt_count >= config_.max_attempts) {
            result.dropped.push_back(pending);
            erase_ids.push_back(id);
            continue;
        }

        ++pending.attempt_count;
        pending.last_sent_ms = now_ms;
        result.retransmissions.push_back(pending.envelope);
    }

    for (const auto& id : erase_ids) {
        pending_.erase(id);
    }
    return result;
}

void TransportReliabilityTracker::clear() {
    pending_.clear();
    received_.clear();
    received_order_.clear();
}

core::Status TransportReliabilityTracker::validate_endpoint_ids() const {
    if (!local_id_.is_valid() || !remote_id_.is_valid()) {
        return core::Status::failure("transport_reliability.invalid_endpoint_id",
                                     "reliability tracker endpoint ids must be valid");
    }
    if (local_id_ == remote_id_) {
        return core::Status::failure("transport_reliability.same_endpoint_id",
                                     "reliability tracker local and remote ids must differ");
    }
    return core::Status::ok();
}

std::string TransportReliabilityTracker::key_id(const TransportReliableMessageKey& key) const {
    return transport_reliable_message_key_name(key);
}

void TransportReliabilityTracker::remember_received(std::string key) {
    if (received_.insert(key).second) {
        received_order_.push_back(std::move(key));
    }

    while (received_order_.size() >
           static_cast<std::size_t>(config_.max_tracked_received_messages)) {
        received_.erase(received_order_.front());
        received_order_.erase(received_order_.begin());
    }
}

core::Status
validate_transport_reliability_config(const TransportReliabilityConfig& config) noexcept {
    if (config.retry_delay_ms == 0) {
        return core::Status::failure("transport_reliability.invalid_retry_delay",
                                     "reliable transport retry delay must be non-zero");
    }
    if (config.max_attempts == 0) {
        return core::Status::failure("transport_reliability.invalid_max_attempts",
                                     "reliable transport max attempts must be non-zero");
    }
    if (config.max_in_flight == 0) {
        return core::Status::failure("transport_reliability.invalid_in_flight_limit",
                                     "reliable transport in-flight limit must be non-zero");
    }
    if (config.max_tracked_received_messages == 0) {
        return core::Status::failure(
            "transport_reliability.invalid_received_limit",
            "reliable transport received-message tracking limit must be non-zero");
    }
    return core::Status::ok();
}

core::Status
validate_transport_reliable_message_key(const TransportReliableMessageKey& key) noexcept {
    if (!key.sender.is_valid() || !key.recipient.is_valid()) {
        return core::Status::failure("transport_reliability.invalid_message_endpoint",
                                     "reliable message key endpoints must be valid");
    }
    if (key.sender == key.recipient) {
        return core::Status::failure("transport_reliability.same_message_endpoint",
                                     "reliable message key sender and recipient must differ");
    }
    if (key.sequence == 0 && !allows_zero_sequence(key)) {
        return core::Status::failure("transport_reliability.invalid_sequence",
                                     "reliable non-control message key sequence must be non-zero");
    }
    if (!is_valid_command_payload_key(key.payload_type)) {
        return core::Status::failure("transport_reliability.invalid_payload_type",
                                     "reliable message key payload type is invalid");
    }
    if (key.kind == TransportMessageKind::control &&
        key.payload_type == transport_reliability_ack_payload_type) {
        return core::Status::failure("transport_reliability.ack_key_not_reliable",
                                     "reliability acknowledgement keys are not reliable payloads");
    }
    return core::Status::ok();
}

core::Result<TransportReliableMessageKey>
transport_reliable_message_key_from_envelope(const TransportEnvelope& envelope) {
    if (envelope.message.channel != TransportChannel::reliable) {
        return core::Result<TransportReliableMessageKey>::failure(
            "transport_reliability.unreliable_message",
            "only reliable-channel messages can be tracked for acknowledgement");
    }

    TransportReliableMessageKey key;
    key.sender = envelope.sender;
    key.recipient = envelope.recipient;
    key.kind = envelope.message.kind;
    key.sequence = envelope.message.sequence;
    key.payload_type = envelope.message.payload_type;

    auto status = validate_transport_reliable_message_key(key);
    if (!status) {
        return core::Result<TransportReliableMessageKey>::failure(status.error().code,
                                                                  status.error().message);
    }
    return core::Result<TransportReliableMessageKey>::success(std::move(key));
}

TransportMessage make_transport_reliability_ack_message(const TransportReliableMessageKey& key,
                                                        std::int64_t timestamp_ms) {
    return TransportMessage{
        TransportMessageKind::control,
        TransportChannel::unreliable,
        key.sequence,
        std::string(transport_reliability_ack_payload_type),
        TransportReliabilityAckTextCodec::encode(key),
        timestamp_ms,
    };
}

core::Result<TransportEnvelope>
make_transport_reliability_ack_envelope(const TransportEnvelope& reliable_envelope,
                                        std::int64_t timestamp_ms) {
    auto key = transport_reliable_message_key_from_envelope(reliable_envelope);
    if (!key) {
        return core::Result<TransportEnvelope>::failure(key.error().code, key.error().message);
    }

    TransportEnvelope acknowledgement;
    acknowledgement.sender = reliable_envelope.recipient;
    acknowledgement.recipient = reliable_envelope.sender;
    acknowledgement.message = make_transport_reliability_ack_message(key.value(), timestamp_ms);
    acknowledgement.session_token = reliable_envelope.session_token;
    return core::Result<TransportEnvelope>::success(std::move(acknowledgement));
}

std::string transport_reliable_message_key_name(const TransportReliableMessageKey& key) {
    std::ostringstream output;
    output << key.sender.value() << ">" << key.recipient.value() << ":"
           << transport_message_kind_name(key.kind) << ":" << key.sequence << ":"
           << key.payload_type;
    return output.str();
}

} // namespace heartstead::net
