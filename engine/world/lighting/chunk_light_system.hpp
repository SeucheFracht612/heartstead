#pragma once

#include "engine/core/result.hpp"
#include "engine/dirty/dirty_region.hpp"
#include "engine/profiling/latency_window.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/lighting/chunk_light_scheduler.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace heartstead::world {

struct ChunkLightSystemConfig {
    ChunkLightSchedulerConfig scheduler;
    std::size_t max_snapshot_cells_per_update = 4096;
    double apply_time_budget_ms = 2.0;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkLightSystemStats {
    bool relight_requested = false;
    bool snapshot_in_progress = false;
    bool solve_in_flight = false;
    std::size_t snapshot_chunk_count = 0;
    std::size_t snapshot_pending_cell_count = 0;
    std::size_t completed_mailbox_count = 0;
    std::size_t snapshot_cells_copied_this_update = 0;
    std::size_t changed_chunks_this_update = 0;
    std::size_t changed_cells_this_update = 0;
    std::uint64_t dirty_regions_consumed = 0;
    std::uint64_t submitted_fields = 0;
    std::uint64_t applied_fields = 0;
    std::uint64_t stale_snapshots = 0;
    std::uint64_t stale_results = 0;
    std::uint64_t failed_results = 0;
    std::uint64_t total_snapshot_cells_copied = 0;
    std::uint64_t total_changed_chunks = 0;
    std::uint64_t total_changed_cells = 0;
    std::uint64_t last_sunlight_queue_visits = 0;
    std::uint64_t last_block_light_queue_visits = 0;
    std::uint64_t total_sunlight_queue_visits = 0;
    std::uint64_t total_block_light_queue_visits = 0;
    std::uint64_t apply_budget_overruns = 0;
    std::size_t relight_response_completed_this_update = 0;
    std::size_t pending_relight_response_count = 0;
    std::uint64_t total_relight_response_completed = 0;
    std::uint64_t total_coalesced_relight_invalidations = 0;
    std::uint64_t total_abandoned_relight_invalidations = 0;
    profiling::LatencyWindowStats relight_convergence_latency;
    double last_solve_ms = 0.0;
    double last_apply_ms = 0.0;
};

class ChunkLightSystem {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<ChunkLightSystem>>
    create(const VoxelPalette& palette, ChunkLightSystemConfig config = {});

    ~ChunkLightSystem();

    ChunkLightSystem(const ChunkLightSystem&) = delete;
    ChunkLightSystem& operator=(const ChunkLightSystem&) = delete;

    [[nodiscard]] core::Status update(ChunkDatabase& chunks,
                                      dirty::DirtyRegionTracker& dirty_regions,
                                      const VoxelPalette& palette,
                                      std::span<const VoxelLightSource> sources = {});
    void shutdown() noexcept;

    [[nodiscard]] std::span<const ChunkCoord> changed_chunks() const noexcept;
    [[nodiscard]] const ChunkLightSystemStats& stats() noexcept;
    [[nodiscard]] const ChunkLightSystemStats& stats() const noexcept;
    void reset_latency_observations() noexcept;

  private:
    struct SnapshotBuildState {
        std::vector<ChunkIdentity> identities;
        VoxelLightSnapshot snapshot;
        std::size_t chunk_index = 0;
        std::size_t cell_index = 0;
        std::uint64_t source_revision = 0;
    };

    ChunkLightSystem(ChunkLightSystemConfig config, std::unique_ptr<ChunkLightScheduler> scheduler,
                     std::shared_ptr<const VoxelLightBlockTable> block_table);

    [[nodiscard]] core::Status refresh_block_table(const VoxelPalette& palette,
                                                   ChunkDatabase& chunks);
    void collect_dirty(ChunkDatabase& chunks, dirty::DirtyRegionTracker& dirty_regions);
    void invalidate_field(
        ChunkDatabase& chunks,
        std::optional<dirty::DirtyRegionClock::time_point> invalidated_at = std::nullopt);
    void track_relight_response(dirty::DirtyRegionClock::time_point started_at) noexcept;
    void complete_relight_response(dirty::DirtyRegionClock::time_point completed_at) noexcept;
    void begin_snapshot(ChunkDatabase& chunks);
    [[nodiscard]] bool snapshot_still_current(const ChunkDatabase& chunks) const;
    [[nodiscard]] core::Status advance_snapshot(ChunkDatabase& chunks);
    [[nodiscard]] core::Status submit_snapshot(ChunkDatabase& chunks);
    [[nodiscard]] core::Status apply_completed(ChunkDatabase& chunks,
                                               dirty::DirtyRegionTracker& dirty_regions);
    void refresh_stats() noexcept;

    ChunkLightSystemConfig config_{};
    std::unique_ptr<ChunkLightScheduler> scheduler_;
    std::shared_ptr<const VoxelLightBlockTable> block_table_;
    std::optional<SnapshotBuildState> snapshot_build_;
    std::map<ChunkIdentity, std::uint64_t> observed_dirty_revisions_;
    std::vector<ChunkCoord> changed_chunks_;
    std::vector<VoxelLightSource> sources_;
    std::optional<dirty::DirtyRegionClock::time_point> pending_relight_response_;
    static constexpr std::size_t relight_latency_window_size = 256;
    profiling::LatencyWindow<relight_latency_window_size> relight_latency_;
    bool relight_requested_ = false;
    std::uint64_t source_revision_ = 1;
    std::uint64_t next_request_id_ = 1;
    ChunkLightSystemStats stats_{};
};

} // namespace heartstead::world
