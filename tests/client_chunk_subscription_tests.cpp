#include "engine/net/host_session.hpp"
#include "engine/world/chunks/chunk_replication.hpp"
#include "engine/world/world_state.hpp"
#include "game/runtime/client_runtime.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace heartstead;

void deliver_pending(net::HostSession& host, core::NetId client_id, game::ClientRuntime& client) {
    net::HostSessionTickResult tick;
    assert(host.flush_outbound(tick));
    auto messages = host.drain_client_messages(client_id);
    assert(messages);
    assert(client.receive(messages.value()));
}

net::HostSession make_host(core::NetId server_id) {
    return net::HostSession(net::HostSessionConfig{
        net::TransportHostDesc{
            net::TransportBackend::in_memory,
            net::InMemoryTransportHostConfig{server_id, 4096},
        },
        net::ReplicationRelevancePolicy{},
    });
}

world::VoxelChangeRecord make_voxel_change(world::BlockCoord position, world::VoxelCell previous,
                                           world::VoxelCell current,
                                           const world::VoxelChunk& chunk) {
    world::VoxelChangeRecord change;
    change.position = position;
    change.previous = previous;
    change.current = current;
    change.chunk_identity = chunk.identity();
    change.content_revision = chunk.content_revision();
    assert(change.validate());
    return change;
}

void queue_voxel_delta(net::HostSession& host, core::NetId client_id,
                       const world::WorldState& materialization_world,
                       const world::VoxelChangeRecord& change, std::uint64_t replication_sequence,
                       std::int64_t timestamp_ms) {
    net::ReplicationBatch batch;
    batch.command_sequence = replication_sequence;
    batch.replication_sequence = replication_sequence;
    batch.source_client_id = client_id;
    batch.command_type = "world.set_voxel";
    batch.events.emplace_back(std::string(world::voxel_changed_event_type), core::SaveId{},
                              world::VoxelChangeTextCodec::encode(change));
    assert(host.send_replication_message(
        client_id, net::make_replication_transport_message(batch, timestamp_ms)));
    auto delta = world::materialize_replication_delta(materialization_world, batch);
    auto delta_message = world::make_replication_delta_transport_message(delta, timestamp_ms);
    assert(delta_message);
    assert(host.send_replication_message(client_id, std::move(delta_message).value()));
}

void queue_chunk_snapshot(net::HostSession& host, core::NetId client_id,
                          const world::VoxelChunk& chunk, std::uint64_t& transport_sequence,
                          std::int64_t timestamp_ms) {
    auto slices = world::make_chunk_snapshot_slices(chunk);
    assert(slices);
    for (const auto& slice : slices.value()) {
        assert(host.send_replication_message(
            client_id,
            world::make_chunk_snapshot_slice_message(slice, transport_sequence++, timestamp_ms)));
    }
}

void test_ordered_removal_discards_resident_and_partial_chunk_state() {
    auto host = make_host(core::NetId::from_value(90));
    assert(host.start());
    auto client_id = host.connect_client();
    assert(client_id);
    game::ClientRuntime client(client_id.value(), world::WorldStateDesc{});
    deliver_pending(host, client_id.value(), client);
    assert(client.is_connected());

    constexpr world::ChunkCoord coordinate{4, -2, 9};
    world::WorldState authoritative_world;
    auto& authoritative_chunk = authoritative_world.chunks().get_or_create(coordinate);
    authoritative_chunk.fill({1, 0, 0, 0});
    assert(client.install_local_chunk_snapshot(authoritative_chunk));
    assert(client.world().chunks().contains(coordinate));

    authoritative_chunk.fill({2, 0, 0, 0});
    auto slices = world::make_chunk_snapshot_slices(authoritative_chunk);
    assert(slices);
    assert(slices.value().size() == world::VoxelChunk::edge_length);

    assert(host.send_replication_message(
        client_id.value(), world::make_chunk_snapshot_slice_message(slices.value().front(), 1, 1)));
    deliver_pending(host, client_id.value(), client);
    auto partial = client.synchronize(1, 1);
    assert(partial);
    assert(partial.value().chunk_snapshot_slice_count == 1);
    assert(partial.value().completed_chunk_snapshot_count == 0);
    assert(partial.value().chunk_subscription_removal_count == 0);
    assert(client.world().chunks().contains(coordinate));

    assert(host.send_replication_message(
        client_id.value(), world::make_chunk_subscription_removal_message({coordinate}, 2, 2)));
    deliver_pending(host, client_id.value(), client);
    auto removed = client.synchronize(2, 1);
    assert(removed);
    assert(removed.value().chunk_snapshot_slice_count == 0);
    assert(removed.value().completed_chunk_snapshot_count == 0);
    assert(removed.value().chunk_subscription_removal_count == 1);
    assert(!client.world().chunks().contains(coordinate));

    std::uint64_t sequence = 3;
    for (const auto& slice : slices.value()) {
        assert(host.send_replication_message(
            client_id.value(), world::make_chunk_snapshot_slice_message(slice, sequence++, 3)));
    }
    deliver_pending(host, client_id.value(), client);
    auto restored = client.synchronize(3, world::VoxelChunk::edge_length);
    assert(restored);
    assert(restored.value().chunk_snapshot_slice_count == world::VoxelChunk::edge_length);
    assert(restored.value().completed_chunk_snapshot_count == 1);
    assert(restored.value().chunk_subscription_removal_count == 0);
    const auto* resident = client.world().chunks().find(coordinate);
    assert(resident != nullptr);
    const auto cell = resident->get({0, 0, 0});
    assert(cell);
    assert(cell.value().type == 2);

    for (const auto& slice : slices.value()) {
        assert(host.send_replication_message(
            client_id.value(), world::make_chunk_snapshot_slice_message(slice, sequence++, 4)));
    }
    assert(host.send_replication_message(
        client_id.value(),
        world::make_chunk_subscription_removal_message({coordinate}, sequence, 4)));
    deliver_pending(host, client_id.value(), client);
    auto ordered = client.synchronize(4, world::VoxelChunk::edge_length + 1);
    assert(ordered);
    assert(ordered.value().chunk_snapshot_slice_count == world::VoxelChunk::edge_length);
    assert(ordered.value().completed_chunk_snapshot_count == 0);
    assert(ordered.value().chunk_subscription_removal_count == 1);
    assert(!client.world().chunks().contains(coordinate));
}

