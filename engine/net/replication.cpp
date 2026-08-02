#include "engine/net/replication.hpp"

#include "engine/net/binary_message.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace heartstead::net {

namespace {

constexpr std::string_view magic = "heartstead.replication_events.v1";
constexpr std::size_t max_replication_bytes = 4U * 1024U * 1024U;
constexpr std::size_t max_replication_line_bytes = 1024U * 1024U;
constexpr std::size_t max_replication_records = 100'000;
constexpr std::size_t max_replication_command_bytes = 128;
constexpr std::size_t max_replication_event_type_bytes = 256;
constexpr std::size_t max_replication_event_message_bytes = 1024U * 1024U;
constexpr std::uint32_t binary_magic = 0x31524548U; // "HER1" in little-endian byte order.

[[nodiscard]] bool is_hex_digit(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

[[nodiscard]] char hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<char>(10 + value - 'a');
    }
    return static_cast<char>(10 + value - 'A');
}

[[nodiscard]] std::string percent_escape(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto value : input) {
        const auto byte = static_cast<unsigned char>(value);
        if (value == '%' || value == '|' || value == '=' || value == '\n' || value == '\r') {
            result.push_back('%');
            result.push_back(hex[(byte >> 4u) & 0x0Fu]);
            result.push_back(hex[byte & 0x0Fu]);
        } else {
            result.push_back(value);
        }
    }
    return result;
}

[[nodiscard]] core::Result<std::string> percent_unescape(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] != '%') {
            result.push_back(input[index]);
            continue;
        }
        if (index + 2 >= input.size() || !is_hex_digit(input[index + 1]) ||
            !is_hex_digit(input[index + 2])) {
            return core::Result<std::string>::failure(
                "replication.invalid_escape", "replication payload contains an invalid escape");
        }
        const auto high = hex_value(input[index + 1]);
        const auto low = hex_value(input[index + 2]);
        result.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }

    return core::Result<std::string>::success(std::move(result));
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view value, char delimiter) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(delimiter, start);
        if (end == std::string_view::npos) {
            result.push_back(value.substr(start));
            break;
        }
        result.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

[[nodiscard]] core::Result<std::uint64_t> parse_u64(std::string_view value,
                                                    std::string_view field_name) {
    std::uint64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return core::Result<std::uint64_t>::failure("replication.invalid_number",
                                                    "invalid numeric replication field: " +
                                                        std::string(field_name));
    }
    return core::Result<std::uint64_t>::success(parsed);
}

[[nodiscard]] core::Result<world::OperationEvent> parse_event(std::string_view value) {
    const auto parts = split(value, '|');
    if (parts.size() != 3) {
        return core::Result<world::OperationEvent>::failure(
            "replication.invalid_event", "replication event must contain type, subject, message");
    }

    auto type = percent_unescape(parts[0]);
    auto subject = parse_u64(parts[1], "event_subject");
    auto message = percent_unescape(parts[2]);
    if (!type) {
        return core::Result<world::OperationEvent>::failure(type.error().code,
                                                            type.error().message);
    }
    if (!subject) {
        return core::Result<world::OperationEvent>::failure(subject.error().code,
                                                            subject.error().message);
    }
    if (!message) {
        return core::Result<world::OperationEvent>::failure(message.error().code,
                                                            message.error().message);
    }

    return core::Result<world::OperationEvent>::success(
        world::OperationEvent{std::move(type).value(), core::SaveId::from_value(subject.value()),
                              std::move(message).value()});
}

[[nodiscard]] const ReplicationInterestRule*
find_interest_rule(const ReplicationRelevancePolicy& policy, core::NetId client_id) noexcept {
    const auto found =
        std::ranges::find_if(policy.client_rules, [client_id](const ReplicationInterestRule& rule) {
            return rule.client_id == client_id;
        });
    return found == policy.client_rules.end() ? nullptr : &*found;
}

