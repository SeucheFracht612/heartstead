#include "engine/net/host_session.hpp"

#include "engine/net/binary_message.hpp"
#include "engine/net/command_payload.hpp"
#include "engine/net/transport_control.hpp"
#include "engine/net/transport_packet.hpp"

#include <charconv>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace heartstead::net {

namespace {

constexpr std::uint32_t command_result_binary_magic =
    0x31524348U; // "HCR1" in little-endian byte order.
constexpr std::size_t max_command_result_type_bytes = 128;
constexpr std::size_t max_command_result_error_code_bytes = 128;
constexpr std::size_t max_command_result_error_message_bytes = 32U * 1024U;
constexpr std::size_t max_command_result_bytes = 64U * 1024U;

[[nodiscard]] HostSessionCommandReport
make_transport_error_report(const TransportEnvelope& envelope, std::string code,
                            std::string message) {
    HostSessionCommandReport report;
    report.client_id = envelope.sender;
    report.sequence = envelope.message.sequence;
    report.command_type = envelope.message.payload_type;
    report.error_code = std::move(code);
    report.error_message = std::move(message);
    return report;
}

[[nodiscard]] HostSessionCommandReport
make_host_command_report(core::NetId client_id, const CommandDispatchReport& dispatch_report) {
    HostSessionCommandReport report;
    report.client_id = client_id;
    report.sequence = dispatch_report.sequence;
    report.command_type = dispatch_report.command_type;
    report.success = dispatch_report.succeeded;
    report.committed_world_mutation = dispatch_report.committed_world_mutation;
    report.events = dispatch_report.events;
    report.reserved_ids = dispatch_report.reserved_ids;
    report.operation_trace = dispatch_report.operation_trace;
    if (dispatch_report.error.has_value()) {
        report.error_code = dispatch_report.error->code;
        report.error_message = dispatch_report.error->message;
    }
    return report;
}

[[nodiscard]] core::Result<std::uint64_t> parse_u64(std::string_view value,
                                                    std::string_view field_name) {
    std::uint64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return core::Result<std::uint64_t>::failure("host_session_result.invalid_number",
                                                    "invalid command result number field: " +
                                                        std::string(field_name));
    }
    return core::Result<std::uint64_t>::success(parsed);
}

[[nodiscard]] core::Result<std::uint32_t> parse_u32(std::string_view value,
                                                    std::string_view field_name) {
    auto parsed = parse_u64(value, field_name);
    if (!parsed) {
        return core::Result<std::uint32_t>::failure(parsed.error().code, parsed.error().message);
    }
    if (parsed.value() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<std::uint32_t>::failure(
            "host_session_result.number_out_of_range",
            "command result number field exceeds uint32 range: " + std::string(field_name));
    }
    return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(parsed.value()));
}

[[nodiscard]] core::Result<bool> parse_bool(std::string_view value, std::string_view field_name) {
    if (value == "1") {
        return core::Result<bool>::success(true);
    }
    if (value == "0") {
        return core::Result<bool>::success(false);
    }
    return core::Result<bool>::failure("host_session_result.invalid_bool",
                                       "invalid command result bool field: " +
                                           std::string(field_name));
}

template <typename T> [[nodiscard]] core::Result<T> fail_from(const core::Error& error) {
    return core::Result<T>::failure(error.code, error.message);
}

} // namespace

HostSession::HostSession(HostSessionConfig config)
    : HostSession(std::move(config),
                  [](TransportHostDesc desc) { return create_transport_host(std::move(desc)); }) {}

HostSession::HostSession(HostSessionConfig config, HostSessionTransportFactory transport_factory)
    : config_(std::move(config)), transport_factory_(std::move(transport_factory)) {}

HostSessionState HostSession::state() const noexcept {
    return state_;
}

bool HostSession::is_running() const noexcept {
    return state_ == HostSessionState::running;
}

core::NetId HostSession::server_id() const noexcept {
    if (transport_) {
        return transport_->server_id();
    }
    return transport_host_server_id(config_.transport);
}

std::optional<TransportEndpoint> HostSession::local_endpoint() const {
    return transport_ ? transport_->local_endpoint() : std::nullopt;
}

std::size_t HostSession::connected_client_count() const noexcept {
    return transport_ ? transport_->connected_client_count() : 0;
}