void test_snapshot_base_and_contiguous_delta_apply_in_one_sync() {
    auto host = make_host(core::NetId::from_value(91));
    assert(host.start());
    auto client_id = host.connect_client();
    assert(client_id);
    game::ClientRuntime client(client_id.value(), world::WorldStateDesc{});
    deliver_pending(host, client_id.value(), client);

    constexpr world::ChunkCoord coordinate{2, 0, -3};
    constexpr world::VoxelCoord local{4, 5, 6};
    const auto resolved_position = world::chunk_local_to_block(coordinate, local);
    assert(resolved_position);
    const auto position = resolved_position.value();
    world::WorldState authoritative;
    auto& chunk = authoritative.chunks().get_or_create(coordinate);
    chunk.fill({1, 0, 0, 0});
    const world::VoxelChunk base_snapshot = chunk;
    const auto previous = chunk.get(local);
    assert(previous);
    const world::VoxelCell current{2, 0, 0, 0};
    assert(chunk.set(local, current));
    const auto change = make_voxel_change(position, previous.value(), current, chunk);

    std::uint64_t transport_sequence = 100;
    queue_chunk_snapshot(host, client_id.value(), base_snapshot, transport_sequence, 1);
    queue_voxel_delta(host, client_id.value(), authoritative, change, 200, 1);
    deliver_pending(host, client_id.value(), client);
    auto synchronized = client.synchronize(1, std::numeric_limits<std::size_t>::max());
    assert(synchronized);
    assert(synchronized.value().completed_chunk_snapshot_count == 1);
    assert(synchronized.value().replication.batches.size() == 1);
    assert(
        synchronized.value().replication.batches.front().delta_apply_report.voxel_edits_applied ==
        1);
    assert(synchronized.value()
               .replication.batches.front()
               .delta_apply_report.voxel_edits_superseded == 0);
    const auto applied = client.world().chunks().get(coordinate, local);
    assert(applied && applied.value() == current);
    assert(client.accepted_voxel_edits().size() == 1);
    assert(client.accepted_voxel_edits().front().content_revision == change.content_revision);
}

void test_newer_snapshot_supersedes_older_queued_delta_without_regression() {
    auto host = make_host(core::NetId::from_value(92));
    assert(host.start());
    auto client_id = host.connect_client();
    assert(client_id);
    game::ClientRuntime client(client_id.value(), world::WorldStateDesc{});
    deliver_pending(host, client_id.value(), client);

    constexpr world::ChunkCoord coordinate{-5, 1, 7};
    constexpr world::VoxelCoord local{7, 8, 9};
    const auto resolved_position = world::chunk_local_to_block(coordinate, local);
    assert(resolved_position);
    const auto position = resolved_position.value();
    world::WorldState authoritative;
    auto& chunk = authoritative.chunks().get_or_create(coordinate);
    chunk.fill({1, 0, 0, 0});
    assert(client.install_local_chunk_snapshot(chunk));
    const auto base = chunk.get(local);
    assert(base);

    const world::VoxelCell intermediate{2, 0, 0, 0};
    assert(chunk.set(local, intermediate));
    const auto older_change = make_voxel_change(position, base.value(), intermediate, chunk);
    const world::VoxelCell newest{3, 0, 0, 0};
    assert(chunk.set(local, newest));

    queue_voxel_delta(host, client_id.value(), authoritative, older_change, 10, 2);
    std::uint64_t transport_sequence = 300;
    queue_chunk_snapshot(host, client_id.value(), chunk, transport_sequence, 2);
    deliver_pending(host, client_id.value(), client);
    auto synchronized = client.synchronize(2, std::numeric_limits<std::size_t>::max());
    assert(synchronized);
    assert(synchronized.value().completed_chunk_snapshot_count == 1);
    assert(synchronized.value().replication.batches.size() == 1);
    const auto& apply = synchronized.value().replication.batches.front().delta_apply_report;
    assert(apply.voxel_edits_applied == 0);
    assert(apply.voxel_edits_superseded == 1);
    const auto retained = client.world().chunks().get(coordinate, local);
    assert(retained && retained.value() == newest);
    assert(client.accepted_voxel_edits().size() == 1);
    assert(client.accepted_voxel_edits().front().current == intermediate);
}

