#include "engine/net/client_session.hpp"
#include "engine/net/host_session.hpp"
#include "engine/net/replication.hpp"
#include "engine/net/server_command.hpp"
#include "engine/save/save_metadata.hpp"
#include "engine/world/replication_delta.hpp"
#include "engine/world/voxel_change.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace heartstead;

void accept_welcome(net::HostSession& host, core::NetId client_id, net::ClientSession& client) {
    auto messages = host.drain_client_messages(client_id);
    assert(messages);
    assert(messages.value().size() == 1);
    assert(client.receive_server_message(messages.value().front()));
}

net::ServerCommandDispatcher make_mutation_dispatcher() {
    net::ServerCommandDispatcher dispatcher;
    auto registered = dispatcher.register_command(net::CommandDescriptor{
        "test.mutate",
        true,
        true,
        [](const net::CommandEnvelope& envelope, const net::CommandExecutionContext&,
           world::WorldOperation& operation) {
            auto status = operation.record_mutation("test mutation " + envelope.payload);
            if (!status) {
                return status;
            }
            operation.emit_event({"test.changed", core::SaveId::from_value(10), envelope.payload});
            operation.mark_replication_dirty();
            operation.mark_save_dirty();
            return core::Status::ok();
        },
    });
    assert(registered);
    return dispatcher;
}

void test_two_clients_may_both_use_command_sequence_one() {
    net::HostSession host(net::HostSessionConfig{
        net::TransportHostDesc{
            net::TransportBackend::in_memory,
            net::InMemoryTransportHostConfig{core::NetId::from_value(50), 4096},
        },
        net::ReplicationRelevancePolicy{},
    });
    assert(host.start());

    auto first_id = host.connect_client();
    auto second_id = host.connect_client();
    assert(first_id);
    assert(second_id);
    net::ClientSession first(first_id.value());
    net::ClientSession second(second_id.value());
    accept_welcome(host, first_id.value(), first);
    accept_welcome(host, second_id.value(), second);

    auto first_command = first.create_command("test.mutate", "first");
    auto second_command = second.create_command("test.mutate", "second");
    assert(first_command);
    assert(second_command);
    assert(first_command.value().sequence == 1);
    assert(second_command.value().sequence == 1);
    assert(host.send_client_command(first_id.value(), first_command.value()));
    assert(host.send_client_command(second_id.value(), second_command.value()));

    auto dispatcher = make_mutation_dispatcher();
    auto tick = host.tick(dispatcher, net::CommandExecutionContext{});
    assert(tick);
    assert(tick.value().command_reports.size() == 2);
    assert(tick.value().command_reports[0].sequence == 1);
    assert(tick.value().command_reports[1].sequence == 1);
    assert(tick.value().command_reports[0].replication_sequence == 1);
    assert(tick.value().command_reports[1].replication_sequence == 2);

    for (auto* client : {&first, &second}) {
        const auto client_id = client->client_id();
        auto messages = host.drain_client_messages(client_id);
        assert(messages);
        assert(messages.value().size() == 3);
        for (const auto& message : messages.value()) {
            assert(client->receive_server_message(message));
        }

        auto results = client->drain_command_results();
        assert(results.size() == 1);
        assert(results.front().sequence == 1);

        auto batches = client->drain_replication_batches();
        assert(batches.size() == 2);
        assert(batches[0].command_sequence == 1);
        assert(batches[1].command_sequence == 1);
        assert(net::replication_stream_sequence(batches[0]) == 1);
        assert(net::replication_stream_sequence(batches[1]) == 2);
        assert(batches[0].source_client_id == first_id.value());
        assert(batches[1].source_client_id == second_id.value());
        assert(client->stats().last_replication_sequence == 2);
    }
}