std::size_t HostSession::pending_outbound_message_count() const noexcept {
    std::size_t count = 0;
    for (const auto& [_, messages] : pending_outbound_) {
        count += messages.size();
    }
    return count;
}

const ReplicationRelevancePolicy& HostSession::replication_relevance_policy() const noexcept {
    return config_.replication_relevance;
}

core::Status HostSession::start() {
    if (is_running()) {
        return core::Status::failure("host_session.already_running",
                                     "host session is already running");
    }

    if (!transport_factory_) {
        return core::Status::failure("host_session.missing_transport_factory",
                                     "host session transport factory is not configured");
    }
    if (config_.max_outbound_bytes_per_client_per_second == 0) {
        return core::Status::failure("host_session.invalid_outbound_budget",
                                     "per-client outbound byte budget must be non-zero");
    }
    auto transport = transport_factory_(config_.transport);
    if (!transport) {
        return core::Status::failure(transport.error().code, transport.error().message);
    }
    if (!transport.value()) {
        return core::Status::failure("host_session.null_transport",
                                     "host session transport factory returned a null transport");
    }

    transport_ = std::move(transport).value();
    pending_outbound_.clear();
    outbound_budget_windows_.clear();
    next_replication_sequence_ = 1;
    current_server_time_ms_ = 0;
    pending_budget_dropped_unreliable_message_count_ = 0;
    state_ = HostSessionState::running;
    return core::Status::ok();
}

core::Status HostSession::stop() {
    if (!is_running()) {
        return core::Status::failure("host_session.not_running", "host session is not running");
    }

    state_ = HostSessionState::stopped;
    transport_.reset();
    pending_outbound_.clear();
    outbound_budget_windows_.clear();
    return core::Status::ok();
}

void HostSession::set_replication_relevance_policy(ReplicationRelevancePolicy policy) {
    config_.replication_relevance = std::move(policy);
}

core::Result<core::NetId> HostSession::connect_client() {
    auto running = require_running();
    if (!running) {
        return core::Result<core::NetId>::failure(running.error().code, running.error().message);
    }
    auto client = transport_->connect_client();
    if (!client) {
        return client;
    }

    auto status = send_welcome(client.value(), 0);
    if (!status) {
        (void)transport_->disconnect_client(client.value());
        return core::Result<core::NetId>::failure(status.error().code, status.error().message);
    }

    return client;
}

core::Status HostSession::disconnect_client(core::NetId client_id) {
    auto running = require_running();
    if (!running) {
        return running;
    }
    const TransportServerDisconnect disconnect{
        transport_control_protocol_version, transport_->server_id(),      client_id,
        "host_session.disconnect",          "server disconnected client",
    };
    auto notice = transport_->send_server_to_client(
        client_id, make_server_disconnect_transport_message(disconnect, 0));
    auto disconnected = transport_->disconnect_client(client_id);
    if (disconnected) {
        pending_outbound_.erase(client_id);
        // A disconnect notice is best effort. Once the transport has removed the peer, report the
        // requested state transition as successful even if its final notification could not be
        // delivered.
        return core::Status::ok();
    }
    if (!notice) {
        return notice;
    }
    return disconnected;
}

core::Status HostSession::send_client_command(core::NetId client_id, CommandEnvelope envelope) {
    auto running = require_running();
    if (!running) {
        return running;
    }
    if (!envelope.sender.is_valid()) {
        envelope.sender = client_id;
    }
    if (envelope.sender != client_id) {
        return core::Status::failure("host_session.sender_mismatch",
                                     "command sender must match client connection");
    }
    return transport_->send_client_to_server(client_id, make_command_transport_message(envelope));
}

core::Status HostSession::send_client_control(core::NetId client_id, TransportMessage message) {
    auto running = require_running();
    if (!running) {
        return running;
    }
    if (message.kind != TransportMessageKind::control ||
        message.channel != TransportChannel::unreliable) {
        return core::Status::failure(
            "host_session.not_unreliable_control",
            "client control path accepts only unreliable control messages");
    }
    return transport_->send_client_to_server(client_id, std::move(message));
}

