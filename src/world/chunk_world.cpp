#include "heartstead/world/chunk_world.hpp"

#include <utility>

namespace heartstead {

Chunk& ChunkWorld::ensure_chunk(Int3 coordinates, BlockId fill) {
    return chunks_.try_emplace(coordinates, fill).first->second;
}

void ChunkWorld::set_chunk(Int3 coordinates, Chunk chunk) {
    chunks_.insert_or_assign(coordinates, std::move(chunk));
}

bool ChunkWorld::erase_chunk(Int3 coordinates) noexcept {
    return chunks_.erase(coordinates) != 0U;
}

std::optional<Chunk> ChunkWorld::take_chunk(Int3 coordinates) {
    auto node = chunks_.extract(coordinates);
    if (node.empty()) return std::nullopt;
    return std::move(node.mapped());
}

Chunk* ChunkWorld::find_chunk(Int3 coordinates) noexcept {
    const auto found = chunks_.find(coordinates);
    return found == chunks_.end() ? nullptr : &found->second;
}

const Chunk* ChunkWorld::find_chunk(Int3 coordinates) const noexcept {
    const auto found = chunks_.find(coordinates);
    return found == chunks_.end() ? nullptr : &found->second;
}

BlockId ChunkWorld::get_block(Int3 world_position) const noexcept {
    const Int3 chunk_position{
        floor_div(world_position.x, Chunk::edge),
        floor_div(world_position.y, Chunk::edge),
        floor_div(world_position.z, Chunk::edge),
    };
    const auto* chunk = find_chunk(chunk_position);
    if (chunk == nullptr) {
        return air_block;
    }
    return chunk->get(
        floor_mod(world_position.x, Chunk::edge),
        floor_mod(world_position.y, Chunk::edge),
        floor_mod(world_position.z, Chunk::edge));
}

void ChunkWorld::set_block(Int3 world_position, BlockId block) {
    const Int3 chunk_position{
        floor_div(world_position.x, Chunk::edge),
        floor_div(world_position.y, Chunk::edge),
        floor_div(world_position.z, Chunk::edge),
    };
    ensure_chunk(chunk_position).set(
        floor_mod(world_position.x, Chunk::edge),
        floor_mod(world_position.y, Chunk::edge),
        floor_mod(world_position.z, Chunk::edge),
        block);
}

std::size_t ChunkWorld::memory_bytes() const noexcept {
    auto bytes = sizeof(ChunkWorld);
    for (const auto& [coordinates, chunk] : chunks_) {
        static_cast<void>(coordinates);
        bytes += sizeof(Int3) + chunk.memory_bytes();
    }
    return bytes;
}

} // namespace heartstead
