#include "engine/debug/inspection.hpp"
#include "engine/net/host_session.hpp"
#include "engine/net/transport.hpp"
#include "engine/net/transport_client.hpp"
#include "engine/net/transport_handshake.hpp"
#include "engine/net/transport_packet.hpp"
#include "engine/net/transport_reliability.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace heartstead;

[[nodiscard]] net::TransportMessage reliable_message(net::TransportMessageKind kind,
                                                     std::uint64_t sequence, std::string payload) {
    return {kind, net::TransportChannel::reliable, sequence, "test.reliable", std::move(payload),
            0};
}

class FaultInjectingTransportHost final : public net::ITransportHost {
  public:
    explicit FaultInjectingTransportHost(net::InMemoryTransportHostConfig config)
        : delegate_(config) {}

    void fail_server_send(core::NetId client_id, net::TransportMessageKind kind,
                          std::uint64_t sequence, std::uint32_t count = 1) {
        failures_.push_back({client_id, kind, sequence, count});
    }

    [[nodiscard]] net::TransportBackend backend() const noexcept override {
        return delegate_.backend();
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return delegate_.backend_name();
    }

    [[nodiscard]] net::TransportCapabilities capabilities() const noexcept override {
        return delegate_.capabilities();
    }

    [[nodiscard]] core::NetId server_id() const noexcept override {
        return delegate_.server_id();
    }

    [[nodiscard]] std::size_t connected_client_count() const noexcept override {
        return delegate_.connected_client_count();
    }

    [[nodiscard]] bool is_client_connected(core::NetId client_id) const noexcept override {
        return delegate_.is_client_connected(client_id);
    }

    [[nodiscard]] std::vector<core::NetId> connected_client_ids() const override {
        return delegate_.connected_client_ids();
    }

    [[nodiscard]] core::Result<core::NetId> connect_client() override {
        return delegate_.connect_client();
    }

    [[nodiscard]] core::Status disconnect_client(core::NetId client_id) override {
        return delegate_.disconnect_client(client_id);
    }

    [[nodiscard]] core::Status send_client_to_server(core::NetId client_id,
                                                     net::TransportMessage message) override {
        return delegate_.send_client_to_server(client_id, std::move(message));
    }

    [[nodiscard]] core::Status send_server_to_client(core::NetId client_id,
                                                     net::TransportMessage message) override {
        for (auto& failure : failures_) {
            if (failure.remaining_count > 0 && failure.client_id == client_id &&
                failure.kind == message.kind && failure.sequence == message.sequence) {
                --failure.remaining_count;
                return core::Status::failure("test.injected_send_failure",
                                             "injected server send failure");
            }
        }
        return delegate_.send_server_to_client(client_id, std::move(message));
    }

    [[nodiscard]] core::Result<net::TransportMaintenanceResult>
    poll_maintenance(std::int64_t now_ms) override {
        return delegate_.poll_maintenance(now_ms);
    }

    [[nodiscard]] std::vector<net::TransportEnvelope> drain_server_messages() override {
        return delegate_.drain_server_messages();
    }

    [[nodiscard]] core::Result<std::vector<net::TransportEnvelope>>
    drain_client_messages(core::NetId client_id) override {
        return delegate_.drain_client_messages(client_id);
    }

  private:
    struct SendFailure {
        core::NetId client_id;
        net::TransportMessageKind kind = net::TransportMessageKind::command;
        std::uint64_t sequence = 0;
        std::uint32_t remaining_count = 0;
    };

    net::InMemoryTransportHost delegate_;
    std::vector<SendFailure> failures_;
};

[[nodiscard]] net::ServerCommandDispatcher make_mutating_dispatcher(std::uint32_t& dispatch_count) {
    net::ServerCommandDispatcher dispatcher;
    auto registered = dispatcher.register_command(net::CommandDescriptor{
        "test.mutate",
        true,
        true,
        [&dispatch_count](const net::CommandEnvelope& envelope, const net::CommandExecutionContext&,
                          world::WorldOperation& operation) {
            ++dispatch_count;
            auto mutation = operation.record_mutation("test mutation " + envelope.payload);
            if (!mutation) {
                return mutation;
            }
            operation.emit_event({"test.changed", core::SaveId::from_value(1), envelope.payload});
            operation.mark_replication_dirty();
            operation.mark_save_dirty();
            return core::Status::ok();
        },
    });
    assert(registered);
    return dispatcher;
}

[[nodiscard]] net::HostSession make_fault_injected_session(FaultInjectingTransportHost*& host) {
    net::HostSessionConfig config;
    config.transport.backend = net::TransportBackend::in_memory;
    config.transport.in_memory =
        net::InMemoryTransportHostConfig{core::NetId::from_value(1), 4096, 4};
    return net::HostSession(
        config,
        [&host](net::TransportHostDesc desc) -> core::Result<std::unique_ptr<net::ITransportHost>> {
            auto transport = std::make_unique<FaultInjectingTransportHost>(desc.in_memory);
            host = transport.get();
            return core::Result<std::unique_ptr<net::ITransportHost>>::success(
                std::move(transport));
        });
}

void test_tracking_can_be_rolled_back_before_retry() {
    const auto client_id = core::NetId::from_value(2);
    const auto server_id = core::NetId::from_value(1);
    net::TransportReliabilityTracker tracker(client_id, server_id);
    const net::TransportEnvelope envelope{
        client_id, server_id, reliable_message(net::TransportMessageKind::command, 1, "first")};

    assert(tracker.track_send(envelope, 10));
    assert(tracker.pending_count() == 1);
    assert(tracker.rollback_tracked_send(envelope));
    assert(tracker.pending_count() == 0);
    assert(!tracker.rollback_tracked_send(envelope));
    assert(tracker.track_send(envelope, 20));
}