core::Status HostSession::send_replication_message(core::NetId client_id,
                                                   TransportMessage message) {
    auto running = require_running();
    if (!running) {
        return running;
    }
    if (message.kind != TransportMessageKind::replication) {
        return core::Status::failure("host_session.not_replication_message",
                                     "host session can only send replication messages through "
                                     "send_replication_message");
    }
    auto status = validate_transport_message(message, transport_->capabilities().max_payload_bytes);
    if (!status) {
        return status;
    }
    if (!transport_->is_client_connected(client_id)) {
        return core::Status::failure("transport.unknown_client",
                                     "replication recipient is not connected");
    }

    auto pending = pending_outbound_.find(client_id);
    if (pending != pending_outbound_.end() && !pending->second.empty()) {
        if (message.channel == TransportChannel::reliable) {
            pending->second.push_back(PendingOutboundMessage{std::move(message), 0});
        } else {
            ++pending_budget_dropped_unreliable_message_count_;
        }
        return core::Status::ok();
    }
    if (!admit_outbound(client_id, message)) {
        if (message.channel == TransportChannel::reliable) {
            pending_outbound_[client_id].push_back(PendingOutboundMessage{std::move(message), 0});
        } else {
            ++pending_budget_dropped_unreliable_message_count_;
        }
        return core::Status::ok();
    }

    status = transport_->send_server_to_client(client_id, message);
    if (status || message.channel != TransportChannel::reliable) {
        return status;
    }

    // Reliable application replication describes authoritative state that may already have been
    // committed. Retain it behind the same per-client FIFO as command results so a transient
    // transport failure cannot turn a successful world mutation into an apparent API failure.
    pending_outbound_[client_id].push_back(PendingOutboundMessage{std::move(message), 1});
    return core::Status::ok();
}

core::Result<std::vector<TransportEnvelope>>
HostSession::drain_client_messages(core::NetId client_id) {
    auto running = require_running();
    if (!running) {
        return core::Result<std::vector<TransportEnvelope>>::failure(running.error().code,
                                                                     running.error().message);
    }
    return transport_->drain_client_messages(client_id);
}

core::Result<HostSessionTickResult> HostSession::tick(const ServerCommandDispatcher& dispatcher,
                                                      CommandExecutionContext context) {
    auto running = require_running();
    if (!running) {
        return core::Result<HostSessionTickResult>::failure(running.error().code,
                                                            running.error().message);
    }

    context.executor_role = CommandExecutorRole::authoritative_server;
    current_server_time_ms_ = context.server_time_ms;

    HostSessionTickResult tick_result;
    tick_result.outbound_budget_dropped_unreliable_message_count =
        pending_budget_dropped_unreliable_message_count_;
    pending_budget_dropped_unreliable_message_count_ = 0;
    auto maintenance = transport_->poll_maintenance(context.server_time_ms);
    if (!maintenance) {
        return core::Result<HostSessionTickResult>::failure(maintenance.error().code,
                                                            maintenance.error().message);
    }
    tick_result.transport_retransmission_count = maintenance.value().retransmission_count;
    tick_result.transport_dropped_reliable_message_count =
        maintenance.value().dropped_reliable_message_count;
    tick_result.transport_malformed_datagram_count = maintenance.value().malformed_datagram_count;
    tick_result.transport_rejected_datagram_count = maintenance.value().rejected_datagram_count;
    tick_result.transport_rate_limited_datagram_count =
        maintenance.value().rate_limited_datagram_count;
    tick_result.transport_client_to_server_bytes = maintenance.value().client_to_server_bytes;
    tick_result.transport_server_to_client_bytes = maintenance.value().server_to_client_bytes;
    tick_result.transport_client_to_server_message_count =
        maintenance.value().client_to_server_message_count;
    tick_result.transport_server_to_client_message_count =
        maintenance.value().server_to_client_message_count;
    tick_result.transport_simulated_dropped_unreliable_message_count =
        maintenance.value().simulated_dropped_unreliable_message_count;
    tick_result.transport_pending_impaired_message_count =
        maintenance.value().pending_impaired_message_count;
    for (const auto client_id : maintenance.value().connected_clients) {
        auto status = send_welcome(client_id, context.server_time_ms);
        if (!status) {
            (void)transport_->disconnect_client(client_id);
            return core::Result<HostSessionTickResult>::failure(status.error().code,
                                                                status.error().message);
        }
        tick_result.connected_clients.push_back(client_id);
    }
    tick_result.disconnected_clients = maintenance.value().disconnected_clients;
    for (const auto client_id : tick_result.disconnected_clients) {
        pending_outbound_.erase(client_id);
        outbound_budget_windows_.erase(client_id);
    }

    // A committed command must never be dispatched again, and a later command must not make an
    // already-behind recipient fall farther out of sync. Give deferred reliable output the first
    // opportunity to drain. If any recipient is still blocked, leave inbound commands in the
    // transport until the next tick instead of growing the host queue without bound.
    flush_pending_outbound(tick_result.outbound_delivery);
    if (tick_result.outbound_delivery.pending_message_count > 0) {
        return core::Result<HostSessionTickResult>::success(std::move(tick_result));
    }

    auto messages = transport_->drain_server_messages();
    tick_result.transport_message_count = static_cast<std::uint32_t>(messages.size());

    for (const auto& message : messages) {
        if (message.message.kind == TransportMessageKind::control &&
            message.message.channel == TransportChannel::unreliable) {
            ++tick_result.control_message_count;
            tick_result.control_messages.push_back(message);
            continue;
        }
        HostSessionCommandReport report;
        if (message.message.kind != TransportMessageKind::command) {
            report = make_transport_error_report(message, "transport.not_command_message",
                                                 "host session expected a command message");
        } else if (next_replication_sequence_ == 0) {
            ++tick_result.command_message_count;
            report = make_transport_error_report(
                message, "host_session.replication_sequence_exhausted",
                "server replication stream exhausted its 64-bit sequence space");
        } else {
            ++tick_result.command_message_count;
            auto command = command_envelope_from_transport(message);
            if (!command) {
                report = make_transport_error_report(message, command.error().code,
                                                     command.error().message);
            } else {
                report = make_host_command_report(
                    message.sender, dispatcher.dispatch_report(command.value(), context));
            }
        }

        auto sequence = assign_replication_sequence(report);
        if (!sequence) {
            return core::Result<HostSessionTickResult>::failure(sequence.error().code,
                                                                sequence.error().message);
        }

        queue_command_response(report);
        ++tick_result.response_message_count;

        std::uint32_t queued_replication_count = 0;
        auto relevance_report =
            queue_replication(report, context.server_time_ms, queued_replication_count);
        tick_result.replication_message_count += queued_replication_count;
        if (relevance_report.event_count > 0) {
            tick_result.replication_relevance_reports.push_back(std::move(relevance_report));
        }
        tick_result.command_reports.push_back(std::move(report));
    }

    flush_pending_outbound(tick_result.outbound_delivery);

    return core::Result<HostSessionTickResult>::success(std::move(tick_result));
}