[[nodiscard]] const ReplicationPrivateAccessRule*
find_private_access_rule(const ReplicationRelevancePolicy& policy, core::NetId client_id) noexcept {
    const auto found = std::ranges::find_if(policy.private_access_rules,
                                            [client_id](const ReplicationPrivateAccessRule& rule) {
                                                return rule.client_id == client_id;
                                            });
    return found == policy.private_access_rules.end() ? nullptr : &*found;
}

[[nodiscard]] const ReplicationChunkInterestRule*
find_chunk_interest_rule(const ReplicationRelevancePolicy& policy,
                         core::NetId client_id) noexcept {
    const auto found = std::ranges::find_if(
        policy.chunk_interest_rules, [client_id](const ReplicationChunkInterestRule& rule) {
            return rule.client_id == client_id;
        });
    return found == policy.chunk_interest_rules.end() ? nullptr : &*found;
}

[[nodiscard]] bool subject_is_visible_for_rule(const ReplicationInterestRule& rule,
                                               core::SaveId subject) noexcept {
    if (!subject.is_valid()) {
        return rule.receives_global_events;
    }
    return std::ranges::find(rule.visible_subjects, subject) != rule.visible_subjects.end();
}

[[nodiscard]] ReplicationRelevanceDecision
evaluate_client_relevance(const ReplicationRelevancePolicy& policy, const ReplicationBatch& batch,
                          core::NetId client_id) {
    ReplicationRelevanceDecision decision;
    decision.client_id = client_id;

    const auto* rule = find_interest_rule(policy, client_id);
    const auto* chunk_rule = find_chunk_interest_rule(policy, client_id);
    decision.explicit_rule = rule != nullptr || chunk_rule != nullptr;
    for (const auto& event : batch.events) {
        const auto visible = ReplicationRelevance::event_is_visible(policy, client_id, event);
        if (visible) {
            ++decision.relevant_event_count;
        }
        if (!event.routing_chunk.has_value()) {
            continue;
        }
        if (visible) {
            ++decision.relevant_spatial_event_count;
        } else {
            ++decision.filtered_spatial_event_count;
            decision.filtered_spatial_payload_bytes +=
                static_cast<std::uint64_t>(event.type.size() + event.message.size());
        }
    }
    decision.relevant = decision.relevant_event_count > 0;
    if (decision.relevant) {
        decision.reason = decision.relevant_spatial_event_count > 0
                              ? "matched_chunk"
                              : (rule == nullptr ? "broadcast_default" : "matched_subject");
    } else {
        decision.reason = decision.filtered_spatial_event_count > 0
                              ? "filtered_chunk"
                              : (rule == nullptr && !policy.broadcast_by_default
                                     ? "no_interest_rule"
                                     : "filtered_subject");
    }
    return decision;
}

} // namespace

std::uint64_t replication_stream_sequence(const ReplicationBatch& batch) noexcept {
    return batch.replication_sequence != 0 ? batch.replication_sequence : batch.command_sequence;
}

std::uint64_t replication_stream_sequence(const ReplicationRelevanceReport& report) noexcept {
    return report.replication_sequence != 0 ? report.replication_sequence : report.command_sequence;
}

std::string ReplicationTextCodec::encode(const ReplicationBatch& batch) {
    std::ostringstream output;
    output << magic << '\n';
    output << "sequence=" << replication_stream_sequence(batch) << '\n';
    output << "command_sequence=" << batch.command_sequence << '\n';
    output << "source_client=" << batch.source_client_id.value() << '\n';
    output << "command=" << percent_escape(batch.command_type) << '\n';
    for (const auto& event : batch.events) {
        output << "event=" << percent_escape(event.type) << '|' << event.subject.value() << '|'
               << percent_escape(event.message) << '\n';
    }
    for (const auto reserved_id : batch.reserved_ids) {
        output << "reserved=" << reserved_id.value() << '\n';
    }
    output << "end\n";
    return output.str();
}

