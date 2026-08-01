#include "game/foundation/foundation_world.hpp"

#include "engine/world/fluids/fluid_state.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace heartstead::game::foundation {

namespace {

struct FoundationCells {
    world::VoxelCell clay;
    world::VoxelCell dirt;
    world::VoxelCell grass;
    world::VoxelCell half_step;
    world::VoxelCell low_ceiling;
    world::VoxelCell stone;
    world::VoxelCell water;
};

[[nodiscard]] core::Result<world::VoxelCell> required_cell(const world::VoxelPalette& palette,
                                                           std::string_view encoded_id) {
    const auto id = core::PrototypeId::parse(encoded_id);
    if (!id) {
        return core::Result<world::VoxelCell>::failure(
            "foundation_world.invalid_voxel_id",
            "foundation world contains an invalid voxel prototype id");
    }
    auto cell = palette.cell_for(id.value());
    if (!cell) {
        return core::Result<world::VoxelCell>::failure(
            "foundation_world.missing_voxel",
            "foundation world requires loaded voxel prototype " + std::string(encoded_id));
    }
    return cell;
}

[[nodiscard]] core::Result<FoundationCells> resolve_cells(const world::VoxelPalette& palette) {
    auto clay = required_cell(palette, "base:voxels/clay");
    auto dirt = required_cell(palette, "base:voxels/dirt");
    auto grass = required_cell(palette, "base:voxels/grass");
    auto half_step = required_cell(palette, "base:voxels/foundation_half_step");
    auto low_ceiling = required_cell(palette, "base:voxels/foundation_low_ceiling");
    auto stone = required_cell(palette, "base:voxels/stone");
    auto water = required_cell(palette, "base:voxels/water");
    if (!clay || !dirt || !grass || !half_step || !low_ceiling || !stone || !water) {
        const auto* error = !clay          ? &clay.error()
                            : !dirt        ? &dirt.error()
                            : !grass       ? &grass.error()
                            : !half_step   ? &half_step.error()
                            : !low_ceiling ? &low_ceiling.error()
                            : !stone       ? &stone.error()
                                           : &water.error();
        return core::Result<FoundationCells>::failure(error->code, error->message);
    }
    auto water_cell = water.value();
    water_cell.state_bits = world::full_fluid_source_state_bits();
    return core::Result<FoundationCells>::success({clay.value(), dirt.value(), grass.value(),
                                                   half_step.value(), low_ceiling.value(),
                                                   stone.value(), water_cell});
}

[[nodiscard]] core::Status set_block(world::ChunkDatabase& chunks, world::BlockCoord block,
                                     world::VoxelCell cell, std::size_t& writes) {
    const auto address = world::block_to_chunk_local(block);
    auto status = chunks.get_or_create(address.chunk).set(address.local, cell);
    if (status) {
        ++writes;
    }
    return status;
}

[[nodiscard]] core::Status fill_box(world::ChunkDatabase& chunks, world::BlockCoord minimum,
                                    world::BlockCoord maximum, world::VoxelCell cell,
                                    std::size_t& writes) {
    for (auto x = minimum.x; x <= maximum.x; ++x) {
        for (auto y = minimum.y; y <= maximum.y; ++y) {
            for (auto z = minimum.z; z <= maximum.z; ++z) {
                auto status = set_block(chunks, {x, y, z}, cell, writes);
                if (!status) {
                    return status;
                }
            }
        }
    }
    return core::Status::ok();
}

} // namespace

world::WorldPosition spawn_position() noexcept {
    return {8.5, 1.0, 8.5};
}

core::Result<FoundationWorldBuildStats> build_world(world::ChunkDatabase& chunks,
                                                    const world::VoxelPalette& palette) {
    return build_world(chunks, palette, {});
}