core::Status HostSession::require_running() const {
    if (!is_running()) {
        return core::Status::failure("host_session.not_running", "host session is not running");
    }
    return core::Status::ok();
}

core::Status HostSession::send_welcome(core::NetId client_id, std::int64_t server_time_ms) {
    const auto capabilities = transport_->capabilities();
    const TransportServerWelcome welcome{
        transport_control_protocol_version,
        transport_->server_id(),
        client_id,
        capabilities.max_payload_bytes,
        capabilities.max_clients,
        capabilities.supports_unreliable,
        capabilities.enforces_reliable_command_order,
    };
    return transport_->send_server_to_client(
        client_id, make_server_welcome_transport_message(welcome, server_time_ms));
}

core::Status HostSession::assign_replication_sequence(HostSessionCommandReport& report) {
    if (!report.success || !report.committed_world_mutation || report.events.empty()) {
        return core::Status::ok();
    }
    if (next_replication_sequence_ == 0) {
        return core::Status::failure(
            "host_session.replication_sequence_exhausted",
            "server replication stream exhausted its 64-bit sequence space");
    }

    report.replication_sequence = next_replication_sequence_;
    next_replication_sequence_ =
        next_replication_sequence_ == std::numeric_limits<std::uint64_t>::max()
            ? 0
            : next_replication_sequence_ + 1;
    return core::Status::ok();
}

void HostSession::queue_command_response(const HostSessionCommandReport& report) {
    const auto response_type = report.command_type.empty() ? std::string("command.result")
                                                           : report.command_type + ".result";
    pending_outbound_[report.client_id].push_back(PendingOutboundMessage{
        TransportMessage{TransportMessageKind::command_result, TransportChannel::reliable,
                         report.sequence, response_type, host_session_result_payload(report), 0},
        0});
}