void test_external_capacity_failure_does_not_leak_untracked_datagram() {
    if (!net::transport_backend_info(net::TransportBackend::external_library).available) {
        return;
    }

    net::TransportHostDesc desc;
    desc.backend = net::TransportBackend::external_library;
    desc.external.server_id = core::NetId::from_value(1);
    desc.external.bind_endpoint = {"127.0.0.1", 0};
    desc.external.max_payload_bytes = 4096;
    desc.external.max_clients = 1;
    desc.external.reliability.max_in_flight = 1;
    auto transport = net::create_transport_host(desc);
    assert(transport);
    auto client = transport.value()->connect_client();
    assert(client);

    assert(transport.value()->send_client_to_server(
        client.value(), reliable_message(net::TransportMessageKind::command, 1, "first")));
    auto client_capacity = transport.value()->send_client_to_server(
        client.value(), reliable_message(net::TransportMessageKind::command, 2, "second"));
    assert(!client_capacity);
    assert(client_capacity.error().code == "transport_reliability.in_flight_limit_reached");

    auto server_messages = transport.value()->drain_server_messages();
    assert(server_messages.size() == 1);
    assert(server_messages.front().message.sequence == 1);
    auto client_messages = transport.value()->drain_client_messages(client.value());
    assert(client_messages && client_messages.value().empty());

    assert(transport.value()->send_client_to_server(
        client.value(), reliable_message(net::TransportMessageKind::command, 2, "second")));
    server_messages = transport.value()->drain_server_messages();
    assert(server_messages.size() == 1);
    assert(server_messages.front().message.sequence == 2);
    client_messages = transport.value()->drain_client_messages(client.value());
    assert(client_messages && client_messages.value().empty());

    assert(transport.value()->send_server_to_client(
        client.value(), reliable_message(net::TransportMessageKind::command_result, 1, "first")));
    auto server_capacity = transport.value()->send_server_to_client(
        client.value(), reliable_message(net::TransportMessageKind::command_result, 2, "second"));
    assert(!server_capacity);
    assert(server_capacity.error().code == "transport_reliability.in_flight_limit_reached");

    client_messages = transport.value()->drain_client_messages(client.value());
    assert(client_messages && client_messages.value().size() == 1);
    assert(client_messages.value().front().message.sequence == 1);
    server_messages = transport.value()->drain_server_messages();
    assert(server_messages.empty());

    assert(transport.value()->send_server_to_client(
        client.value(), reliable_message(net::TransportMessageKind::command_result, 2, "second")));
    client_messages = transport.value()->drain_client_messages(client.value());
    assert(client_messages && client_messages.value().size() == 1);
    assert(client_messages.value().front().message.sequence == 2);
}

