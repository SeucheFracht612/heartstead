#pragma once

#include "engine/core/result.hpp"
#include "engine/dirty/dirty_region.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/fluids/fluid_simulation.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <vector>

namespace heartstead::world {

struct ChunkFluidSystemConfig {
    std::uint32_t simulation_tick_interval = 3;
    std::size_t maximum_active_cells_per_step = 32'768;
    double apply_time_budget_ms = 2.0;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkFluidSystemStats {
    bool step_due = false;
    bool budget_exhausted = false;
    std::size_t active_cell_count = 0;
    std::size_t processed_cells_this_update = 0;
    std::size_t deferred_cells_this_update = 0;
    std::size_t proposals_this_update = 0;
    std::size_t changed_cells_this_update = 0;
    std::size_t changed_chunks_this_update = 0;
    std::uint64_t dirty_regions_consumed = 0;
    std::uint64_t topology_rebuilds = 0;
    std::uint64_t steps = 0;
    std::uint64_t settled_steps = 0;
    std::uint64_t total_processed_cells = 0;
    std::uint64_t total_changed_cells = 0;
    std::uint64_t budget_exhaustions = 0;
    std::uint64_t apply_budget_overruns = 0;
    double last_snapshot_ms = 0.0;
    double last_simulation_ms = 0.0;
    double last_apply_ms = 0.0;
};

class ChunkFluidSystem {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<ChunkFluidSystem>>
    create(const VoxelPalette& palette, ChunkFluidSystemConfig config = {});

    [[nodiscard]] core::Status update(ChunkDatabase& chunks,
                                      dirty::DirtyRegionTracker& dirty_regions,
                                      const VoxelPalette& palette, std::uint64_t tick);
    void activate(BlockCoord position);
    void activate(ChunkLocalCoord address);
    void clear() noexcept;

    [[nodiscard]] std::span<const ChunkCoord> changed_chunks() const noexcept;
    [[nodiscard]] const ChunkFluidSystemStats& stats() const noexcept;

  private:
    ChunkFluidSystem(ChunkFluidSystemConfig config, FluidBlockTable block_table);

    [[nodiscard]] core::Status refresh_block_table(const VoxelPalette& palette);
    void reconcile_topology(const ChunkDatabase& chunks);
    void collect_dirty(const ChunkDatabase& chunks, dirty::DirtyRegionTracker& dirty_regions);
    void activate_region(const ChunkDatabase& chunks, const dirty::DirtyRegionBounds& bounds);
    void rebuild_frontier(const ChunkDatabase& chunks);

    ChunkFluidSystemConfig config_{};
    FluidBlockTable block_table_;
    std::set<ChunkLocalCoord> active_;
    std::vector<ChunkIdentity> known_identities_;
    std::vector<ChunkCoord> changed_chunks_;
    ChunkFluidSystemStats stats_{};
};

} // namespace heartstead::world