ReplicationRelevanceReport HostSession::queue_replication(const HostSessionCommandReport& report,
                                                          std::int64_t server_time_ms,
                                                          std::uint32_t& queued_message_count) {
    ReplicationRelevanceReport relevance_report;
    if (!report.success || !report.committed_world_mutation || report.events.empty()) {
        return relevance_report;
    }

    ReplicationBatch batch{
        report.sequence,     report.command_type,         report.events,
        report.reserved_ids, report.replication_sequence, report.client_id,
    };
    relevance_report = ReplicationRelevance::evaluate(config_.replication_relevance, batch,
                                                      transport_->connected_client_ids());
    for (const auto& decision : relevance_report.decisions) {
        if (!decision.relevant) {
            continue;
        }
        auto filtered = ReplicationRelevance::filter_for_client(config_.replication_relevance,
                                                                batch, decision.client_id);
        if (filtered.events.empty()) {
            continue;
        }
        pending_outbound_[decision.client_id].push_back(PendingOutboundMessage{
            make_replication_transport_message(filtered, server_time_ms), 0});
        ++queued_message_count;
    }
    return relevance_report;
}

void HostSession::flush_pending_outbound(HostSessionOutboundDeliveryReport& report) {
    for (auto pending = pending_outbound_.begin(); pending != pending_outbound_.end();) {
        auto& messages = pending->second;
        bool blocked = false;
        while (!messages.empty()) {
            auto& outbound = messages.front();
            if (!admit_outbound(pending->first, outbound.message)) {
                ++report.budget_deferred_message_count;
                blocked = true;
                break;
            }
            ++report.attempted_message_count;
            if (outbound.attempt_count > 0) {
                ++report.retry_attempt_count;
            }
            ++outbound.attempt_count;

            auto delivered = transport_->send_server_to_client(pending->first, outbound.message);
            if (!delivered) {
                ++report.failed_attempt_count;
                report.failures.push_back(HostSessionOutboundDeliveryFailure{
                    pending->first,
                    outbound.message.kind,
                    outbound.message.sequence,
                    outbound.attempt_count,
                    delivered.error().code,
                    delivered.error().message,
                });
                blocked = true;
                break;
            }

            ++report.delivered_message_count;
            messages.pop_front();
        }

        if (blocked) {
            ++report.blocked_client_count;
        }
        if (messages.empty()) {
            pending = pending_outbound_.erase(pending);
        } else {
            ++pending;
        }
    }
    report.pending_message_count = pending_outbound_message_count();
}

bool HostSession::admit_outbound(core::NetId client_id, const TransportMessage& message) {
    auto& window = outbound_budget_windows_[client_id];
    if (!window.initialized || current_server_time_ms_ < window.started_ms ||
        current_server_time_ms_ - window.started_ms >= 1'000) {
        window.started_ms = current_server_time_ms_;
        window.used_bytes = 0;
        window.initialized = true;
    }
    const auto bytes = static_cast<std::uint64_t>(outbound_wire_bytes(client_id, message));
    const auto limit = static_cast<std::uint64_t>(config_.max_outbound_bytes_per_client_per_second);
    if (bytes > limit || window.used_bytes > limit - bytes) {
        return false;
    }
    window.used_bytes += bytes;
    return true;
}

std::size_t HostSession::outbound_wire_bytes(core::NetId client_id,
                                             const TransportMessage& message) const {
    return TransportPacketCodec::encode({server_id(), client_id, message}).size();
}

std::string_view host_session_state_name(HostSessionState state) noexcept {
    switch (state) {
    case HostSessionState::stopped:
        return "stopped";
    case HostSessionState::running:
        return "running";
    }
    return "unknown";
}

std::string HostSessionCommandResultTextCodec::encode(const HostSessionCommandResult& result) {
    CommandPayload payload;
    (void)payload.set("status", result.success ? "ok" : "error");
    (void)payload.set("sequence", std::to_string(result.sequence));
    (void)payload.set("command_type", result.command_type);
    (void)payload.set("committed", result.committed_world_mutation ? "1" : "0");
    (void)payload.set("events", std::to_string(result.event_count));
    (void)payload.set("reserved_ids", std::to_string(result.reserved_id_count));
    if (!result.success) {
        (void)payload.set("code", result.error_code);
        (void)payload.set("message", result.error_message);
    }
    return CommandPayloadTextCodec::encode(payload);
}