void test_remote_endpoint_challenge_token_and_timeout() {
    if (!net::transport_backend_info(net::TransportBackend::external_library).available) {
        return;
    }

    net::TransportHostDesc desc;
    desc.backend = net::TransportBackend::external_library;
    desc.external.server_id = core::NetId::from_value(71);
    desc.external.bind_endpoint = {"127.0.0.1", 0};
    desc.external.max_payload_bytes = 4096;
    desc.external.max_clients = 2;
    desc.external.content_fingerprint = "base@remote-test";
    desc.external.idle_timeout_ms = 1'000;
    desc.external.keepalive_interval_ms = 100;
    auto host = net::create_transport_host(desc);
    assert(host);
    assert(host.value()->local_endpoint().has_value());

    net::ExternalTransportClientConfig client_config;
    client_config.server_endpoint = *host.value()->local_endpoint();
    client_config.max_payload_bytes = 4096;
    client_config.content_fingerprint = desc.external.content_fingerprint;
    client_config.idle_timeout_ms = 1'000;
    client_config.keepalive_interval_ms = 100;
    auto client = net::create_external_transport_client(client_config);
    assert(client);
    assert(client.value()->connect(0));

    auto host_hello = host.value()->poll_maintenance(0);
    assert(host_hello && host_hello.value().connected_clients.empty());
    auto client_challenge = client.value()->poll_maintenance(0);
    assert(client_challenge && !client_challenge.value().connected);
    auto host_response = host.value()->poll_maintenance(1);
    assert(host_response && host_response.value().connected_clients.size() == 1);
    const auto client_id = host_response.value().connected_clients.front();
    auto client_accept = client.value()->poll_maintenance(1);
    assert(client_accept && client_accept.value().connected);
    assert(client.value()->state() == net::TransportClientState::connected);
    assert(client.value()->client_id() == client_id);
    assert(client.value()->server_id() == desc.external.server_id);
    assert(client.value()->session_token().is_valid());

    assert(client.value()->send_to_server(
        reliable_message(net::TransportMessageKind::command, 1, "remote")));
    assert(host.value()->poll_maintenance(2));
    auto received = host.value()->drain_server_messages();
    assert(received.size() == 1);
    assert(received.front().sender == client_id);
    assert(received.front().message.payload == "remote");
    assert(received.front().session_token == client.value()->session_token());
    assert(client.value()->poll_maintenance(2));

    assert(host.value()->send_server_to_client(
        client_id, reliable_message(net::TransportMessageKind::command_result, 1, "accepted")));
    assert(client.value()->poll_maintenance(3));
    auto delivered = client.value()->drain_server_messages();
    assert(delivered.size() == 1);
    assert(delivered.front().message.payload == "accepted");
    assert(host.value()->poll_maintenance(3));

    auto timed_out = host.value()->poll_maintenance(1'004);
    assert(timed_out && timed_out.value().disconnected_clients == std::vector{client_id});
    assert(!host.value()->is_client_connected(client_id));
}

void test_remote_ipv6_endpoint_handshake() {
    if (!net::transport_backend_info(net::TransportBackend::external_library).available) {
        return;
    }

    net::TransportHostDesc desc;
    desc.backend = net::TransportBackend::external_library;
    desc.external.bind_endpoint = {"::1", 0};
    desc.external.content_fingerprint = "ipv6-test";
    auto host = net::create_transport_host(desc);
    if (!host && (host.error().code == "transport.socket_failed" ||
                  host.error().code == "transport.bind_failed")) {
        return;
    }
    assert(host && host.value()->local_endpoint().has_value());

    net::ExternalTransportClientConfig config;
    config.server_endpoint = *host.value()->local_endpoint();
    config.content_fingerprint = desc.external.content_fingerprint;
    auto client = net::create_external_transport_client(config);
    assert(client && client.value()->connect(0));
    assert(host.value()->poll_maintenance(0));
    assert(client.value()->poll_maintenance(0));
    auto accepted = host.value()->poll_maintenance(1);
    assert(accepted && accepted.value().connected_clients.size() == 1);
    auto connected = client.value()->poll_maintenance(1);
    assert(connected && connected.value().connected);
    assert(client.value()->state() == net::TransportClientState::connected);
}

void test_remote_endpoint_rejects_content_mismatch() {
    if (!net::transport_backend_info(net::TransportBackend::external_library).available) {
        return;
    }
    net::TransportHostDesc desc;
    desc.backend = net::TransportBackend::external_library;
    desc.external.bind_endpoint = {"127.0.0.1", 0};
    desc.external.content_fingerprint = "server-content";
    auto host = net::create_transport_host(desc);
    assert(host && host.value()->local_endpoint().has_value());

    net::ExternalTransportClientConfig config;
    config.server_endpoint = *host.value()->local_endpoint();
    config.content_fingerprint = "different-content";
    auto client = net::create_external_transport_client(config);
    assert(client && client.value()->connect(0));
    assert(host.value()->poll_maintenance(0));
    assert(client.value()->poll_maintenance(0));
    auto response = host.value()->poll_maintenance(1);
    assert(response && response.value().connected_clients.empty());
    auto rejected = client.value()->poll_maintenance(1);
    assert(rejected && rejected.value().disconnected);
    assert(rejected.value().disconnect_reason_code == "transport_handshake.content_mismatch");
    assert(client.value()->state() == net::TransportClientState::disconnected);
}

void test_remote_endpoint_rate_limits_unreliable_input() {
    if (!net::transport_backend_info(net::TransportBackend::external_library).available) {
        return;
    }
    net::TransportHostDesc desc;
    desc.backend = net::TransportBackend::external_library;
    desc.external.bind_endpoint = {"127.0.0.1", 0};
    desc.external.max_payload_bytes = 4096;
    desc.external.content_fingerprint = "rate-limit-test";
    desc.external.max_inbound_messages_per_second = 2;
    auto host = net::create_transport_host(desc);
    assert(host && host.value()->local_endpoint().has_value());

    net::ExternalTransportClientConfig config;
    config.server_endpoint = *host.value()->local_endpoint();
    config.max_payload_bytes = 4096;
    config.content_fingerprint = desc.external.content_fingerprint;
    auto client = net::create_external_transport_client(config);
    assert(client && client.value()->connect(0));
    assert(host.value()->poll_maintenance(0));
    assert(client.value()->poll_maintenance(0));
    auto accepted = host.value()->poll_maintenance(1);
    assert(accepted && accepted.value().connected_clients.size() == 1);
    assert(client.value()->poll_maintenance(1));

    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        assert(client.value()->send_to_server({net::TransportMessageKind::control,
                                               net::TransportChannel::unreliable, sequence,
                                               "test.input", "payload", 2}));
    }
    auto maintenance = host.value()->poll_maintenance(2);
    assert(maintenance && maintenance.value().rate_limited_datagram_count == 1);
    auto messages = host.value()->drain_server_messages();
    assert(messages.size() == 2);
}