void test_multiple_same_cell_deltas_advance_one_revision_at_a_time() {
    auto host = make_host(core::NetId::from_value(93));
    assert(host.start());
    auto client_id = host.connect_client();
    assert(client_id);
    game::ClientRuntime client(client_id.value(), world::WorldStateDesc{});
    deliver_pending(host, client_id.value(), client);

    constexpr world::ChunkCoord coordinate{6, -1, 4};
    constexpr world::VoxelCoord local{10, 11, 12};
    const auto resolved_position = world::chunk_local_to_block(coordinate, local);
    assert(resolved_position);
    const auto position = resolved_position.value();
    world::WorldState authoritative;
    auto& chunk = authoritative.chunks().get_or_create(coordinate);
    chunk.fill({1, 0, 0, 0});
    assert(client.install_local_chunk_snapshot(chunk));
    const auto base = chunk.get(local);
    assert(base);

    const world::VoxelCell second{2, 0, 0, 0};
    assert(chunk.set(local, second));
    const auto first_change = make_voxel_change(position, base.value(), second, chunk);
    const world::VoxelCell third{3, 0, 0, 0};
    assert(chunk.set(local, third));
    const auto second_change = make_voxel_change(position, second, third, chunk);

    queue_voxel_delta(host, client_id.value(), authoritative, first_change, 20, 3);
    queue_voxel_delta(host, client_id.value(), authoritative, second_change, 21, 3);
    deliver_pending(host, client_id.value(), client);
    auto synchronized = client.synchronize(3);
    assert(synchronized);
    assert(synchronized.value().replication.batches.size() == 2);
    assert(synchronized.value().replication.batches[0].delta_apply_report.voxel_edits_applied == 1);
    assert(synchronized.value().replication.batches[1].delta_apply_report.voxel_edits_applied == 1);
    const auto applied = client.world().chunks().get(coordinate, local);
    assert(applied && applied.value() == third);
    assert(client.accepted_voxel_edits().size() == 2);
    assert(client.accepted_voxel_edits()[0].content_revision == first_change.content_revision);
    assert(client.accepted_voxel_edits()[1].content_revision == second_change.content_revision);
}

void test_revision_gap_fails_without_advancing_client_cursor() {
    auto host = make_host(core::NetId::from_value(94));
    assert(host.start());
    auto client_id = host.connect_client();
    assert(client_id);
    game::ClientRuntime client(client_id.value(), world::WorldStateDesc{});
    deliver_pending(host, client_id.value(), client);

    constexpr world::ChunkCoord coordinate{-8, 2, -6};
    constexpr world::VoxelCoord local{13, 14, 15};
    const auto resolved_position = world::chunk_local_to_block(coordinate, local);
    assert(resolved_position);
    const auto position = resolved_position.value();
    world::WorldState authoritative;
    auto& chunk = authoritative.chunks().get_or_create(coordinate);
    chunk.fill({1, 0, 0, 0});
    assert(client.install_local_chunk_snapshot(chunk));
    const auto base_revision = chunk.content_revision();
    const auto base = chunk.get(local);
    assert(base);

    world::VoxelChangeRecord gap_change{position, base.value(), world::VoxelCell{3, 0, 0, 0},
                                        chunk.identity(), base_revision + 2};
    assert(gap_change.validate());
    queue_voxel_delta(host, client_id.value(), authoritative, gap_change, 30, 4);
    deliver_pending(host, client_id.value(), client);
    auto rejected = client.synchronize(4);
    assert(!rejected);
    assert(rejected.error().code == "client_runtime.accepted_voxel_base_gap");
    const auto unchanged = client.world().chunks().get(coordinate, local);
    assert(unchanged && unchanged.value() == base.value());

    world::VoxelChangeRecord contiguous_change{position, base.value(), world::VoxelCell{2, 0, 0, 0},
                                               chunk.identity(), base_revision + 1};
    assert(contiguous_change.validate());
    queue_voxel_delta(host, client_id.value(), authoritative, contiguous_change, 31, 5);
    deliver_pending(host, client_id.value(), client);
    auto recovered = client.synchronize(5);
    assert(recovered);
    const auto applied = client.world().chunks().get(coordinate, local);
    assert(applied && applied.value() == contiguous_change.current);
    assert(client.accepted_voxel_edits().size() == 1);
}

} // namespace

int main() {
    test_ordered_removal_discards_resident_and_partial_chunk_state();
    test_snapshot_base_and_contiguous_delta_apply_in_one_sync();
    test_newer_snapshot_supersedes_older_queued_delta_without_regression();
    test_multiple_same_cell_deltas_advance_one_revision_at_a_time();
    test_revision_gap_fails_without_advancing_client_cursor();
    return 0;
}