core::Result<HostSessionCommandResult>
HostSessionCommandResultTextCodec::decode(std::string_view text) {
    auto payload = CommandPayloadTextCodec::decode(text);
    if (!payload) {
        return core::Result<HostSessionCommandResult>::failure(payload.error().code,
                                                               payload.error().message);
    }

    auto status = payload.value().require("status");
    auto sequence_value = payload.value().require("sequence");
    auto committed_value = payload.value().require("committed");
    auto event_count_value = payload.value().require("events");
    auto reserved_id_count_value = payload.value().require("reserved_ids");
    if (!status || !sequence_value || !committed_value || !event_count_value ||
        !reserved_id_count_value) {
        return core::Result<HostSessionCommandResult>::failure(
            "host_session_result.missing_key", "command result payload is missing required fields");
    }

    HostSessionCommandResult result;
    if (status.value() == "ok") {
        result.success = true;
    } else if (status.value() == "error") {
        result.success = false;
    } else {
        return core::Result<HostSessionCommandResult>::failure(
            "host_session_result.invalid_status", "command result status must be ok or error");
    }

    auto sequence = parse_u64(sequence_value.value(), "sequence");
    auto committed = parse_bool(committed_value.value(), "committed");
    auto event_count = parse_u32(event_count_value.value(), "events");
    auto reserved_id_count = parse_u32(reserved_id_count_value.value(), "reserved_ids");
    if (!sequence) {
        return fail_from<HostSessionCommandResult>(sequence.error());
    }
    if (!committed) {
        return fail_from<HostSessionCommandResult>(committed.error());
    }
    if (!event_count) {
        return fail_from<HostSessionCommandResult>(event_count.error());
    }
    if (!reserved_id_count) {
        return fail_from<HostSessionCommandResult>(reserved_id_count.error());
    }

    result.sequence = sequence.value();
    const auto* command_type = payload.value().find("command_type");
    if (command_type != nullptr) {
        result.command_type = *command_type;
    }
    result.committed_world_mutation = committed.value();
    result.event_count = event_count.value();
    result.reserved_id_count = reserved_id_count.value();

    if (!result.success) {
        auto error_code = payload.value().require("code");
        auto error_message = payload.value().require("message");
        if (!error_code || !error_message) {
            return core::Result<HostSessionCommandResult>::failure(
                "host_session_result.missing_error",
                "failed command result payload must include error code and message");
        }
        result.error_code = std::string(error_code.value());
        result.error_message = std::string(error_message.value());
    }

    auto validation = validate_host_session_command_result(result);
    if (!validation) {
        return core::Result<HostSessionCommandResult>::failure(validation.error().code,
                                                               validation.error().message);
    }
    return core::Result<HostSessionCommandResult>::success(std::move(result));
}

std::string HostSessionCommandResultBinaryCodec::encode(const HostSessionCommandResult& result) {
    BinaryMessageWriter writer;
    writer.u32(command_result_binary_magic);
    writer.var_u64(result.sequence);
    std::uint8_t flags = result.success ? 1U : 0U;
    flags |= result.committed_world_mutation ? 2U : 0U;
    writer.u8(flags);
    writer.var_u64(result.event_count);
    writer.var_u64(result.reserved_id_count);
    writer.text_var(result.command_type);
    if (!result.success) {
        writer.text_var(result.error_code);
        writer.text_var(result.error_message);
    }
    return writer.take();
}

core::Result<HostSessionCommandResult>
HostSessionCommandResultBinaryCodec::decode(std::string_view bytes) {
    if (bytes.size() > max_command_result_bytes) {
        return core::Result<HostSessionCommandResult>::failure(
            "host_session_result.too_large", "binary command result exceeds its payload limit");
    }
    BinaryMessageReader reader(bytes);
    std::uint32_t decoded_magic = 0;
    std::uint64_t event_count = 0;
    std::uint64_t reserved_id_count = 0;
    std::uint8_t flags = 0;
    HostSessionCommandResult result;
    if (!reader.u32(decoded_magic) || decoded_magic != command_result_binary_magic ||
        !reader.var_u64(result.sequence) || !reader.u8(flags) || flags > 3U ||
        !reader.var_u64(event_count) || !reader.var_u64(reserved_id_count) ||
        event_count > std::numeric_limits<std::uint32_t>::max() ||
        reserved_id_count > std::numeric_limits<std::uint32_t>::max() ||
        !reader.text_var(result.command_type, max_command_result_type_bytes)) {
        return core::Result<HostSessionCommandResult>::failure(
            "host_session_result.invalid_binary",
            "binary command result is malformed, truncated, or outside its bounds");
    }
    result.success = (flags & 1U) != 0;
    result.committed_world_mutation = (flags & 2U) != 0;
    result.event_count = static_cast<std::uint32_t>(event_count);
    result.reserved_id_count = static_cast<std::uint32_t>(reserved_id_count);
    if (!result.success &&
        (!reader.text_var(result.error_code, max_command_result_error_code_bytes) ||
         !reader.text_var(result.error_message, max_command_result_error_message_bytes))) {
        return core::Result<HostSessionCommandResult>::failure(
            "host_session_result.invalid_binary",
            "failed binary command result is missing bounded error fields");
    }
    if (!reader.finished()) {
        return core::Result<HostSessionCommandResult>::failure(
            "host_session_result.trailing_data", "binary command result contains trailing bytes");
    }
    auto validation = validate_host_session_command_result(result);
    if (!validation) {
        return core::Result<HostSessionCommandResult>::failure(validation.error().code,
                                                               validation.error().message);
    }
    return core::Result<HostSessionCommandResult>::success(std::move(result));
}