void test_fragment_reassembly_is_scoped_and_expires() {
    net::TransportPacketFragmentCodecConfig config;
    config.max_fragment_payload_bytes = 4;
    config.max_packet_bytes = 8;
    config.max_fragment_count = 2;
    config.max_pending_packets = 2;
    config.max_pending_packet_bytes = 16;
    config.pending_packet_timeout_ms = 50;
    net::TransportPacketReassembler reassembler(config);

    const net::TransportPacketFragment first_a{7, 0, 2, 8, "AAAA"};
    const net::TransportPacketFragment second_a{7, 1, 2, 8, "aaaa"};
    const net::TransportPacketFragment first_b{7, 0, 2, 8, "BBBB"};
    const net::TransportPacketFragment second_b{7, 1, 2, 8, "bbbb"};
    auto a = reassembler.accept_fragment(10, first_a, 0);
    auto b = reassembler.accept_fragment(20, first_b, 0);
    assert(a && b && !a.value().complete && !b.value().complete);
    assert(reassembler.pending_packet_count() == 2);
    assert(reassembler.pending_packet_bytes() == 16);

    a = reassembler.accept_fragment(10, second_a, 10);
    assert(a && a.value().complete && a.value().packet == "AAAAaaaa");
    assert(reassembler.pending_packet_count() == 1);
    b = reassembler.accept_fragment(20, second_b, 10);
    assert(b && b.value().complete && b.value().packet == "BBBBbbbb");
    assert(reassembler.pending_packet_count() == 0);
    assert(reassembler.pending_packet_bytes() == 0);

    config.max_pending_packets = 1;
    config.max_pending_packet_bytes = 8;
    net::TransportPacketReassembler expiring(config);
    assert(expiring.accept_fragment(10, first_a, 100));
    auto at_capacity = expiring.accept_fragment(20, first_b, 149);
    assert(!at_capacity);
    assert(at_capacity.error().code == "transport_fragment.pending_budget_exceeded");
    assert(expiring.pending_packet_count() == 1);
    assert(expiring.expire(150) == 1);
    assert(expiring.pending_packet_count() == 0);
    assert(expiring.accept_fragment(20, first_b, 150));
    expiring.discard_source(20);
    assert(expiring.pending_packet_count() == 0);
}

void test_reliable_commands_are_delivered_only_after_gaps_close() {
    const auto client_id = core::NetId::from_value(2);
    const auto server_id = core::NetId::from_value(1);
    const auto envelope = [client_id, server_id](std::uint64_t sequence, std::string payload) {
        return net::TransportEnvelope{
            client_id, server_id,
            reliable_message(net::TransportMessageKind::command, sequence, std::move(payload))};
    };

    net::TransportReliableCommandSequencer sequencer(2);
    auto second = sequencer.accept(envelope(2, "second"));
    assert(second && second.value().empty());
    assert(sequencer.next_expected_sequence() == 1);
    assert(sequencer.pending_count() == 1);

    auto duplicate_second = sequencer.accept(envelope(2, "duplicate-second"));
    assert(duplicate_second && duplicate_second.value().empty());
    assert(sequencer.pending_count() == 1);

    auto third = sequencer.accept(envelope(3, "third"));
    assert(third && third.value().empty());
    assert(sequencer.pending_count() == 2);
    auto beyond_window = sequencer.accept(envelope(4, "fourth"));
    assert(!beyond_window);
    assert(beyond_window.error().code == "transport_reliability.sequence_window_exceeded");

    auto first = sequencer.accept(envelope(1, "first"));
    assert(first && first.value().size() == 3);
    assert(first.value()[0].message.sequence == 1);
    assert(first.value()[1].message.sequence == 2);
    assert(first.value()[2].message.sequence == 3);
    assert(first.value()[1].message.payload == "second");
    assert(sequencer.next_expected_sequence() == 4);
    assert(sequencer.pending_count() == 0);

    auto stale = sequencer.accept(envelope(2, "stale"));
    assert(stale && stale.value().empty());
}

void test_host_retries_responses_without_redispatching_drained_commands() {
    FaultInjectingTransportHost* transport = nullptr;
    auto session = make_fault_injected_session(transport);
    assert(session.start());
    assert(transport != nullptr);
    auto client = session.connect_client();
    assert(client);
    auto welcome = session.drain_client_messages(client.value());
    assert(welcome && welcome.value().size() == 1);

    std::uint32_t dispatch_count = 0;
    auto dispatcher = make_mutating_dispatcher(dispatch_count);
    transport->fail_server_send(client.value(), net::TransportMessageKind::command_result, 1);
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        assert(session.send_client_command(
            client.value(), net::CommandEnvelope{sequence, client.value(), "test.mutate",
                                                 std::to_string(sequence), 0}));
    }

    auto first_tick = session.tick(dispatcher, net::CommandExecutionContext{});
    assert(first_tick);
    assert(dispatch_count == 3);
    assert(first_tick.value().command_reports.size() == 3);
    assert(first_tick.value().response_message_count == 3);
    assert(first_tick.value().replication_message_count == 3);
    assert(first_tick.value().outbound_delivery.attempted_message_count == 1);
    assert(first_tick.value().outbound_delivery.delivered_message_count == 0);
    assert(first_tick.value().outbound_delivery.failed_attempt_count == 1);
    assert(first_tick.value().outbound_delivery.pending_message_count == 6);
    assert(first_tick.value().outbound_delivery.blocked_client_count == 1);
    assert(first_tick.value().outbound_delivery.failures.size() == 1);
    assert(first_tick.value().outbound_delivery.failures.front().error_code ==
           "test.injected_send_failure");
    const auto deferred_inspection = debug::Inspector::inspect(first_tick.value());
    assert(deferred_inspection.state == "delivery_deferred");
    assert(deferred_inspection.find_field("outbound_delivery_pending_message_count")->value == "6");
    assert(deferred_inspection.find_field("first_delivery_failure_client_id")->value == "2");
    bool reported_deferred_delivery = false;
    for (const auto& issue : deferred_inspection.issues) {
        reported_deferred_delivery =
            reported_deferred_delivery || issue.code == "host_tick.outbound_delivery_deferred";
    }
    assert(reported_deferred_delivery);
    assert(!deferred_inspection.has_errors());
    assert(session.pending_outbound_message_count() == 6);
    auto before_retry = session.drain_client_messages(client.value());
    assert(before_retry && before_retry.value().empty());

    auto retry_tick = session.tick(dispatcher, net::CommandExecutionContext{});
    assert(retry_tick);
    assert(dispatch_count == 3);
    assert(retry_tick.value().command_reports.empty());
    assert(retry_tick.value().outbound_delivery.attempted_message_count == 6);
    assert(retry_tick.value().outbound_delivery.delivered_message_count == 6);
    assert(retry_tick.value().outbound_delivery.retry_attempt_count == 1);
    assert(retry_tick.value().outbound_delivery.failed_attempt_count == 0);
    assert(retry_tick.value().outbound_delivery.pending_message_count == 0);
    assert(debug::Inspector::inspect(retry_tick.value()).state == "outbound_delivery");
    assert(session.pending_outbound_message_count() == 0);

    auto delivered = session.drain_client_messages(client.value());
    assert(delivered && delivered.value().size() == 6);
    for (std::size_t index = 0; index < delivered.value().size(); ++index) {
        const auto expected_sequence = static_cast<std::uint64_t>(index / 2 + 1);
        const auto expected_kind = index % 2 == 0 ? net::TransportMessageKind::command_result
                                                  : net::TransportMessageKind::replication;
        assert(delivered.value()[index].message.sequence == expected_sequence);
        assert(delivered.value()[index].message.kind == expected_kind);
    }
}