void test_mixed_visibility_filters_events_ids_and_typed_state() {
    net::HostSession host(net::HostSessionConfig{
        net::TransportHostDesc{
            net::TransportBackend::in_memory,
            net::InMemoryTransportHostConfig{core::NetId::from_value(60), 8192},
        },
        net::ReplicationRelevancePolicy{},
    });
    assert(host.start());
    auto first_id = host.connect_client();
    auto second_id = host.connect_client();
    assert(first_id);
    assert(second_id);
    (void)host.drain_client_messages(first_id.value());
    (void)host.drain_client_messages(second_id.value());

    net::ReplicationRelevancePolicy policy;
    policy.broadcast_by_default = false;
    const std::vector public_subjects{core::SaveId::from_value(100), core::SaveId::from_value(101)};
    policy.client_rules.push_back({first_id.value(), public_subjects, false});
    policy.client_rules.push_back({second_id.value(), public_subjects, false});
    policy.private_access_rules.push_back({first_id.value(), {core::SaveId::from_value(100)}});
    policy.private_access_rules.push_back({second_id.value(), {core::SaveId::from_value(101)}});
    host.set_replication_relevance_policy(policy);

    net::ServerCommandDispatcher dispatcher;
    auto registered = dispatcher.register_command(net::CommandDescriptor{
        "test.private_mutation",
        true,
        true,
        [](const net::CommandEnvelope&, const net::CommandExecutionContext& context,
           world::WorldOperation& operation) {
            assert(context.save_ids != nullptr);
            auto first = operation.reserve_save_id(*context.save_ids);
            auto second = operation.reserve_save_id(*context.save_ids);
            assert(first);
            assert(second);
            auto status = operation.record_mutation("mixed visibility mutation");
            if (!status) {
                return status;
            }
            operation.emit_event({"inventory.changed", first.value(), "first private"});
            operation.emit_event({"inventory.changed", second.value(), "second private"});
            operation.emit_event({"test.global", {}, "hidden global"});
            operation.mark_replication_dirty();
            operation.mark_save_dirty();
            return core::Status::ok();
        },
    });
    assert(registered);

    save::SaveIdAllocator ids(100);
    net::CommandEnvelope command{1, first_id.value(), "test.private_mutation", {}, 0};
    assert(host.send_client_command(first_id.value(), command));
    net::CommandExecutionContext context;
    context.save_ids = &ids;
    auto tick = host.tick(dispatcher, context);
    assert(tick);
    assert(tick.value().command_reports.size() == 1);
    const auto& command_report = tick.value().command_reports.front();
    assert(command_report.reserved_ids.size() == 2);
    assert(command_report.replication_sequence == 1);

    auto first_messages = host.drain_client_messages(first_id.value());
    auto second_messages = host.drain_client_messages(second_id.value());
    assert(first_messages);
    assert(second_messages);
    assert(first_messages.value().size() == 2);
    assert(second_messages.value().size() == 1);

    auto first_batch = net::replication_batch_from_transport(first_messages.value().back());
    auto second_batch = net::replication_batch_from_transport(second_messages.value().front());
    assert(first_batch);
    assert(second_batch);
    assert(first_batch.value().events.size() == 1);
    assert(second_batch.value().events.size() == 1);
    assert(first_batch.value().events.front().subject == core::SaveId::from_value(100));
    assert(second_batch.value().events.front().subject == core::SaveId::from_value(101));
    assert(first_batch.value().reserved_ids ==
           std::vector<core::SaveId>{core::SaveId::from_value(100)});
    assert(second_batch.value().reserved_ids ==
           std::vector<core::SaveId>{core::SaveId::from_value(101)});

    world::WorldState state;
    assert(state.inventories().insert({core::SaveId::from_value(100), {}}));
    assert(state.inventories().insert({core::SaveId::from_value(101), {}}));

    const auto tick_deltas = world::materialize_replication_deltas_for_tick(state, tick.value());
    auto delivery =
        world::send_replication_delta_snapshots_for_tick(host, tick_deltas, tick.value(), 10);
    assert(delivery);
    assert(delivery.value().sent_message_count == 2);
    assert(host.flush_outbound(tick.value()));
    auto first_delta_messages = host.drain_client_messages(first_id.value());
    auto second_delta_messages = host.drain_client_messages(second_id.value());
    assert(first_delta_messages);
    assert(second_delta_messages);
    assert(first_delta_messages.value().size() == 1);
    assert(second_delta_messages.value().size() == 1);
    auto delivered_first_snapshot =
        world::replication_delta_snapshot_from_transport(first_delta_messages.value().front());
    auto delivered_second_snapshot =
        world::replication_delta_snapshot_from_transport(second_delta_messages.value().front());
    assert(delivered_first_snapshot);
    assert(delivered_second_snapshot);
    assert(delivered_first_snapshot.value().inventories.size() == 1);
    assert(delivered_second_snapshot.value().inventories.size() == 1);
    assert(delivered_first_snapshot.value().inventories.front().owner_id ==
           core::SaveId::from_value(100));
    assert(delivered_second_snapshot.value().inventories.front().owner_id ==
           core::SaveId::from_value(101));

    net::ReplicationBatch complete_batch;
    complete_batch.command_sequence = command_report.sequence;
    complete_batch.command_type = command_report.command_type;
    complete_batch.events = command_report.events;
    complete_batch.reserved_ids = command_report.reserved_ids;
    complete_batch.replication_sequence = command_report.replication_sequence;
    complete_batch.source_client_id = command_report.client_id;
    const auto complete_snapshot = world::materialize_replication_delta(state, complete_batch);
    assert(complete_snapshot.inventories.size() == 2);
    assert(complete_snapshot.plan.global_event_count == 1);

    auto first_snapshot =
        world::filter_replication_delta_snapshot(complete_snapshot, policy, first_id.value());
    auto second_snapshot =
        world::filter_replication_delta_snapshot(complete_snapshot, policy, second_id.value());
    assert(first_snapshot);
    assert(second_snapshot);
    assert(first_snapshot.value().plan.event_count == 1);
    assert(second_snapshot.value().plan.event_count == 1);
    assert(first_snapshot.value().plan.global_event_count == 0);
    assert(second_snapshot.value().plan.global_event_count == 0);
    assert(first_snapshot.value().inventories.size() == 1);
    assert(second_snapshot.value().inventories.size() == 1);
    assert(first_snapshot.value().inventories.front().owner_id == core::SaveId::from_value(100));
    assert(second_snapshot.value().inventories.front().owner_id == core::SaveId::from_value(101));

    net::ReplicationRelevancePolicy no_private_access;
    no_private_access.broadcast_by_default = true;
    const auto hidden_batch = net::ReplicationRelevance::filter_for_client(
        no_private_access, complete_batch, first_id.value());
    assert(hidden_batch.events.size() == 1);
    assert(hidden_batch.events.front().type == "test.global");
    assert(hidden_batch.reserved_ids.empty());
    auto hidden_snapshot = world::filter_replication_delta_snapshot(
        complete_snapshot, no_private_access, first_id.value());
    assert(hidden_snapshot);
    assert(hidden_snapshot.value().inventories.empty());
    assert(hidden_snapshot.value().plan.subjects.empty());
    assert(hidden_snapshot.value().plan.subject_event_count == 0);
    assert(hidden_snapshot.value().plan.global_event_count == 1);

    auto unsafe_snapshot = complete_snapshot;
    unsafe_snapshot.inventories.front().owner_id = core::SaveId::from_value(999);
    auto rejected_filter =
        world::filter_replication_delta_snapshot(unsafe_snapshot, policy, first_id.value());
    assert(!rejected_filter);
    assert(rejected_filter.error().code == "replication_delta.recipient_filter_requires_resync");
}