core::Result<FoundationWorldBuildStats> build_world(world::ChunkDatabase& chunks,
                                                    const world::VoxelPalette& palette,
                                                    std::stop_token stop_token) {
    const auto cancellation = []() {
        return core::Result<FoundationWorldBuildStats>::failure(
            "session_startup.cancelled", "world generation was cancelled");
    };
    if (stop_token.stop_requested()) {
        return cancellation();
    }
    auto cells = resolve_cells(palette);
    if (!cells) {
        return core::Result<FoundationWorldBuildStats>::failure(cells.error().code,
                                                                cells.error().message);
    }

    FoundationWorldBuildStats stats;
    auto status = core::Status::ok();

    // A compact, deterministic three-layer test island. The negative-Y chunk gives holes and
    // water real depth while keeping the visible surface in the origin chunk.
    for (std::int64_t x = 0; x < world::VoxelChunk::edge_length && status; ++x) {
        if (stop_token.stop_requested()) {
            return cancellation();
        }
        for (std::int64_t z = 0; z < world::VoxelChunk::edge_length && status; ++z) {
            status = set_block(chunks, {x, -2, z}, cells.value().stone, stats.voxel_writes);
            if (status) {
                status = set_block(chunks, {x, -1, z}, cells.value().dirt, stats.voxel_writes);
            }
            if (status) {
                status = set_block(chunks, {x, 0, z}, cells.value().grass, stats.voxel_writes);
            }
        }
    }

    // Material swatches near the southwest corner.
    constexpr std::array<std::pair<std::int64_t, std::int64_t>, 4> swatches{{
        {1, 2},
        {3, 4},
        {5, 6},
        {7, 8},
    }};
    const std::array swatch_cells{
        cells.value().grass,
        cells.value().dirt,
        cells.value().stone,
        cells.value().clay,
    };
    if (stop_token.stop_requested()) {
        return cancellation();
    }
    for (std::size_t index = 0; index < swatches.size() && status; ++index) {
        status = fill_box(chunks, {swatches[index].first, 0, 1}, {swatches[index].second, 0, 4},
                          swatch_cells[index], stats.voxel_writes);
    }

    // Clay border markers identify an editable grass cell on the y=0/-1 chunk seam. This station
    // makes cross-chunk remeshing and collision invalidation visible without enlarging the
    // compact course.
    if (status) {
        status = fill_box(chunks, {27, 0, 5}, {31, 0, 9}, cells.value().clay, stats.voxel_writes);
    }
    if (status) {
        status = set_block(chunks, boundary_edit_upper, cells.value().grass, stats.voxel_writes);
    }

    // Voxel-native "slope": 0.5 m terraces made only from full and partial-height blocks.
    if (status) {
        status =
            fill_box(chunks, {3, 1, 14}, {6, 1, 14}, cells.value().half_step, stats.voxel_writes);
    }
    if (status) {
        status = fill_box(chunks, {3, 1, 15}, {6, 1, 15}, cells.value().stone, stats.voxel_writes);
    }
    if (status) {
        status = fill_box(chunks, {3, 1, 16}, {6, 1, 16}, cells.value().stone, stats.voxel_writes);
    }
    if (status) {
        status =
            fill_box(chunks, {3, 2, 16}, {6, 2, 16}, cells.value().half_step, stats.voxel_writes);
    }
    if (status) {
        status = fill_box(chunks, {3, 1, 17}, {6, 2, 20}, cells.value().stone, stats.voxel_writes);
    }
    if (status) {
        status = fill_box(chunks, {3, 1, 21}, {6, 1, 21}, cells.value().stone, stats.voxel_writes);
    }
    if (status) {
        status =
            fill_box(chunks, {3, 2, 21}, {6, 2, 21}, cells.value().half_step, stats.voxel_writes);
    }
    if (status) {
        status = fill_box(chunks, {3, 1, 22}, {6, 1, 22}, cells.value().stone, stats.voxel_writes);
    }
    if (status) {
        status =
            fill_box(chunks, {3, 1, 23}, {6, 1, 23}, cells.value().half_step, stats.voxel_writes);
    }

    // A deliberate one-block ledge and a 1.5 m-clearance crouch tunnel.
    if (status) {
        status =
            fill_box(chunks, {10, 1, 14}, {12, 1, 17}, cells.value().stone, stats.voxel_writes);
    }
    if (status) {
        status = fill_box(chunks, {14, 2, 14}, {16, 2, 20}, cells.value().low_ceiling,
                          stats.voxel_writes);
    }

    // A two-block-deep dry hole and a neighboring two-block-deep water pool.
    if (status) {
        status = fill_box(chunks, {19, -1, 14}, {21, 0, 16}, world::VoxelCell::air(),
                          stats.voxel_writes);
    }
    if (status) {
        status =
            fill_box(chunks, {24, -1, 14}, {29, 0, 20}, cells.value().water, stats.voxel_writes);
    }

    // Ledges for falling/landing and camera composition checks.
    if (status) {
        status =
            fill_box(chunks, {20, 1, 23}, {23, 1, 27}, cells.value().stone, stats.voxel_writes);
    }
    if (status) {
        status =
            fill_box(chunks, {21, 2, 25}, {22, 2, 27}, cells.value().stone, stats.voxel_writes);
    }

    // Spawn and the starting-cargo strip remain guaranteed clear after every base rebuild.
    if (stop_token.stop_requested()) {
        return cancellation();
    }
    if (status) {
        status =
            fill_box(chunks, {6, 1, 6}, {12, 4, 12}, world::VoxelCell::air(), stats.voxel_writes);
    }
    if (!status) {
        return core::Result<FoundationWorldBuildStats>::failure(status.error().code,
                                                                status.error().message);
    }
    stats.chunk_count = chunks.chunk_count();
    return core::Result<FoundationWorldBuildStats>::success(stats);
}

} // namespace heartstead::game::foundation