void test_host_send_failure_blocks_only_the_affected_client() {
    FaultInjectingTransportHost* transport = nullptr;
    auto session = make_fault_injected_session(transport);
    assert(session.start());
    auto origin = session.connect_client();
    auto observer = session.connect_client();
    assert(origin && observer && transport != nullptr);
    assert(session.drain_client_messages(origin.value()));
    assert(session.drain_client_messages(observer.value()));

    std::uint32_t dispatch_count = 0;
    auto dispatcher = make_mutating_dispatcher(dispatch_count);
    transport->fail_server_send(observer.value(), net::TransportMessageKind::replication, 1, 2);
    assert(session.send_client_command(
        origin.value(), net::CommandEnvelope{1, origin.value(), "test.mutate", "isolated", 0}));

    auto first_tick = session.tick(dispatcher, net::CommandExecutionContext{});
    assert(first_tick && dispatch_count == 1);
    assert(first_tick.value().outbound_delivery.delivered_message_count == 2);
    assert(first_tick.value().outbound_delivery.failed_attempt_count == 1);
    assert(first_tick.value().outbound_delivery.pending_message_count == 1);
    assert(first_tick.value().outbound_delivery.blocked_client_count == 1);
    auto origin_delivery = session.drain_client_messages(origin.value());
    auto observer_delivery = session.drain_client_messages(observer.value());
    assert(origin_delivery && origin_delivery.value().size() == 2);
    assert(observer_delivery && observer_delivery.value().empty());

    assert(session.send_client_command(
        origin.value(), net::CommandEnvelope{2, origin.value(), "test.mutate", "healthy", 0}));
    auto blocked_tick = session.tick(dispatcher, net::CommandExecutionContext{});
    assert(blocked_tick && dispatch_count == 2);
    assert(blocked_tick.value().transport_message_count == 1);
    assert(blocked_tick.value().command_reports.size() == 1);
    assert(blocked_tick.value().outbound_delivery.delivered_message_count == 2);
    assert(blocked_tick.value().outbound_delivery.failed_attempt_count == 1);
    assert(blocked_tick.value().outbound_delivery.pending_message_count == 2);
    origin_delivery = session.drain_client_messages(origin.value());
    observer_delivery = session.drain_client_messages(observer.value());
    assert(origin_delivery && origin_delivery.value().size() == 2);
    assert(observer_delivery && observer_delivery.value().empty());

    auto recovered_tick = session.tick(dispatcher, net::CommandExecutionContext{});
    assert(recovered_tick && dispatch_count == 2);
    assert(recovered_tick.value().outbound_delivery.delivered_message_count == 2);
    assert(recovered_tick.value().outbound_delivery.retry_attempt_count == 1);
    assert(recovered_tick.value().outbound_delivery.pending_message_count == 0);
    observer_delivery = session.drain_client_messages(observer.value());
    assert(observer_delivery && observer_delivery.value().size() == 2);
    assert(observer_delivery.value()[0].message.kind == net::TransportMessageKind::replication);
    assert(observer_delivery.value()[0].message.sequence == 1);
    assert(observer_delivery.value()[1].message.sequence == 2);
}

