#include "heartstead/voxel/greedy_mesher.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint32_t random_step(std::uint32_t& state) noexcept {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

heartstead::Chunk make_chunk() {
    using namespace heartstead;
    Chunk chunk;
    std::uint32_t random = 0x12345678U;
    for (std::int32_t z = 0; z < Chunk::edge; ++z) {
        for (std::int32_t x = 0; x < Chunk::edge; ++x) {
            const auto height = 8 + static_cast<std::int32_t>(random_step(random) % 16U);
            for (std::int32_t y = 0; y <= height; ++y) {
                chunk.set(x, y, z, y == height ? 3 : (y + 3 > height ? 2 : 1));
            }
        }
    }
    return chunk;
}

} // namespace


int main() {
    using namespace heartstead;
    constexpr std::size_t warmup_iterations = 20;
    constexpr std::size_t measured_iterations = 250;
    const auto blocks = BlockRegistry::defaults();
    auto chunk = make_chunk();

    std::uint64_t guard = 0;
    for (std::size_t index = 0; index < warmup_iterations; ++index) {
        guard += GreedyMesher::build(chunk, blocks).indices.size();
    }

    const auto start = Clock::now();
    for (std::size_t index = 0; index < measured_iterations; ++index) {
        guard += GreedyMesher::build(chunk, blocks).indices.size();
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const auto mesh = GreedyMesher::build(chunk, blocks);
    const auto milliseconds_per_chunk = elapsed / static_cast<double>(measured_iterations);
    const auto chunks_per_second = 1000.0 / milliseconds_per_chunk;

    std::cout << std::fixed << std::setprecision(3)
              << "storage_bytes=" << chunk.memory_bytes() << '\n'
              << "mesh_quads=" << mesh.quad_count << '\n'
              << "mesh_bytes=" << mesh.memory_bytes() << '\n'
              << "milliseconds_per_chunk=" << milliseconds_per_chunk << '\n'
              << "chunks_per_second=" << chunks_per_second << '\n'
              << "guard=" << guard << '\n';
}

