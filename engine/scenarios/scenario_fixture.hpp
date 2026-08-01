#pragma once

#include "engine/core/result.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"

#include <cstdint>

namespace heartstead::world {
class WorldState;
}

namespace heartstead::scenarios {

inline constexpr world::ChunkCoord renderer_proof_center{1'000'000'000, 0, -1'000'000'000};

struct RendererProofVoxelTypes {
    std::uint16_t grass = 1;
    std::uint16_t dirt = 2;
    std::uint16_t stone = 3;
};

[[nodiscard]] core::Status populate_renderer_proof_fixture(world::WorldState& state,
                                                           RendererProofVoxelTypes types = {});

} // namespace heartstead::scenarios
