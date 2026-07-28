#pragma once

#include "engine/core/result.hpp"
#include "engine/dirty/dirty_region.hpp"
#include "engine/physics/chunk_collision_scheduler.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/coords/world_position.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>

namespace heartstead::physics {

struct ChunkCollisionSystemConfig {
    ChunkCollisionSchedulerConfig scheduler;
    world::PhysicsIslandFrame physics_island{};
    std::size_t max_submissions_per_update = 2;
    std::size_t max_applies_per_update = 2;
    double apply_time_budget_ms = 2.0;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkCollisionBodyRecord {
    world::ChunkIdentity identity{};
    std::uint64_t content_revision = 0;
    std::uint64_t collision_table_revision = 0;
    PhysicsBodyId body_id{};
    std::uint32_t box_count = 0;
};

struct ChunkCollisionSystemStats {
    std::size_t resident_body_count = 0;
    std::size_t pending_chunk_count = 0;
    std::size_t in_flight_job_count = 0;
    std::size_t completed_mailbox_count = 0;
    std::uint32_t submitted_this_update = 0;
    std::uint32_t applied_this_update = 0;
    std::uint32_t removed_this_update = 0;
    std::uint64_t applied_shapes = 0;
    std::uint64_t removed_bodies = 0;
    std::uint64_t stale_results = 0;
    std::uint64_t failed_results = 0;
    std::uint64_t current_collision_boxes = 0;
    double last_cooking_ms = 0.0;
    double last_apply_ms = 0.0;
};

class ChunkCollisionSystem {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<ChunkCollisionSystem>>
    create(IPhysicsWorld& physics_world, const world::VoxelPalette& palette,
           ChunkCollisionSystemConfig config = {});

    ~ChunkCollisionSystem();

    ChunkCollisionSystem(const ChunkCollisionSystem&) = delete;
    ChunkCollisionSystem& operator=(const ChunkCollisionSystem&) = delete;

    [[nodiscard]] core::Status update(world::ChunkDatabase& chunks,
                                      dirty::DirtyRegionTracker& dirty_regions,
                                      const world::VoxelPalette& palette);
    void shutdown() noexcept;

    [[nodiscard]] const ChunkCollisionBodyRecord* find(world::ChunkCoord coordinate) const noexcept;
    [[nodiscard]] const ChunkCollisionSystemStats& stats() noexcept;
    [[nodiscard]] const ChunkCollisionSystemStats& stats() const noexcept;

  private:
    ChunkCollisionSystem(IPhysicsWorld& physics_world, ChunkCollisionSystemConfig config,
                         std::unique_ptr<ChunkCollisionScheduler> scheduler,
                         std::shared_ptr<const world::ChunkCollisionTableSnapshot> table);

    [[nodiscard]] core::Status refresh_collision_table(const world::VoxelPalette& palette,
                                                       const world::ChunkDatabase& chunks);
    void collect_dirty_chunks(world::ChunkDatabase& chunks,
                              dirty::DirtyRegionTracker& dirty_regions);
    [[nodiscard]] core::Status reconcile_unloaded_chunks(const world::ChunkDatabase& chunks);
    [[nodiscard]] core::Status apply_completed(world::ChunkDatabase& chunks);
    [[nodiscard]] core::Status submit_pending(const world::ChunkDatabase& chunks);
    [[nodiscard]] core::Status apply_result(world::ChunkDatabase& chunks,
                                            ChunkCollisionResult result);
    [[nodiscard]] core::Result<Vec3> chunk_physics_position(world::ChunkCoord coordinate) const;
    void refresh_stats() noexcept;

    IPhysicsWorld* physics_world_ = nullptr;
    ChunkCollisionSystemConfig config_{};
    std::unique_ptr<ChunkCollisionScheduler> scheduler_;
    std::shared_ptr<const world::ChunkCollisionTableSnapshot> collision_table_;
    std::map<world::ChunkCoord, ChunkCollisionBodyRecord> bodies_;
    std::set<world::ChunkCoord> pending_chunks_;
    ChunkCollisionSystemStats stats_{};
};

} // namespace heartstead::physics
