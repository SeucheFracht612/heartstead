#pragma once

#include "engine/core/result.hpp"
#include "engine/dirty/dirty_region.hpp"
#include "engine/world/chunks/chunk_identity.hpp"
#include "engine/world/coords/world_coords.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace heartstead::world {

class ChunkDatabase;

struct FluidBlockInfo {
    bool fluid = false;
};

struct FluidBlockTable {
    std::uint64_t revision = 1;
    std::vector<FluidBlockInfo> blocks;

    [[nodiscard]] FluidBlockInfo block(std::uint16_t type) const noexcept;
    [[nodiscard]] core::Status validate() const;
};

[[nodiscard]] FluidBlockTable build_fluid_block_table(const VoxelPalette& palette);

struct FluidChunkSnapshot {
    ChunkIdentity identity{};
    std::uint64_t content_revision = 0;
    std::vector<VoxelCell> cells;

    [[nodiscard]] core::Status validate() const;
};

struct FluidSimulationSnapshot {
    std::vector<FluidChunkSnapshot> chunks;

    [[nodiscard]] core::Status validate() const;
};

[[nodiscard]] FluidSimulationSnapshot
build_fluid_simulation_snapshot(const ChunkDatabase& chunks);

struct FluidCellChange {
    ChunkLocalCoord address{};
    ChunkIdentity chunk_identity{};
    std::uint64_t source_content_revision = 0;
    VoxelCell previous{};
    VoxelCell next{};

    friend auto operator<=>(const FluidCellChange&, const FluidCellChange&) = default;
};

struct FluidStepStats {
    std::size_t input_active_cell_count = 0;
    std::size_t processed_active_cell_count = 0;
    std::size_t deferred_active_cell_count = 0;
    std::size_t proposal_count = 0;
    std::size_t accepted_transfer_count = 0;
    std::size_t transferred_unit_count = 0;
    std::size_t changed_cell_count = 0;
    std::size_t next_active_cell_count = 0;
    bool budget_exhausted = false;
};

struct FluidStepResult {
    std::uint64_t tick = 0;
    std::vector<FluidCellChange> changes;
    std::vector<ChunkLocalCoord> next_active;
    FluidStepStats stats;
};

[[nodiscard]] core::Result<FluidStepResult>
simulate_fluid_step(const FluidSimulationSnapshot& snapshot, const FluidBlockTable& blocks,
                    std::span<const ChunkLocalCoord> active_cells, std::size_t maximum_active_cells,
                    std::uint64_t tick);

struct FluidApplyReport {
    std::size_t changed_cell_count = 0;
    std::vector<ChunkCoord> changed_chunks;
};

[[nodiscard]] core::Result<FluidApplyReport>
apply_fluid_step(ChunkDatabase& chunks, dirty::DirtyRegionTracker& dirty_regions,
                 const FluidStepResult& result);

[[nodiscard]] std::vector<ChunkLocalCoord>
fluid_cells_and_neighbors(const FluidSimulationSnapshot& snapshot,
                          const FluidBlockTable& blocks);

} // namespace heartstead::world