void test_voxel_event_applies_before_observation_without_separate_delta() {
    net::HostSession host(net::HostSessionConfig{
        net::TransportHostDesc{
            net::TransportBackend::in_memory,
            net::InMemoryTransportHostConfig{core::NetId::from_value(70), 4096},
        },
        net::ReplicationRelevancePolicy{},
    });
    assert(host.start());
    auto client_id = host.connect_client();
    assert(client_id);
    net::ClientSession client(client_id.value());
    accept_welcome(host, client_id.value(), client);

    world::WorldState state;
    auto& chunk = state.chunks().get_or_create({0, 0, 0});
    const world::VoxelChangeRecord change{
        {2, 3, 4}, world::VoxelCell::air(), {1, 0}, chunk.identity(), chunk.content_revision() + 1,
    };
    net::ReplicationBatch batch;
    batch.command_sequence = 1;
    batch.replication_sequence = 1;
    batch.source_client_id = client_id.value();
    batch.command_type = "voxel.place";
    batch.events.push_back({std::string(world::voxel_changed_event_type),
                            {},
                            world::VoxelChangeTextCodec::encode(change)});
    assert(host.send_replication_message(client_id.value(),
                                         net::make_replication_transport_message(batch, 10)));
    net::HostSessionTickResult delivery_tick;
    assert(host.flush_outbound(delivery_tick));
    auto messages = host.drain_client_messages(client_id.value());
    assert(messages && messages.value().size() == 1);
    assert(client.receive_server_message(messages.value().front()));

    auto applied = world::apply_client_queued_replication_deltas(state, client);
    assert(applied);
    assert(applied.value().applied_delta_count == 1);
    assert(applied.value().observed_events.size() == 1);
    assert(applied.value().batches.size() == 1);
    assert(applied.value().batches.front().state == "applied_event_delta");
    const auto address = world::block_to_chunk_local(change.position);
    const auto current = state.chunks().get(address.chunk, address.local);
    assert(current && current.value() == change.current);
}

