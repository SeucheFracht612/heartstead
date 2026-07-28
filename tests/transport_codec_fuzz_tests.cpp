#include "engine/movement/movement_prediction.hpp"
#include "engine/net/transport_handshake.hpp"
#include "engine/net/transport_packet.hpp"
#include "engine/world/chunks/chunk_replication.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace heartstead;

void exercise(std::string_view bytes) {
    if (auto packet =
            net::TransportPacketCodec::decode(bytes, {.max_payload_bytes = 4096});
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
    if (auto snapshot =
            movement::PlayerControllerSnapshotBinaryCodec::decode(bytes);
        snapshot) {
        assert(snapshot.value().validate({}));
    }
    if (auto chunk = world::ChunkSnapshotSliceBinaryCodec::decode(bytes); chunk) {
        assert(chunk.value().validate());
    }
}

void exercise_seed(const std::string& seed) {
    exercise(seed);
    for (std::size_t size = 0; size < seed.size(); ++size) {
        exercise(std::string_view(seed).substr(0, size));
    }
    for (std::size_t index = 0; index < seed.size(); ++index) {
        auto mutated = seed;
        mutated[index] = static_cast<char>(
            static_cast<unsigned char>(mutated[index]) ^ 0xa5U);
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
        {client, server,
         {net::TransportMessageKind::control, net::TransportChannel::unreliable,
          7, "test.input", "payload", 42},
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
    slice.cells.resize(
        static_cast<std::size_t>(world::VoxelChunk::edge_length) *
            world::VoxelChunk::edge_length,
        {1, 255, 3, 0});
    seeds.push_back(world::ChunkSnapshotSliceBinaryCodec::encode(slice));
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
