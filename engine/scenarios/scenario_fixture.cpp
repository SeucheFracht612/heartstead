#include "engine/scenarios/scenario_fixture.hpp"

#include "engine/world/world_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace heartstead::scenarios {

core::Status populate_renderer_proof_fixture(world::WorldState& state,
                                             RendererProofVoxelTypes types) {
    if (types.grass == 0 || types.dirt == 0 || types.stone == 0) {
        return core::Status::failure("scenario_fixture.invalid_voxel_type",
                                     "renderer proof voxel types must be non-air");
    }
    constexpr auto edge = static_cast<std::int64_t>(world::VoxelChunk::edge_length);
    for (std::int64_t chunk_z = -1; chunk_z <= 1; ++chunk_z) {
        for (std::int64_t chunk_x = -1; chunk_x <= 1; ++chunk_x) {
            const world::ChunkCoord coord{renderer_proof_center.x + chunk_x,
                                          renderer_proof_center.y,
                                          renderer_proof_center.z + chunk_z};
            auto& chunk = state.chunks().get_or_create(coord);
            for (std::uint16_t z = 0; z < world::VoxelChunk::edge_length; ++z) {
                for (std::uint16_t x = 0; x < world::VoxelChunk::edge_length; ++x) {
                    const auto local_x = static_cast<float>(chunk_x * edge + x);
                    const auto local_z = static_cast<float>(chunk_z * edge + z);
                    const auto wave =
                        3.0F * std::sin(local_x * 0.12F) + 2.0F * std::cos(local_z * 0.15F);
                    const auto height = static_cast<std::uint16_t>(
                        std::clamp(7 + static_cast<int>(std::lround(wave)), 2, 14));
                    for (std::uint16_t y = 0; y <= height; ++y) {
                        const auto type = y == height       ? types.grass
                                          : y + 3 >= height ? types.dirt
                                                            : types.stone;
                        auto status = chunk.set({x, y, z}, world::VoxelCell{type, 255, 0, 0});
                        if (!status) {
                            return status;
                        }
                    }
                }
            }
        }
    }
    return core::Status::ok();
}

} // namespace heartstead::scenarios