void test_host_backpressures_commands_while_committed_delivery_is_blocked() {
    FaultInjectingTransportHost* transport = nullptr;
    auto session = make_fault_injected_session(transport);
    assert(session.start());
    auto client = session.connect_client();
    assert(client && transport != nullptr);
    assert(session.drain_client_messages(client.value()));

    std::uint32_t dispatch_count = 0;
    auto dispatcher = make_mutating_dispatcher(dispatch_count);
    transport->fail_server_send(client.value(), net::TransportMessageKind::command_result, 1, 2);
    assert(session.send_client_command(
        client.value(), net::CommandEnvelope{1, client.value(), "test.mutate", "first", 0}));

    auto first_tick = session.tick(dispatcher, net::CommandExecutionContext{});
    assert(first_tick && dispatch_count == 1);
    assert(first_tick.value().outbound_delivery.pending_message_count == 2);

    assert(session.send_client_command(
        client.value(), net::CommandEnvelope{2, client.value(), "test.mutate", "second", 0}));
    auto blocked_tick = session.tick(dispatcher, net::CommandExecutionContext{});
    assert(blocked_tick && dispatch_count == 2);
    assert(blocked_tick.value().transport_message_count == 1);
    assert(blocked_tick.value().command_reports.size() == 1);
    assert(blocked_tick.value().outbound_delivery.failed_attempt_count == 1);
    assert(blocked_tick.value().outbound_delivery.pending_message_count == 4);

    auto recovered_tick = session.tick(dispatcher, net::CommandExecutionContext{});
    assert(recovered_tick && dispatch_count == 2);
    assert(recovered_tick.value().transport_message_count == 0);
    assert(recovered_tick.value().command_reports.empty());
    assert(recovered_tick.value().outbound_delivery.pending_message_count == 0);

    auto delivered = session.drain_client_messages(client.value());
    assert(delivered && delivered.value().size() == 4);
    for (std::size_t index = 0; index < delivered.value().size(); ++index) {
        assert(delivered.value()[index].message.sequence ==
               static_cast<std::uint64_t>(index / 2 + 1));
        assert(delivered.value()[index].message.kind ==
               (index % 2 == 0 ? net::TransportMessageKind::command_result
                               : net::TransportMessageKind::replication));
    }
}

void test_host_defers_reliable_application_replication_without_overtaking() {
    FaultInjectingTransportHost* transport = nullptr;
    auto session = make_fault_injected_session(transport);
    assert(session.start());
    auto client = session.connect_client();
    assert(client && transport != nullptr);
    assert(session.drain_client_messages(client.value()));

    transport->fail_server_send(client.value(), net::TransportMessageKind::replication, 1);
    assert(session.send_replication_message(client.value(), {net::TransportMessageKind::replication,
                                                             net::TransportChannel::reliable, 1,
                                                             "test.snapshot", "first", 0}));
    assert(session.send_replication_message(client.value(), {net::TransportMessageKind::replication,
                                                             net::TransportChannel::reliable, 2,
                                                             "test.snapshot", "second", 0}));
    assert(session.pending_outbound_message_count() == 2);
    auto before_retry = session.drain_client_messages(client.value());
    assert(before_retry && before_retry.value().empty());

    net::ServerCommandDispatcher dispatcher;
    auto failed_delivery = session.tick(dispatcher, net::CommandExecutionContext{});
    assert(failed_delivery);
    assert(failed_delivery.value().outbound_delivery.failed_attempt_count == 1);
    assert(failed_delivery.value().outbound_delivery.pending_message_count == 2);
    auto retry = session.tick(dispatcher, net::CommandExecutionContext{});
    assert(retry);
    assert(retry.value().outbound_delivery.delivered_message_count == 2);
    assert(retry.value().outbound_delivery.retry_attempt_count == 1);
    assert(retry.value().outbound_delivery.pending_message_count == 0);

    auto delivered = session.drain_client_messages(client.value());
    assert(delivered && delivered.value().size() == 2);
    assert(delivered.value()[0].message.sequence == 1);
    assert(delivered.value()[0].message.payload == "first");
    assert(delivered.value()[1].message.sequence == 2);
    assert(delivered.value()[1].message.payload == "second");
}

void test_host_disconnects_when_the_final_notice_cannot_be_delivered() {
    FaultInjectingTransportHost* transport = nullptr;
    auto session = make_fault_injected_session(transport);
    assert(session.start());
    auto client = session.connect_client();
    assert(client && transport != nullptr);
    assert(session.drain_client_messages(client.value()));

    transport->fail_server_send(client.value(), net::TransportMessageKind::control, 0);
    assert(session.disconnect_client(client.value()));
    assert(session.connected_client_count() == 0);
    assert(!transport->is_client_connected(client.value()));
}

