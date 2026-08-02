#include "engine/net/host_session.hpp"
#include "engine/world/chunks/chunk_replication.hpp"
#include "engine/world/world_state.hpp"
#include "game/runtime/client_runtime.hpp"

#include <cassert>
#include <cstdint>
#include <span>
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

void test_ordered_removal_discards_resident_and_partial_chunk_state() {
    net::HostSession host(net::HostSessionConfig{
        net::TransportHostDesc{
            net::TransportBackend::in_memory,
            net::InMemoryTransportHostConfig{core::NetId::from_value(90), 4096},
        },
        net::ReplicationRelevancePolicy{},
    });
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

} // namespace

int main() {
    test_ordered_removal_discards_resident_and_partial_chunk_state();
    return 0;
}
