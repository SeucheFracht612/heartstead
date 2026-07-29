#pragma once

#include "engine/core/result.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/coords/world_position.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cstddef>
#include <cstdint>

namespace heartstead::game::foundation {

inline constexpr std::uint64_t world_seed = 0x4845415254535445ULL;
inline constexpr std::uint32_t layout_version = 1;
inline constexpr world::BlockCoord boundary_edit_upper{29, 0, 7};
inline constexpr world::BlockCoord boundary_edit_lower{29, -1, 7};

struct FoundationWorldBuildStats {
    std::size_t voxel_writes = 0;
    std::size_t chunk_count = 0;
};

[[nodiscard]] world::WorldPosition spawn_position() noexcept;

[[nodiscard]] core::Result<FoundationWorldBuildStats>
build_world(world::ChunkDatabase& chunks, const world::VoxelPalette& palette);

} // namespace heartstead::game::foundation