void test_typed_delta_rejects_mismatched_global_event_payload() {
    net::HostSession host(net::HostSessionConfig{
        net::TransportHostDesc{
            net::TransportBackend::in_memory,
            net::InMemoryTransportHostConfig{core::NetId::from_value(75), 4096},
        },
        net::ReplicationRelevancePolicy{},
    });
    assert(host.start());
    auto client_id = host.connect_client();
    assert(client_id);
    net::ClientSession client(client_id.value());
    accept_welcome(host, client_id.value(), client);

    net::ReplicationBatch observed;
    observed.command_sequence = 1;
    observed.replication_sequence = 1;
    observed.source_client_id = client_id.value();
    observed.command_type = "test.global_change";
    observed.events.push_back({"test.global", {}, "observed"});
    assert(host.send_replication_message(client_id.value(),
                                         net::make_replication_transport_message(observed, 10)));
    net::HostSessionTickResult delivery_tick;
    assert(host.flush_outbound(delivery_tick));
    auto messages = host.drain_client_messages(client_id.value());
    assert(messages && messages.value().size() == 1);
    assert(client.receive_server_message(messages.value().front()));

    auto mismatched = observed;
    mismatched.events.front().message = "different";
    world::WorldState state;
    const auto snapshot = world::materialize_replication_delta(state, mismatched);
    const std::span<const world::WorldReplicationDeltaSnapshot> snapshots(&snapshot, 1);
    auto applied = world::apply_client_replication_deltas(state, client, snapshots);
    assert(!applied);
    assert(applied.error().code == "world_client_replication.global_event_mismatch");
}

void test_replication_message_drain_preserves_a_bounded_backlog() {
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

    for (std::uint64_t sequence = 1; sequence <= 5; ++sequence) {
        net::TransportMessage message;
        message.kind = net::TransportMessageKind::replication;
        message.channel = net::TransportChannel::reliable;
        message.sequence = sequence;
        message.payload_type = sequence % 2 == 0 ? "test.even" : "test.odd";
        message.payload = std::to_string(sequence);
        assert(host.send_replication_message(client_id.value(), std::move(message)));
    }
    net::HostSessionTickResult delivery_tick;
    assert(host.flush_outbound(delivery_tick));
    auto delivered = host.drain_client_messages(client_id.value());
    assert(delivered && delivered.value().size() == 5);
    for (const auto& envelope : delivered.value()) {
        assert(client.receive_server_message(envelope));
    }

    const auto first_odds = client.drain_replication_messages("test.odd", 2);
    assert(first_odds.size() == 2);
    assert(first_odds[0].message.payload == "1");
    assert(first_odds[1].message.payload == "3");
    assert(client.stats().queued_replication_message_count == 3);
    const auto next = client.drain_replication_messages({}, 1);
    assert(next.size() == 1 && next.front().message.payload == "2");
    const auto remaining = client.drain_replication_messages();
    assert(remaining.size() == 2);
    assert(remaining[0].message.payload == "4");
    assert(remaining[1].message.payload == "5");
}

} // namespace

int main() {
    test_two_clients_may_both_use_command_sequence_one();
    test_mixed_visibility_filters_events_ids_and_typed_state();
    test_voxel_event_applies_before_observation_without_separate_delta();
    test_typed_delta_rejects_mismatched_global_event_payload();
    test_replication_message_drain_preserves_a_bounded_backlog();
    return 0;
}