core::Result<ReplicationBatch> ReplicationTextCodec::decode(std::string_view text) {
    if (text.size() > max_replication_bytes)
        return core::Result<ReplicationBatch>::failure(
            "replication.too_large", "replication payload exceeds its size limit");
    ReplicationBatch batch;
    bool saw_magic = false;
    bool saw_sequence = false;
    bool saw_command_sequence = false;
    bool saw_source_client = false;
    bool saw_command = false;
    bool saw_end = false;
    std::size_t consumed_bytes = 0;
    std::size_t record_count = 0;

    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        const auto line_end = text.find('\n', line_start);
        auto line = line_end == std::string_view::npos
                        ? text.substr(line_start)
                        : text.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.size() > max_replication_line_bytes || ++record_count > max_replication_records)
            return core::Result<ReplicationBatch>::failure(
                "replication.too_large", "replication payload exceeds its record limits");

        if (!saw_magic) {
            if (line != magic) {
                return core::Result<ReplicationBatch>::failure(
                    "replication.invalid_magic",
                    "replication payload does not start with expected magic");
            }
            saw_magic = true;
        } else if (line == "end") {
            saw_end = true;
            consumed_bytes = line_end == std::string_view::npos ? text.size() : line_end + 1;
            break;
        } else if (!line.empty()) {
            const auto separator = line.find('=');
            if (separator == std::string_view::npos) {
                return core::Result<ReplicationBatch>::failure(
                    "replication.invalid_line",
                    "replication payload line is missing key/value separator");
            }

            const auto key = line.substr(0, separator);
            const auto value = line.substr(separator + 1);
            if (key == "sequence") {
                if (saw_sequence)
                    return core::Result<ReplicationBatch>::failure(
                        "replication.duplicate_sequence", "replication sequence is duplicated");
                auto parsed = parse_u64(value, "sequence");
                if (!parsed) {
                    return core::Result<ReplicationBatch>::failure(parsed.error().code,
                                                                   parsed.error().message);
                }
                batch.replication_sequence = parsed.value();
                saw_sequence = true;
            } else if (key == "command_sequence") {
                if (saw_command_sequence)
                    return core::Result<ReplicationBatch>::failure(
                        "replication.duplicate_command_sequence",
                        "replication command sequence is duplicated");
                auto parsed = parse_u64(value, "command_sequence");
                if (!parsed) {
                    return core::Result<ReplicationBatch>::failure(parsed.error().code,
                                                                   parsed.error().message);
                }
                batch.command_sequence = parsed.value();
                saw_command_sequence = true;
            } else if (key == "source_client") {
                if (saw_source_client)
                    return core::Result<ReplicationBatch>::failure(
                        "replication.duplicate_source_client",
                        "replication source client is duplicated");
                auto parsed = parse_u64(value, "source_client");
                if (!parsed) {
                    return core::Result<ReplicationBatch>::failure(parsed.error().code,
                                                                   parsed.error().message);
                }
                batch.source_client_id = core::NetId::from_value(parsed.value());
                saw_source_client = true;
            } else if (key == "command") {
                if (saw_command)
                    return core::Result<ReplicationBatch>::failure(
                        "replication.duplicate_command", "replication command is duplicated");
                auto parsed = percent_unescape(value);
                if (!parsed) {
                    return core::Result<ReplicationBatch>::failure(parsed.error().code,
                                                                   parsed.error().message);
                }
                batch.command_type = std::move(parsed).value();
                saw_command = true;
            } else if (key == "event") {
                auto event = parse_event(value);
                if (!event) {
                    return core::Result<ReplicationBatch>::failure(event.error().code,
                                                                   event.error().message);
                }
                batch.events.push_back(std::move(event).value());
            } else if (key == "reserved") {
                auto parsed = parse_u64(value, "reserved_id");
                if (!parsed) {
                    return core::Result<ReplicationBatch>::failure(parsed.error().code,
                                                                   parsed.error().message);
                }
                batch.reserved_ids.push_back(core::SaveId::from_value(parsed.value()));
            } else {
                return core::Result<ReplicationBatch>::failure("replication.unknown_key",
                                                               "unknown replication payload key: " +
                                                                   std::string(key));
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }

    if (!saw_magic || !saw_sequence || !saw_command || !saw_end) {
        return core::Result<ReplicationBatch>::failure(
            "replication.incomplete", "replication payload is missing required records");
    }
    if (consumed_bytes != text.size())
        return core::Result<ReplicationBatch>::failure(
            "replication.trailing_data", "replication payload contains data after its end marker");
    if (!saw_command_sequence) {
        batch.command_sequence = batch.replication_sequence;
    }
    return core::Result<ReplicationBatch>::success(std::move(batch));
}

