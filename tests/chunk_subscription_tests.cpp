#include "engine/net/client_session.hpp"
#include "engine/net/host_session.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/chunks/chunk_replication.hpp"
#include "engine/world/chunks/chunk_subscription.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace heartstead;

void assert_valid_subscriptions(const std::vector<world::ChunkCoord>& subscriptions,
                                std::size_t maximum_count) {
    assert(subscriptions.size() <= maximum_count);
    assert(std::ranges::is_sorted(subscriptions));
    assert(std::ranges::adjacent_find(subscriptions) == subscriptions.end());
}

void accept_welcome(net::HostSession& host, core::NetId client_id, net::ClientSession& client) {
    auto messages = host.drain_client_messages(client_id);
    assert(messages);
    assert(messages.value().size() == 1);
    assert(client.receive_server_message(messages.value().front()));
}

void test_default_policy_converges_incrementally() {
    const world::ChunkSubscriptionPolicy policy;
    assert(policy.validate());
    assert(policy.desired_chunk_count() == 39);

    const world::ChunkCoord center{10, -4, 7};
    auto first = world::plan_chunk_subscriptions(
        {}, center, policy, {policy.max_additions_per_update, policy.max_removals_per_update});
    assert(first);
    assert(first.value().initial_subscription_count == 0);
    assert(first.value().desired_subscription_count == 39);
    assert(first.value().added_chunks.size() == policy.max_additions_per_update);
    assert(first.value().added_chunks.front() == center);
    assert(first.value().removed_chunks.empty());
    assert(first.value().deferred_addition_count == 35);
    assert(!first.value().converged());

    auto subscriptions = first.value().subscriptions;
    for (std::size_t update = 0; update < 16 && subscriptions.size() < 39; ++update) {
        auto plan = world::plan_chunk_subscriptions(
            subscriptions, center, policy,
            {policy.max_additions_per_update, policy.max_removals_per_update});
        assert(plan);
        assert(plan.value().added_chunks.size() <= policy.max_additions_per_update);
        assert(plan.value().removed_chunks.size() <= policy.max_removals_per_update);
        assert_valid_subscriptions(plan.value().subscriptions, policy.max_chunks_per_client);
        subscriptions = std::move(plan).value().subscriptions;
    }
    assert(subscriptions.size() == policy.desired_chunk_count());

    auto converged = world::plan_chunk_subscriptions(
        subscriptions, center, policy,
        {policy.max_additions_per_update, policy.max_removals_per_update});
    assert(converged);
    assert(converged.value().converged());
    assert(converged.value().added_chunks.empty());
    assert(converged.value().removed_chunks.empty());
    assert(converged.value().hysteresis_retained_count == 0);
}

void test_hysteresis_and_teleport_transitions_stay_bounded() {
    const world::ChunkSubscriptionPolicy policy;
    std::vector<world::ChunkCoord> subscriptions;
    while (subscriptions.size() < policy.desired_chunk_count()) {
        auto plan = world::plan_chunk_subscriptions(
            subscriptions, {0, 0, 0}, policy,
            {policy.max_additions_per_update, policy.max_removals_per_update});
        assert(plan);
        subscriptions = std::move(plan).value().subscriptions;
    }

    auto short_move = world::plan_chunk_subscriptions(
        subscriptions, {1, 0, 0}, policy,
        {policy.max_additions_per_update, policy.max_removals_per_update});
    assert(short_move);
    assert(short_move.value().removed_chunks.empty());
    assert(short_move.value().added_chunks.size() == policy.max_additions_per_update);
    assert(short_move.value().hysteresis_retained_count > 0);
    assert_valid_subscriptions(short_move.value().subscriptions, policy.max_chunks_per_client);

    const auto before_teleport_count = short_move.value().subscriptions.size();
    auto teleport = world::plan_chunk_subscriptions(short_move.value().subscriptions, {100, 0, 0},
                                                    policy, {2, 3});
    assert(teleport);
    assert(teleport.value().removed_chunks.size() == 3);
    assert(teleport.value().added_chunks.size() == 2);
    assert(teleport.value().subscriptions.size() == before_teleport_count - 1);
    assert(teleport.value().deferred_removal_count == before_teleport_count - 3);
    assert(teleport.value().deferred_addition_count == policy.desired_chunk_count() - 2);
    assert_valid_subscriptions(teleport.value().subscriptions, policy.max_chunks_per_client);
}

