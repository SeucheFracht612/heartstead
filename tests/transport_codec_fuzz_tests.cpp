#include "engine/movement/movement_prediction.hpp"
#include "engine/net/command_payload.hpp"
#include "engine/net/host_session.hpp"
#include "engine/net/replication.hpp"
#include "engine/net/transport_handshake.hpp"
#include "engine/net/transport_packet.hpp"
#include "engine/world/chunks/chunk_replication.hpp"
#include "engine/world/replication_delta.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace heartstead;

void exercise(std::string_view bytes) {
    if (auto packet = net::TransportPacketCodec::decode(bytes, {.max_payload_bytes = 4096});
        packet) {
        assert(net::validate_transport_message(packet.value().message, 4096));
    }
    if (auto fragment = net::TransportPacketFragmentCodec::decode(bytes); fragment) {
        assert(net::TransportPacketFragmentCodec::validate_fragment(fragment.value()));
    }
    if (auto handshake = net::TransportHandshakeCodec::decode(bytes); handshake) {
        assert(net::TransportHandshakeCodec::decode(
            net::TransportHandshakeCodec::encode(handshake.value()).value()));
    }
    if (auto input = movement::PlayerInputBundleBinaryCodec::decode(bytes); input) {
        assert(input.value().validate());
    }
    if (auto snapshot = movement::PlayerControllerSnapshotBinaryCodec::decode(bytes); snapshot) {
        assert(snapshot.value().validate({}));
    }
    if (auto chunk = world::ChunkSnapshotSliceBinaryCodec::decode(bytes); chunk) {
        assert(chunk.value().validate());
    }
    if (auto command = net::CommandPayloadBinaryCodec::decode(bytes); command) {
        assert(!command.value().fields().empty());
    }
    if (auto result = net::HostSessionCommandResultBinaryCodec::decode(bytes); result) {
        assert(net::validate_host_session_command_result(result.value()));
    }
    if (auto events = net::ReplicationBinaryCodec::decode(bytes); events) {
        assert(net::ReplicationBinaryCodec::decode(
            net::ReplicationBinaryCodec::encode(events.value())));
    }
    if (auto delta = world::WorldReplicationDeltaSnapshotBinaryCodec::decode(bytes); delta) {
        assert(world::WorldReplicationDeltaSnapshotBinaryCodec::encode(delta.value()));
    }
}

void exercise_seed(const std::string& seed) {
    exercise(seed);
    for (std::size_t size = 0; size < seed.size(); ++size) {
        exercise(std::string_view(seed).substr(0, size));
    }
    for (std::size_t index = 0; index < seed.size(); ++index) {
        auto mutated = seed;
        mutated[index] = static_cast<char>(static_cast<unsigned char>(mutated[index]) ^ 0xa5U);
        exercise(mutated);
    }
}

} // namespace

int main() {
    using namespace heartstead;
    const auto client = core::NetId::from_value(2);
    const auto server = core::NetId::from_value(1);
    std::vector<std::string> seeds;
    seeds.push_back(net::TransportPacketCodec::encode(
        {client,
         server,
         {net::TransportMessageKind::control, net::TransportChannel::unreliable, 7, "test.input",
          "payload", 42},
         {11, 12}}));
    const net::TransportHandshakePacket hello{
        net::TransportHandshakeKind::client_hello,
        net::transport_handshake_protocol_version,
        {1, 2},
        {},
        0,
        {},
        {},
        {},
        "content-test",
        {},
    };
    seeds.push_back(net::TransportHandshakeCodec::encode(hello).value());
    movement::PlayerInputBundle inputs;
    inputs.frames.push_back(
        {movement::player_input_version, 1, 1, 32'767, -12'000, 900, -400, 3, 1});
    seeds.push_back(movement::PlayerInputBundleBinaryCodec::encode(inputs));
    world::ChunkSnapshotSlice slice;
    slice.identity = {{-2, 3, 4}, 1};
    slice.content_revision = 2;
    slice.slice_y = 7;
    slice.cells.resize(static_cast<std::size_t>(world::VoxelChunk::edge_length) *
                           world::VoxelChunk::edge_length,
                       {1, 255, 3, 0});
    seeds.push_back(world::ChunkSnapshotSliceBinaryCodec::encode(slice));
    net::CommandPayload command_payload;
    assert(command_payload.set("target", "base:debug"));
    assert(command_payload.set("count", "7"));
    seeds.push_back(net::CommandPayloadBinaryCodec::encode(command_payload));
    net::HostSessionCommandResult command_result;
    command_result.sequence = 8;
    command_result.command_type = "debug.ping";
    command_result.success = true;
    seeds.push_back(net::HostSessionCommandResultBinaryCodec::encode(command_result));
    net::ReplicationBatch event_batch;
    event_batch.command_sequence = 9;
    event_batch.replication_sequence = 10;
    event_batch.source_client_id = client;
    event_batch.command_type = "debug.change";
    event_batch.events.push_back({"debug.changed", core::SaveId::from_value(3), "changed"});
    seeds.push_back(net::ReplicationBinaryCodec::encode(event_batch));
    world::WorldReplicationDeltaSnapshot delta;
    delta.plan.command_sequence = 11;
    delta.plan.replication_sequence = 12;
    delta.plan.source_client_id = client;
    delta.plan.command_type = "debug.empty_delta";
    auto encoded_delta = world::WorldReplicationDeltaSnapshotBinaryCodec::encode(delta);
    assert(encoded_delta);
    seeds.push_back(std::move(encoded_delta).value());
    for (const auto& seed : seeds) {
        exercise_seed(seed);
    }

    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    for (std::uint32_t iteration = 0; iteration < 25'000; ++iteration) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        const auto size = static_cast<std::size_t>(state % 2049U);
        std::string bytes(size, '\0');
        for (auto& byte : bytes) {
            state ^= state << 13U;
            state ^= state >> 7U;
            state ^= state << 17U;
            byte = static_cast<char>(state & 0xffU);
        }
        exercise(bytes);
    }
    return 0;
}