std::string ReplicationBinaryCodec::encode(const ReplicationBatch& batch) {
    BinaryMessageWriter writer;
    writer.u32(binary_magic);
    writer.var_u64(replication_stream_sequence(batch));
    writer.var_u64(batch.command_sequence);
    writer.var_u64(batch.source_client_id.value());
    writer.text_var(batch.command_type);
    writer.var_u64(static_cast<std::uint64_t>(batch.events.size()));
    for (const auto& event : batch.events) {
        writer.text_var(event.type);
        writer.var_u64(event.subject.value());
        writer.text_var(event.message);
    }
    writer.var_u64(static_cast<std::uint64_t>(batch.reserved_ids.size()));
    for (const auto id : batch.reserved_ids) {
        writer.var_u64(id.value());
    }
    return writer.take();
}

core::Result<ReplicationBatch> ReplicationBinaryCodec::decode(std::string_view bytes) {
    if (bytes.size() > max_replication_bytes) {
        return core::Result<ReplicationBatch>::failure(
            "replication.too_large", "binary replication payload exceeds its size limit");
    }
    BinaryMessageReader reader(bytes);
    std::uint32_t decoded_magic = 0;
    std::uint64_t event_count = 0;
    ReplicationBatch batch;
    if (!reader.u32(decoded_magic) || decoded_magic != binary_magic ||
        !reader.var_u64(batch.replication_sequence) || !reader.var_u64(batch.command_sequence)) {
        return core::Result<ReplicationBatch>::failure(
            "replication.invalid_binary",
            "binary replication payload has an invalid or truncated header");
    }
    std::uint64_t source_client = 0;
    if (!reader.var_u64(source_client) ||
        !reader.text_var(batch.command_type, max_replication_command_bytes) ||
        !reader.var_u64(event_count) || event_count > max_replication_records) {
        return core::Result<ReplicationBatch>::failure(
            "replication.invalid_binary",
            "binary replication header fields are malformed or outside their bounds");
    }
    batch.source_client_id = core::NetId::from_value(source_client);
    batch.events.reserve(static_cast<std::size_t>(event_count));
    for (std::uint64_t index = 0; index < event_count; ++index) {
        world::OperationEvent event;
        std::uint64_t subject = 0;
        if (!reader.text_var(event.type, max_replication_event_type_bytes) ||
            !reader.var_u64(subject) ||
            !reader.text_var(event.message, max_replication_event_message_bytes)) {
            return core::Result<ReplicationBatch>::failure(
                "replication.invalid_binary",
                "binary replication event is malformed, truncated, or oversized");
        }
        event.subject = core::SaveId::from_value(subject);
        batch.events.push_back(std::move(event));
    }
    std::uint64_t reserved_count = 0;
    if (!reader.var_u64(reserved_count) || reserved_count > max_replication_records) {
        return core::Result<ReplicationBatch>::failure(
            "replication.invalid_binary",
            "binary replication reserved-id count is outside its bounds");
    }
    batch.reserved_ids.reserve(static_cast<std::size_t>(reserved_count));
    for (std::uint64_t index = 0; index < reserved_count; ++index) {
        std::uint64_t id = 0;
        if (!reader.var_u64(id)) {
            return core::Result<ReplicationBatch>::failure(
                "replication.invalid_binary", "binary replication reserved-id list is truncated");
        }
        batch.reserved_ids.push_back(core::SaveId::from_value(id));
    }
    if (!reader.finished()) {
        return core::Result<ReplicationBatch>::failure(
            "replication.trailing_data", "binary replication payload contains trailing bytes");
    }
    return core::Result<ReplicationBatch>::success(std::move(batch));
}