void test_capacity_pressure_does_not_starve_current_interest() {
    world::ChunkSubscriptionPolicy policy;
    policy.max_chunks_per_client = policy.desired_chunk_count();
    std::vector<world::ChunkCoord> subscriptions;
    while (subscriptions.size() < policy.desired_chunk_count()) {
        auto plan = world::plan_chunk_subscriptions(
            subscriptions, {0, 0, 0}, policy,
            {policy.max_additions_per_update, policy.max_removals_per_update});
        assert(plan);
        subscriptions = std::move(plan).value().subscriptions;
    }

    bool converged = false;
    for (std::size_t update = 0; update < 16; ++update) {
        auto plan = world::plan_chunk_subscriptions(
            subscriptions, {1, 0, 0}, policy,
            {policy.max_additions_per_update, policy.max_removals_per_update});
        assert(plan);
        assert(plan.value().subscriptions.size() == policy.max_chunks_per_client);
        assert(plan.value().added_chunks.size() == plan.value().removed_chunks.size());
        assert_valid_subscriptions(plan.value().subscriptions, policy.max_chunks_per_client);
        converged = plan.value().converged();
        subscriptions = std::move(plan).value().subscriptions;
        if (converged) {
            break;
        }
    }
    assert(converged);
}

void test_invalid_policies_and_inputs_are_rejected() {
    world::ChunkSubscriptionPolicy policy;
    policy.subscribe_horizontal_radius_chunks =
        static_cast<std::uint16_t>(policy.retain_horizontal_radius_chunks + 1);
    assert(!policy.validate());

    policy = {};
    policy.max_chunks_per_client = policy.desired_chunk_count() - 1;
    assert(!policy.validate());

    policy = {};
    policy.max_additions_per_update = 0;
    assert(!policy.validate());

    policy = {};
    const std::array duplicate_subscriptions{world::ChunkCoord{1, 2, 3},
                                             world::ChunkCoord{1, 2, 3}};
    auto duplicate = world::plan_chunk_subscriptions(
        duplicate_subscriptions, {}, policy,
        {policy.max_additions_per_update, policy.max_removals_per_update});
    assert(!duplicate);
    assert(duplicate.error().code == "chunk_subscription.duplicate_existing");
}

void test_extreme_centers_remain_unique_and_bounded() {
    const world::ChunkSubscriptionPolicy policy;
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    for (const auto center : {world::ChunkCoord{minimum, minimum, minimum},
                              world::ChunkCoord{maximum, maximum, maximum}}) {
        auto plan = world::plan_chunk_subscriptions(
            {}, center, policy, {policy.max_chunks_per_client, policy.max_chunks_per_client});
        assert(plan);
        assert(!plan.value().subscriptions.empty());
        assert(std::ranges::binary_search(plan.value().subscriptions, center));
        assert_valid_subscriptions(plan.value().subscriptions, policy.max_chunks_per_client);
    }
}

void test_removal_protocol_round_trip_and_validation() {
    const world::ChunkSubscriptionRemoval expected{{
        std::numeric_limits<std::int64_t>::min(),
        17,
        std::numeric_limits<std::int64_t>::max(),
    }};
    const auto server_id = core::NetId::from_value(41);
    const auto client_id = core::NetId::from_value(42);
    auto envelope = net::TransportEnvelope{
        server_id, client_id, world::make_chunk_subscription_removal_message(expected, 9, 11)};
    auto decoded = world::chunk_subscription_removal_from_transport(envelope);
    assert(decoded);
    assert(decoded.value() == expected);

    auto unreliable = envelope;
    unreliable.message.channel = net::TransportChannel::unreliable;
    assert(!world::chunk_subscription_removal_from_transport(unreliable));

    auto wrong_type = envelope;
    wrong_type.message.payload_type = "test.not_a_removal";
    assert(!world::chunk_subscription_removal_from_transport(wrong_type));

    auto trailing = envelope;
    trailing.message.payload.push_back('\0');
    assert(!world::chunk_subscription_removal_from_transport(trailing));
}

void test_multi_type_drain_preserves_replication_order() {
    net::HostSession host(net::HostSessionConfig{
        net::TransportHostDesc{
            net::TransportBackend::in_memory,
            net::InMemoryTransportHostConfig{core::NetId::from_value(80), 4096},
        },
        net::ReplicationRelevancePolicy{},
    });
    assert(host.start());
    auto client_id = host.connect_client();
    assert(client_id);
    net::ClientSession client(client_id.value());
    accept_welcome(host, client_id.value(), client);

    const std::array<std::string_view, 4> payload_types{
        "test.a", "test.ignore", world::chunk_subscription_removal_payload_type, "test.b"};
    for (std::size_t index = 0; index < payload_types.size(); ++index) {
        net::TransportMessage message;
        message.kind = net::TransportMessageKind::replication;
        message.channel = net::TransportChannel::reliable;
        message.sequence = index + 1;
        message.payload_type = payload_types[index];
        message.payload = std::to_string(index + 1);
        assert(host.send_replication_message(client_id.value(), std::move(message)));
    }
    net::HostSessionTickResult delivery_tick;
    assert(host.flush_outbound(delivery_tick));
    auto delivered = host.drain_client_messages(client_id.value());
    assert(delivered && delivered.value().size() == payload_types.size());
    for (const auto& envelope : delivered.value()) {
        assert(client.receive_server_message(envelope));
    }

    const std::array<std::string_view, 3> selected_types{
        "test.b", world::chunk_subscription_removal_payload_type, "test.a"};
    auto selected = client.drain_replication_messages_matching(
        std::span<const std::string_view>(selected_types), 2);
    assert(selected.size() == 2);
    assert(selected[0].message.payload_type == "test.a");
    assert(selected[1].message.payload_type == world::chunk_subscription_removal_payload_type);

    selected = client.drain_replication_messages_matching(
        std::span<const std::string_view>(selected_types));
    assert(selected.size() == 1);
    assert(selected.front().message.payload_type == "test.b");
    const auto remaining = client.drain_replication_messages();
    assert(remaining.size() == 1);
    assert(remaining.front().message.payload_type == "test.ignore");
}