bool HostSessionCommandResultBinaryCodec::is_encoded(std::string_view bytes) noexcept {
    BinaryMessageReader reader(bytes);
    std::uint32_t decoded_magic = 0;
    return reader.u32(decoded_magic) && decoded_magic == command_result_binary_magic;
}

core::Status validate_host_session_command_result(const HostSessionCommandResult& result) noexcept {
    if (!result.command_type.empty() && !is_valid_command_payload_key(result.command_type)) {
        return core::Status::failure(
            "host_session_result.invalid_command_type",
            "command result command type must use a stable command payload key shape");
    }
    if (result.success) {
        if (!result.error_code.empty() || !result.error_message.empty()) {
            return core::Status::failure("host_session_result.unexpected_error",
                                         "successful command result must not carry error fields");
        }
        return core::Status::ok();
    }
    if (result.error_code.empty() || !is_valid_command_payload_key(result.error_code)) {
        return core::Status::failure("host_session_result.invalid_error_code",
                                     "failed command result must carry a stable error code");
    }
    if (result.error_message.empty()) {
        return core::Status::failure("host_session_result.missing_error_message",
                                     "failed command result must carry an error message");
    }
    return core::Status::ok();
}

HostSessionCommandResult
host_session_command_result_from_report(const HostSessionCommandReport& report) {
    HostSessionCommandResult result;
    result.sequence = report.sequence;
    result.command_type = report.command_type;
    result.success = report.success;
    result.committed_world_mutation = report.committed_world_mutation;
    result.event_count = static_cast<std::uint32_t>(report.events.size());
    result.reserved_id_count = static_cast<std::uint32_t>(report.reserved_ids.size());
    result.error_code = report.error_code;
    result.error_message = report.error_message;
    return result;
}

std::string host_session_result_payload(const HostSessionCommandReport& report) {
    return HostSessionCommandResultBinaryCodec::encode(
        host_session_command_result_from_report(report));
}

core::Result<HostSessionCommandResult>
host_session_command_result_from_transport(const TransportEnvelope& envelope) {
    if (envelope.message.kind != TransportMessageKind::command_result) {
        return core::Result<HostSessionCommandResult>::failure(
            "host_session_result.not_command_result", "transport message is not a command result");
    }
    if (envelope.message.channel != TransportChannel::reliable) {
        return core::Result<HostSessionCommandResult>::failure(
            "host_session_result.unreliable_command_result",
            "command result must arrive on the reliable transport channel");
    }

    auto result = HostSessionCommandResultBinaryCodec::is_encoded(envelope.message.payload)
                      ? HostSessionCommandResultBinaryCodec::decode(envelope.message.payload)
                      : HostSessionCommandResultTextCodec::decode(envelope.message.payload);
    if (!result) {
        return result;
    }
    if (result.value().sequence != envelope.message.sequence) {
        return core::Result<HostSessionCommandResult>::failure(
            "host_session_result.sequence_mismatch",
            "command result sequence does not match the transport envelope sequence");
    }

    const auto expected_payload_type = result.value().command_type.empty()
                                           ? std::string("command.result")
                                           : result.value().command_type + ".result";
    if (envelope.message.payload_type != expected_payload_type) {
        return core::Result<HostSessionCommandResult>::failure(
            "host_session_result.payload_type_mismatch",
            "command result payload type does not match the decoded command type");
    }
    return result;
}

} // namespace heartstead::net