ReplicationRelevanceReport
ReplicationRelevance::evaluate(const ReplicationRelevancePolicy& policy,
                               const ReplicationBatch& batch,
                               const std::vector<core::NetId>& candidate_clients) {
    ReplicationRelevanceReport report;
    report.command_sequence = batch.command_sequence;
    report.replication_sequence = replication_stream_sequence(batch);
    report.source_client_id = batch.source_client_id;
    report.command_type = batch.command_type;
    report.broadcast_by_default = policy.broadcast_by_default;
    report.event_count = static_cast<std::uint32_t>(batch.events.size());
    report.spatial_event_count = static_cast<std::uint32_t>(std::ranges::count_if(
        batch.events, [](const world::OperationEvent& event) {
            return event.routing_chunk.has_value();
        }));
    report.candidate_client_count = static_cast<std::uint32_t>(candidate_clients.size());
    report.decisions.reserve(candidate_clients.size());

    for (const auto client_id : candidate_clients) {
        auto decision = evaluate_client_relevance(policy, batch, client_id);
        if (decision.relevant) {
            ++report.relevant_client_count;
        } else {
            ++report.filtered_client_count;
        }
        report.relevant_spatial_event_delivery_count += decision.relevant_spatial_event_count;
        report.filtered_spatial_event_delivery_count += decision.filtered_spatial_event_count;
        report.filtered_spatial_payload_bytes += decision.filtered_spatial_payload_bytes;
        report.decisions.push_back(std::move(decision));
    }

    return report;
}

bool ReplicationRelevance::subject_is_visible(const ReplicationRelevancePolicy& policy,
                                              core::NetId client_id,
                                              core::SaveId subject) noexcept {
    const auto* rule = find_interest_rule(policy, client_id);
    if (rule == nullptr) {
        return policy.broadcast_by_default;
    }
    return subject_is_visible_for_rule(*rule, subject);
}

bool ReplicationRelevance::private_subject_is_visible(const ReplicationRelevancePolicy& policy,
                                                      core::NetId client_id,
                                                      core::SaveId subject) noexcept {
    if (!subject.is_valid()) {
        return false;
    }
    const auto* rule = find_private_access_rule(policy, client_id);
    return rule != nullptr &&
           std::ranges::find(rule->private_subjects, subject) != rule->private_subjects.end();
}

bool ReplicationRelevance::chunk_is_visible(const ReplicationRelevancePolicy& policy,
                                            core::NetId client_id,
                                            world::ChunkCoord chunk) noexcept {
    const auto* rule = find_chunk_interest_rule(policy, client_id);
    if (rule == nullptr) {
        return policy.broadcast_by_default;
    }
    return std::ranges::binary_search(rule->visible_chunks, chunk);
}

bool ReplicationRelevance::event_is_visible(const ReplicationRelevancePolicy& policy,
                                            core::NetId client_id,
                                            const world::OperationEvent& event) noexcept {
    if (event.subject.is_valid()) {
        if (!subject_is_visible(policy, client_id, event.subject)) {
            return false;
        }
    } else if (!event.routing_chunk.has_value() &&
               !subject_is_visible(policy, client_id, event.subject)) {
        return false;
    }
    if (event.routing_chunk.has_value() &&
        !chunk_is_visible(policy, client_id, *event.routing_chunk)) {
        return false;
    }
    return !replication_event_requires_private_access(event) ||
           private_subject_is_visible(policy, client_id, event.subject);
}

