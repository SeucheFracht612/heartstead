#pragma once

#include "engine/core/result.hpp"
#include "engine/world/chunks/chunk_database.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::world {

class ChunkEditDeltaTextCodec {
  public:
    [[nodiscard]] static std::string encode(ChunkCoord coord,
                                            std::span<const VoxelEditRecord> edits);
    [[nodiscard]] static std::string encode(ChunkCoord coord,
                                            const std::vector<const VoxelEditRecord*>& edits);
    [[nodiscard]] static core::Result<std::vector<VoxelEditRecord>>
    decode(ChunkCoord expected_coord, std::string_view text);
};

} // namespace heartstead::world
