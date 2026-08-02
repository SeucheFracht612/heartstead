#pragma once

#include "heartstead/core/types.hpp"
#include "heartstead/voxel/chunk.hpp"

#include <cstddef>
#include <optional>
#include <unordered_map>

namespace heartstead {

class ChunkWorld {
public:
    Chunk& ensure_chunk(Int3 coordinates, BlockId fill = air_block);
    void set_chunk(Int3 coordinates, Chunk chunk);
    bool erase_chunk(Int3 coordinates) noexcept;
    [[nodiscard]] std::optional<Chunk> take_chunk(Int3 coordinates);
    [[nodiscard]] Chunk* find_chunk(Int3 coordinates) noexcept;
    [[nodiscard]] const Chunk* find_chunk(Int3 coordinates) const noexcept;

    [[nodiscard]] BlockId get_block(Int3 world_position) const noexcept;
    void set_block(Int3 world_position, BlockId block);

    [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }
    [[nodiscard]] std::size_t memory_bytes() const noexcept;

private:
    std::unordered_map<Int3, Chunk> chunks_;
};

} // namespace heartstead