ReplicationBatch ReplicationRelevance::filter_for_client(const ReplicationRelevancePolicy& policy,
                                                         const ReplicationBatch& batch,
                                                         core::NetId client_id) {
    ReplicationBatch filtered;
    filtered.command_sequence = batch.command_sequence;
    filtered.command_type = batch.command_type;
    filtered.replication_sequence = replication_stream_sequence(batch);
    filtered.source_client_id = batch.source_client_id;

    for (const auto& event : batch.events) {
        if (event_is_visible(policy, client_id, event)) {
            filtered.events.push_back(event);
        }
    }

    for (const auto reserved_id : batch.reserved_ids) {
        const auto is_visible_subject =
            reserved_id.is_valid() &&
            std::ranges::any_of(filtered.events, [reserved_id](const auto& event) {
                return event.subject == reserved_id;
            });
        if (is_visible_subject) {
            filtered.reserved_ids.push_back(reserved_id);
        }
    }
    return filtered;
}

bool replication_event_requires_private_access(const world::OperationEvent& event) noexcept {
    return event.type.starts_with("inventory.");
}

ReplicationIntakeReport ReplicationIntake::summarize(std::span<const ReplicationBatch> batches) {
    ReplicationIntakeReport report;
    report.batch_count = static_cast<std::uint32_t>(batches.size());
    report.batches.reserve(batches.size());

    bool has_previous_sequence = false;
    std::uint64_t previous_sequence = 0;
    for (const auto& batch : batches) {
        ReplicationIntakeBatchReport batch_report;
        batch_report.command_sequence = batch.command_sequence;
        batch_report.command_type = batch.command_type;
        batch_report.event_count = static_cast<std::uint32_t>(batch.events.size());
        batch_report.reserved_id_count = static_cast<std::uint32_t>(batch.reserved_ids.size());

        const auto stream_sequence = replication_stream_sequence(batch);
        if (!has_previous_sequence) {
            report.first_sequence = stream_sequence;
        } else if (stream_sequence <= previous_sequence) {
            report.strictly_increasing_sequences = false;
        }
        previous_sequence = stream_sequence;
        has_previous_sequence = true;
        report.last_sequence = stream_sequence;

        report.event_count += batch_report.event_count;
        report.reserved_id_count += batch_report.reserved_id_count;
        for (const auto& event : batch.events) {
            if (event.subject.is_valid()) {
                batch_report.has_subject_events = true;
                report.has_subject_events = true;
            } else {
                batch_report.has_global_events = true;
                report.has_global_events = true;
            }
        }

        report.batches.push_back(std::move(batch_report));
    }

    return report;
}

TransportMessage make_replication_transport_message(const ReplicationBatch& batch,
                                                    std::int64_t server_time_ms) {
    return TransportMessage{
        TransportMessageKind::replication,     TransportChannel::reliable,
        replication_stream_sequence(batch),    std::string(replication_world_events_payload_type),
        ReplicationBinaryCodec::encode(batch), server_time_ms,
    };
}

core::Result<ReplicationBatch> replication_batch_from_transport(const TransportEnvelope& envelope) {
    if (envelope.message.kind != TransportMessageKind::replication) {
        return core::Result<ReplicationBatch>::failure(
            "replication.not_replication_message", "transport message is not a replication batch");
    }
    if (envelope.message.channel != TransportChannel::reliable) {
        return core::Result<ReplicationBatch>::failure(
            "replication.unreliable_replication",
            "replication batches must arrive on the reliable transport channel");
    }
    if (envelope.message.payload_type != replication_world_events_payload_type &&
        envelope.message.payload_type != replication_world_events_legacy_payload_type) {
        return core::Result<ReplicationBatch>::failure(
            "replication.unexpected_payload_type",
            "replication transport message has an unexpected payload type");
    }

    auto batch = envelope.message.payload_type == replication_world_events_payload_type
                     ? ReplicationBinaryCodec::decode(envelope.message.payload)
                     : ReplicationTextCodec::decode(envelope.message.payload);
    if (!batch) {
        return batch;
    }
    if (replication_stream_sequence(batch.value()) != envelope.message.sequence) {
        return core::Result<ReplicationBatch>::failure(
            "replication.sequence_mismatch",
            "replication batch sequence does not match the transport envelope sequence");
    }
    return batch;
}

} // namespace heartstead::net