void test_reliable_backlog_drains_fairly_and_recovers_within_two_ticks() {
    net::HostSessionConfig config;
    config.max_outbound_bytes_per_client_per_second = 1024u * 1024u;
    config.max_pending_reliable_messages = 8;
    config.max_pending_reliable_bytes = 1024u * 1024u;
    config.max_pending_reliable_messages_per_client = 4;
    config.max_pending_reliable_bytes_per_client = 512u * 1024u;
    config.max_reliable_delivery_messages_per_tick = 2;
    config.max_reliable_delivery_bytes_per_tick = 1024u * 1024u;
    config.max_reliable_delivery_messages_per_client_per_tick = 1;
    config.max_reliable_delivery_bytes_per_client_per_tick = 512u * 1024u;
    net::HostSession session(config);
    assert(session.start());
    auto first = session.connect_client();
    auto second = session.connect_client();
    assert(first && second);
    assert(session.drain_client_messages(first.value()));
    assert(session.drain_client_messages(second.value()));

    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        assert(session.send_replication_message(
            first.value(), reliable_message(net::TransportMessageKind::replication, sequence,
                                            "first-" + std::to_string(sequence))));
        assert(session.send_replication_message(
            second.value(), reliable_message(net::TransportMessageKind::replication, sequence,
                                             "second-" + std::to_string(sequence))));
    }
    assert(session.pending_outbound_message_count() == 4);
    assert(session.pending_outbound_message_count(first.value()) == 2);
    assert(session.pending_outbound_message_count(second.value()) == 2);
    assert(session.pending_outbound_bytes() == session.pending_outbound_bytes(first.value()) +
                                                   session.pending_outbound_bytes(second.value()));

    net::ServerCommandDispatcher dispatcher;
    net::CommandExecutionContext context;
    context.server_time_ms = 10;
    auto first_tick = session.tick(dispatcher, context);
    assert(first_tick);
    const auto& first_delivery = first_tick.value().outbound_delivery;
    assert(first_delivery.initial_pending_message_count == 4);
    assert(first_delivery.attempted_message_count == 2);
    assert(first_delivery.delivered_message_count == 2);
    assert(first_delivery.attempted_bytes == first_delivery.delivered_bytes);
    assert(first_delivery.pending_message_count == 2);
    assert(first_delivery.pending_bytes == session.pending_outbound_bytes());
    assert(first_delivery.blocked_client_count == 2);
    assert(first_delivery.tick_budget_deferred_message_count == 2);
    assert(net::validate_host_session_outbound_delivery_report(first_delivery));
    assert(!debug::Inspector::inspect(first_tick.value()).has_errors());

    auto first_messages = session.drain_client_messages(first.value());
    auto second_messages = session.drain_client_messages(second.value());
    assert(first_messages && first_messages.value().size() == 1);
    assert(second_messages && second_messages.value().size() == 1);
    assert(first_messages.value().front().message.sequence == 1);
    assert(second_messages.value().front().message.sequence == 1);

    context.server_time_ms = 27;
    auto recovered_tick = session.tick(dispatcher, context);
    assert(recovered_tick);
    const auto& recovered_delivery = recovered_tick.value().outbound_delivery;
    assert(recovered_delivery.initial_pending_message_count == 2);
    assert(recovered_delivery.delivered_message_count == 2);
    assert(recovered_delivery.pending_message_count == 0);
    assert(recovered_delivery.pending_bytes == 0);
    assert(recovered_delivery.blocked_client_count == 0);
    assert(recovered_delivery.tick_budget_deferred_message_count == 0);
    assert(net::validate_host_session_outbound_delivery_report(recovered_delivery));
    assert(session.pending_outbound_message_count() == 0);
    assert(session.pending_outbound_bytes() == 0);
    assert(!debug::Inspector::inspect(recovered_tick.value()).has_errors());

    first_messages = session.drain_client_messages(first.value());
    second_messages = session.drain_client_messages(second.value());
    assert(first_messages && first_messages.value().size() == 1);
    assert(second_messages && second_messages.value().size() == 1);
    assert(first_messages.value().front().message.sequence == 2);
    assert(second_messages.value().front().message.sequence == 2);
}

void test_local_bootstrap_flush_hydrates_before_publication() {
    net::HostSessionConfig config;
    config.max_outbound_bytes_per_client_per_second = 1'024;
    config.max_reliable_delivery_messages_per_tick = 1;
    config.max_reliable_delivery_bytes_per_tick = 1'024;
    config.max_reliable_delivery_messages_per_client_per_tick = 1;
    config.max_reliable_delivery_bytes_per_client_per_tick = 1'024;
    net::HostSession session(config);
    assert(session.start());
    auto client = session.connect_client();
    assert(client);
    assert(session.drain_client_messages(client.value()));

    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        assert(session.send_replication_message(
            client.value(), reliable_message(net::TransportMessageKind::replication, sequence,
                                             "bootstrap-" + std::to_string(sequence))));
    }
    net::HostSessionOutboundDeliveryReport delivery;
    assert(session.flush_local_client_bootstrap(client.value(), delivery));
    assert(delivery.initial_pending_message_count == 3);
    assert(delivery.attempted_message_count == 3);
    assert(delivery.delivered_message_count == 3);
    assert(delivery.attempted_bytes == delivery.delivered_bytes);
    assert(delivery.pending_message_count == 0);
    assert(delivery.pending_bytes == 0);
    assert(net::validate_host_session_outbound_delivery_report(delivery));
    assert(session.pending_outbound_message_count() == 0);

    auto messages = session.drain_client_messages(client.value());
    assert(messages && messages.value().size() == 3);
    for (std::size_t index = 0; index < messages.value().size(); ++index) {
        assert(messages.value()[index].message.sequence == index + 1);
    }
    net::HostSessionOutboundDeliveryReport duplicate_delivery;
    auto duplicate = session.flush_local_client_bootstrap(client.value(), duplicate_delivery);
    assert(!duplicate);
    assert(duplicate.error().code == "host_session.bootstrap_flush_not_pending");
}

