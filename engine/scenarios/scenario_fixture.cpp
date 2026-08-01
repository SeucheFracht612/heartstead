#include "engine/scenarios/scenario_fixture.hpp"

#include "engine/core/ids.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heartstead::scenarios {

namespace {

[[nodiscard]] core::Status validate_types(RendererProofVoxelTypes types) {
    if (types.grass == 0 || types.dirt == 0 || types.stone == 0) {
        return core::Status::failure("scenario_fixture.invalid_voxel_type",
                                     "renderer proof voxel types must be non-air");
    }
    return core::Status::ok();
}

[[nodiscard]] core::Result<std::uint16_t> voxel_type(const world::VoxelPalette& palette,
                                                     std::string_view prototype_id) {
    const auto parsed = core::PrototypeId::parse(prototype_id);
    const auto type = parsed.has_value() ? palette.type_for(*parsed) : std::nullopt;
    if (!type.has_value()) {
        return core::Result<std::uint16_t>::failure("scenario_fixture.voxel_missing",
                                                    "renderer proof voxel is not available: " +
                                                        std::string(prototype_id));
    }
    return core::Result<std::uint16_t>::success(*type);
}

} // namespace

core::Result<RendererProofVoxelTypes>
resolve_renderer_proof_voxel_types(const world::VoxelPalette& palette) {
    auto grass = voxel_type(palette, "base:voxels/grass");
    auto dirt = voxel_type(palette, "base:voxels/dirt");
    auto stone = voxel_type(palette, "base:voxels/stone");
    if (!grass || !dirt || !stone) {
        const auto& error = !grass ? grass.error() : !dirt ? dirt.error() : stone.error();
        return core::Result<RendererProofVoxelTypes>::failure(error.code, error.message);
    }
    return core::Result<RendererProofVoxelTypes>::success(
        {grass.value(), dirt.value(), stone.value()});
}

core::Result<world::VoxelChunk> generate_renderer_proof_chunk(world::ChunkCoord coord,
                                                              RendererProofVoxelTypes types) {
    auto status = validate_types(types);
    if (!status) {
        return core::Result<world::VoxelChunk>::failure(status.error().code,
                                                        status.error().message);
    }

    constexpr auto edge = static_cast<std::int64_t>(world::VoxelChunk::edge_length);
    const auto chunk_x =
        static_cast<long double>(coord.x) - static_cast<long double>(renderer_proof_center.x);
    const auto chunk_z =
        static_cast<long double>(coord.z) - static_cast<long double>(renderer_proof_center.z);
    std::vector<world::VoxelCell> cells(world::VoxelChunk::total_cells, world::VoxelCell::air());
    constexpr auto cell_edge = static_cast<std::size_t>(world::VoxelChunk::edge_length);
    for (std::uint16_t z = 0; z < world::VoxelChunk::edge_length; ++z) {
        for (std::uint16_t x = 0; x < world::VoxelChunk::edge_length; ++x) {
            const auto local_x = static_cast<double>(chunk_x * edge + x);
            const auto local_z = static_cast<double>(chunk_z * edge + z);
            const auto wave = 3.0 * std::sin(local_x * 0.12) + 2.0 * std::cos(local_z * 0.15);
            const auto height = std::clamp(7 + static_cast<int>(std::lround(wave)), 2, 14);
            for (std::uint16_t y = 0; y <= height; ++y) {
                const auto type = y == height       ? types.grass
                                  : y + 3 >= height ? types.dirt
                                                    : types.stone;
                const auto cell_index = static_cast<std::size_t>(z) * cell_edge * cell_edge +
                                        static_cast<std::size_t>(y) * cell_edge + x;
                cells[cell_index] = world::VoxelCell{type, 255, 0, 0};
            }
        }
    }

    world::VoxelChunk chunk(coord);
    status = chunk.load_generated_cells(std::move(cells));
    if (!status) {
        return core::Result<world::VoxelChunk>::failure(status.error().code,
                                                        status.error().message);
    }
    return core::Result<world::VoxelChunk>::success(std::move(chunk));
}

core::Status populate_renderer_proof_fixture(world::WorldState& state,
                                             RendererProofVoxelTypes types) {
    auto status = validate_types(types);
    if (!status) {
        return status;
    }
    for (std::int64_t chunk_z = -1; chunk_z <= 1; ++chunk_z) {
        for (std::int64_t chunk_x = -1; chunk_x <= 1; ++chunk_x) {
            const world::ChunkCoord coord{renderer_proof_center.x + chunk_x,
                                          renderer_proof_center.y,
                                          renderer_proof_center.z + chunk_z};
            auto generated = generate_renderer_proof_chunk(coord, types);
            if (!generated) {
                return core::Status::failure(generated.error().code, generated.error().message);
            }
            status = state.chunks().insert_generated(std::move(generated).value(),
                                                     state.dirty_regions());
            if (!status) {
                return status;
            }
        }
    }
    return core::Status::ok();
}

} // namespace heartstead::scenarios