void test_encoded_snapshot_message_round_trip() {
    world::ChunkDatabase chunks;
    auto& chunk = chunks.get_or_create({3, -1, 8});
    chunk.fill({7, 11, 13, 17});
    auto slices = world::make_chunk_snapshot_slices(chunk);
    assert(slices && !slices.value().empty());
    const auto& expected = slices.value().front();
    const auto payload = world::ChunkSnapshotSliceBinaryCodec::encode(expected);
    auto message = world::make_encoded_chunk_snapshot_slice_message(payload, 91, 123);
    assert(message.payload == payload);
    assert(message.sequence == 91);
    assert(message.timestamp_ms == 123);
    const net::TransportEnvelope envelope{core::NetId::from_value(1), core::NetId::from_value(2),
                                          std::move(message)};
    auto decoded = world::chunk_snapshot_slice_from_transport(envelope);
    assert(decoded);
    assert(decoded.value().identity == expected.identity);
    assert(decoded.value().content_revision == expected.content_revision);
    assert(decoded.value().slice_y == expected.slice_y);
    assert(decoded.value().cells == expected.cells);
}

void test_reliable_replication_batch_admission_is_atomic() {
    net::HostSessionConfig config;
    config.transport = {
        net::TransportBackend::in_memory,
        net::InMemoryTransportHostConfig{core::NetId::from_value(100), 4096},
    };
    config.max_pending_reliable_messages = 2;
    config.max_pending_reliable_messages_per_client = 2;
    net::HostSession host(config);
    assert(host.start());
    auto client_id = host.connect_client();
    assert(client_id);
    net::ClientSession client(client_id.value());
    accept_welcome(host, client_id.value(), client);

    const auto make_message = [](std::uint64_t sequence) {
        return net::TransportMessage{net::TransportMessageKind::replication,
                                     net::TransportChannel::reliable,
                                     sequence,
                                     "test.atomic_batch",
                                     std::to_string(sequence),
                                     0};
    };
    std::vector<net::TransportMessage> rejected;
    rejected.push_back(make_message(1));
    rejected.push_back(make_message(2));
    rejected.push_back(make_message(3));
    auto status = host.send_reliable_replication_batch(client_id.value(), std::move(rejected));
    assert(!status);
    assert(net::is_host_session_reliable_backlog_capacity_error(status.error().code));
    assert(host.pending_outbound_message_count() == 0);
    assert(host.pending_outbound_message_count(client_id.value()) == 0);

    std::vector<net::TransportMessage> admitted;
    admitted.push_back(make_message(4));
    admitted.push_back(make_message(5));
    assert(host.send_reliable_replication_batch(client_id.value(), std::move(admitted)));
    assert(host.pending_outbound_message_count() == 2);
    net::HostSessionTickResult tick;
    assert(host.flush_outbound(tick));
    auto delivered = host.drain_client_messages(client_id.value());
    assert(delivered && delivered.value().size() == 2);
    assert(delivered.value()[0].message.sequence == 4);
    assert(delivered.value()[1].message.sequence == 5);

    assert(net::is_host_session_reliable_backlog_capacity_error(
        "host_session.reliable_backlog_client_byte_limit"));
    assert(!net::is_host_session_reliable_backlog_capacity_error(
        "host_session.reliable_message_exceeds_delivery_budget"));
}

} // namespace

int main() {
    test_default_policy_converges_incrementally();
    test_hysteresis_and_teleport_transitions_stay_bounded();
    test_capacity_pressure_does_not_starve_current_interest();
    test_invalid_policies_and_inputs_are_rejected();
    test_extreme_centers_remain_unique_and_bounded();
    test_removal_protocol_round_trip_and_validation();
    test_multi_type_drain_preserves_replication_order();
    test_encoded_snapshot_message_round_trip();
    test_reliable_replication_batch_admission_is_atomic();
    return 0;
}