void test_reliable_backlog_fails_closed_at_admission_and_after_commit() {
    net::HostSessionConfig undeliverable_config;
    undeliverable_config.max_outbound_bytes_per_client_per_second = 1;
    undeliverable_config.max_reliable_delivery_bytes_per_tick = 1;
    undeliverable_config.max_reliable_delivery_bytes_per_client_per_tick = 1;
    net::HostSession undeliverable_session(undeliverable_config);
    assert(undeliverable_session.start());
    auto undeliverable_client = undeliverable_session.connect_client();
    assert(undeliverable_client);
    assert(undeliverable_session.drain_client_messages(undeliverable_client.value()));
    auto oversized = undeliverable_session.send_replication_message(
        undeliverable_client.value(),
        reliable_message(net::TransportMessageKind::replication, 1, "cannot-fit"));
    assert(!oversized);
    assert(oversized.error().code == "host_session.reliable_message_exceeds_delivery_budget");
    assert(undeliverable_session.pending_outbound_message_count() == 0);

    net::HostSessionConfig admission_config;
    admission_config.max_pending_reliable_messages = 2;
    admission_config.max_pending_reliable_messages_per_client = 2;
    auto invalid_config = admission_config;
    invalid_config.max_reliable_delivery_bytes_per_client_per_tick = 0;
    assert(!invalid_config.validate());
    net::HostSession invalid_session(invalid_config);
    assert(!invalid_session.start());
    net::HostSession admission_session(admission_config);
    assert(admission_session.start());
    auto admitted_client = admission_session.connect_client();
    assert(admitted_client);
    assert(admission_session.drain_client_messages(admitted_client.value()));
    assert(admission_session.send_replication_message(
        admitted_client.value(),
        reliable_message(net::TransportMessageKind::replication, 1, "first")));
    assert(admission_session.send_replication_message(
        admitted_client.value(),
        reliable_message(net::TransportMessageKind::replication, 2, "second")));
    auto rejected = admission_session.send_replication_message(
        admitted_client.value(),
        reliable_message(net::TransportMessageKind::replication, 3, "rejected"));
    assert(!rejected);
    assert(rejected.error().code == "host_session.reliable_backlog_client_message_limit");
    assert(admission_session.pending_outbound_message_count() == 2);
    assert(admission_session.connected_client_count() == 1);

    net::HostSessionConfig commit_config;
    commit_config.max_pending_reliable_messages = 2;
    commit_config.max_pending_reliable_messages_per_client = 1;
    net::HostSession commit_session(commit_config);
    assert(commit_session.start());
    auto committed_client = commit_session.connect_client();
    assert(committed_client);
    assert(commit_session.drain_client_messages(committed_client.value()));
    std::uint32_t dispatch_count = 0;
    auto dispatcher = make_mutating_dispatcher(dispatch_count);
    assert(commit_session.send_client_command(
        committed_client.value(),
        net::CommandEnvelope{1, committed_client.value(), "test.mutate", "overload", 0}));
    auto tick = commit_session.tick(dispatcher, net::CommandExecutionContext{});
    assert(tick && dispatch_count == 1);
    assert(tick.value().command_reports.size() == 1);
    assert(tick.value().command_reports.front().committed_world_mutation);
    assert(tick.value().replication_message_count == 1);
    assert(tick.value().replication_relevance_reports.size() == 1);
    assert(tick.value().replication_relevance_reports.front().relevant_client_count == 1);
    assert(tick.value().outbound_delivery.overload_disconnected_client_count == 1);
    assert(tick.value().outbound_delivery.overload_disconnected_clients ==
           std::vector{committed_client.value()});
    assert(std::ranges::find(tick.value().disconnected_clients, committed_client.value()) !=
           tick.value().disconnected_clients.end());
    assert(commit_session.connected_client_count() == 0);
    assert(commit_session.pending_outbound_message_count() == 0);
    assert(net::validate_host_session_outbound_delivery_report(tick.value().outbound_delivery));
    auto invalid_delivery = tick.value().outbound_delivery;
    invalid_delivery.delivered_bytes = invalid_delivery.attempted_bytes + 1;
    auto invalid_delivery_status =
        net::validate_host_session_outbound_delivery_report(invalid_delivery);
    assert(!invalid_delivery_status);
    assert(invalid_delivery_status.error().code == "host_session.invalid_delivery_byte_totals");
    const auto inspection = debug::Inspector::inspect(tick.value());
    assert(inspection.state == "reliable_overload");
    assert(!inspection.has_errors());
}

void test_host_hard_caps_unreliable_outbound_bandwidth() {
    net::HostSessionConfig config;
    config.max_outbound_bytes_per_client_per_second = 1;
    net::HostSession session(config);
    assert(session.start());
    auto client = session.connect_client();
    assert(client);
    auto welcome = session.drain_client_messages(client.value());
    assert(welcome && welcome.value().size() == 1);

    assert(session.send_replication_message(client.value(), {net::TransportMessageKind::replication,
                                                             net::TransportChannel::unreliable, 1,
                                                             "test.snapshot", "latest", 0}));
    auto delivered = session.drain_client_messages(client.value());
    assert(delivered && delivered.value().empty());

    net::ServerCommandDispatcher dispatcher;
    net::CommandExecutionContext context;
    context.server_time_ms = 10;
    auto tick = session.tick(dispatcher, context);
    assert(tick);
    assert(tick.value().outbound_budget_dropped_unreliable_message_count == 1);
    const auto inspection = debug::Inspector::inspect(tick.value());
    assert(inspection.find_field("outbound_budget_dropped_unreliable_message_count")->value == "1");
}

} // namespace

int main() {
    test_tracking_can_be_rolled_back_before_retry();
    test_external_capacity_failure_does_not_leak_untracked_datagram();
    test_remote_endpoint_challenge_token_and_timeout();
    test_remote_ipv6_endpoint_handshake();
    test_remote_endpoint_rejects_content_mismatch();
    test_remote_endpoint_rate_limits_unreliable_input();
    test_fragment_reassembly_is_scoped_and_expires();
    test_reliable_commands_are_delivered_only_after_gaps_close();
    test_host_retries_responses_without_redispatching_drained_commands();
    test_host_send_failure_blocks_only_the_affected_client();
    test_host_backpressures_commands_while_committed_delivery_is_blocked();
    test_host_defers_reliable_application_replication_without_overtaking();
    test_host_disconnects_when_the_final_notice_cannot_be_delivered();
    test_reliable_backlog_drains_fairly_and_recovers_within_two_ticks();
    test_local_bootstrap_flush_hydrates_before_publication();
    test_reliable_backlog_fails_closed_at_admission_and_after_commit();
    test_host_hard_caps_unreliable_outbound_bandwidth();
    return 0;
}
