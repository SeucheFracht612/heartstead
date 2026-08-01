#pragma once

#include "engine/core/result.hpp"
#include "engine/profiling/cpu_timing.hpp"
#include "engine/renderer/camera/frustum.hpp"
#include "engine/renderer/chunks/chunk_gpu_cache.hpp"
#include "engine/renderer/chunks/chunk_mesh_scheduler.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/renderer_config.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"
#include "engine/renderer/visibility/visibility_hierarchy.hpp"
#include "engine/world/streaming/chunk_streamer.hpp"
#include "engine/world/world_state.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_set>
#include <vector>

namespace heartstead::renderer {

struct ChunkRenderConfig {
    WorldRenderDistances distances{};
    std::size_t max_chunks_meshed_per_frame = 4;
    std::size_t max_chunks_uploaded_per_frame = 4;
    std::size_t max_bytes_uploaded_per_frame = 8 * 1024 * 1024;
    std::size_t max_snapshot_cells_per_frame = 512 * 1024;
    std::size_t max_completed_mesh_results_per_frame = 16;
    std::uint32_t mesh_worker_count = 2;
    std::size_t max_concurrent_mesh_jobs = 4;
    std::size_t max_cached_snapshot_buffers = 8;
    std::size_t max_cached_mesh_buffers = 8;
    std::size_t gpu_terrain_budget_bytes = 192U * 1024U * 1024U;
    ChunkMeshingMode meshing_mode = ChunkMeshingMode::greedy;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkRenderStats {
    ChunkGpuCacheStats cache;
    std::size_t pending_mesh_count = 0;
    std::size_t pending_upload_count = 0;
    std::size_t pending_upload_bytes = 0;

    std::size_t meshed_chunk_count = 0;
    std::size_t uploaded_chunk_count = 0;
    std::size_t uploaded_bytes = 0;
    std::size_t failed_mesh_count = 0;
    std::size_t failed_upload_count = 0;
    std::size_t scheduled_mesh_count = 0;
    std::size_t cancelled_mesh_count = 0;
    std::size_t stale_mesh_result_count = 0;
    std::uint64_t total_failed_mesh_count = 0;
    std::uint64_t total_failed_upload_count = 0;
    std::uint64_t total_stale_mesh_result_count = 0;
    std::uint64_t total_completed_mesh_job_count = 0;
    std::uint64_t total_built_mesh_count = 0;
    std::uint64_t total_published_mesh_count = 0;
    double mesh_builds_per_publication = 0.0;
    std::size_t in_flight_mesh_count = 0;
    std::size_t snapshot_cells_copied = 0;
    std::size_t pooled_cpu_mesh_buffers = 0;
    std::size_t pooled_cpu_mesh_vertex_capacity = 0;
    std::size_t pooled_cpu_mesh_index_capacity = 0;
    std::size_t pooled_gpu_vertex_buffers = 0;
    std::size_t pooled_gpu_vertex_capacity = 0;

    std::size_t visible_chunk_count = 0;
    std::size_t culled_chunk_count = 0;
    std::size_t distance_culled_chunk_count = 0;
    std::size_t frustum_culled_chunk_count = 0;
    std::size_t drawn_chunk_count = 0;
    std::size_t draw_count = 0;
    std::size_t visible_vertex_count = 0;
    std::size_t visible_index_count = 0;
    std::size_t distance_evicted_mesh_count = 0;
    std::size_t distance_evicted_mesh_bytes = 0;
    std::size_t memory_pressure_evicted_mesh_count = 0;
    std::size_t memory_pressure_evicted_mesh_bytes = 0;
    std::size_t residency_suppressed_chunk_count = 0;
    std::size_t gpu_terrain_budget_bytes = 0;
    std::size_t visibility_hierarchy_nodes = 0;
    std::size_t visibility_nodes_tested = 0;
    std::size_t visibility_nodes_culled = 0;

    double culling_ms = 0.0;
    double draw_list_ms = 0.0;
    double chunk_snapshot_ms = 0.0;
    double meshing_ms = 0.0;
    double upload_preparation_ms = 0.0;
    double upload_ms = 0.0;
    double gpu_wait_ms = 0.0;
};

struct ChunkDrawList {
    std::vector<rhi::RenderDrawCommand> draws;
    std::vector<std::vector<rhi::RenderDrawCommand>> shadow_draws;
    std::size_t visible_chunk_count = 0;
    std::size_t culled_chunk_count = 0;
    std::size_t distance_culled_chunk_count = 0;
    std::size_t frustum_culled_chunk_count = 0;
    std::size_t drawn_chunk_count = 0;
    std::size_t vertex_count = 0;
    std::size_t index_count = 0;
    std::size_t visibility_hierarchy_nodes = 0;
    std::size_t visibility_nodes_tested = 0;
    std::size_t visibility_nodes_culled = 0;
};

struct TerrainPipelineSet {
    rhi::RenderResourceHandle opaque;
    rhi::RenderResourceHandle alpha_tested;
    rhi::RenderResourceHandle transparent;
    rhi::RenderResourceHandle fluid;

    TerrainPipelineSet() = default;
    constexpr TerrainPipelineSet(rhi::RenderResourceHandle opaque_pipeline,
                                 rhi::RenderResourceHandle alpha_tested_pipeline,
                                 rhi::RenderResourceHandle transparent_pipeline,
                                 rhi::RenderResourceHandle fluid_pipeline) noexcept
        : opaque(opaque_pipeline), alpha_tested(alpha_tested_pipeline),
          transparent(transparent_pipeline), fluid(fluid_pipeline) {}

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] rhi::RenderResourceHandle
    for_phase(world::MeshingRenderPhase phase) const noexcept;
};

class ChunkRenderSystem {
  public:
    ChunkRenderSystem(ChunkGpuCache& cache, rhi::RenderResourceHandle terrain_pipeline,
                      const world::VoxelPalette* palette, ChunkRenderConfig config);
    ChunkRenderSystem(ChunkGpuCache& cache, TerrainPipelineSet terrain_pipelines,
                      const world::VoxelPalette* palette, ChunkRenderConfig config);
    ~ChunkRenderSystem();

    [[nodiscard]] core::Status initialize();
    void shutdown() noexcept;

    [[nodiscard]] core::Status
    process_chunk_loads(std::span<const world::ChunkStreamLoadReport> loads);
    [[nodiscard]] core::Status
    process_chunk_evictions(std::span<const world::ChunkIdentity> evictions);
    [[nodiscard]] core::Status
    process_chunk_evictions(const world::ChunkStreamEvictionReport& eviction);

    [[nodiscard]] core::Status synchronize(world::WorldState& world, const RenderCamera& camera);
    [[nodiscard]] core::Status
    set_terrain_pipeline(rhi::RenderResourceHandle terrain_pipeline) noexcept;
    [[nodiscard]] core::Status set_terrain_pipelines(TerrainPipelineSet pipelines) noexcept;
    [[nodiscard]] ChunkDrawList build_draw_list(const RenderCamera& camera);
    [[nodiscard]] ChunkDrawList
    build_draw_list(const RenderCamera& camera,
                    std::vector<rhi::RenderDrawCommand> reusable_draw_storage,
                    std::span<const math::Mat4f> shadow_view_projections = {});

    [[nodiscard]] const ChunkRenderStats& stats() const noexcept;
    void reset_session_stats() noexcept;

  private:
    struct VisibleChunk {
        const ChunkGpuEntry* entry = nullptr;
        math::Vec3f origin{};
        bool camera_visible = true;
        std::uint64_t shadow_visibility_mask = 0;
    };

    struct PendingMesh {
        world::ChunkIdentity identity;
        bool forced = false;
        std::uint64_t sequence = 0;
    };

    struct PendingUpload {
        world::ChunkIdentity identity;
        world::ChunkStageTicket stage_ticket;
        std::uint64_t content_revision = 0;
        std::uint64_t render_table_revision = 0;
        std::vector<world::ChunkDependencyRevision> dependency_revisions;
        std::vector<terrain::GpuChunkVertex> vertices;
        world::ChunkMesh mesh;
        bool forced = false;
        std::uint64_t sequence = 0;

        [[nodiscard]] std::size_t byte_size() const noexcept;
    };

    void enqueue_mesh(world::ChunkIdentity identity, bool forced);
    void remove_pending(world::ChunkIdentity identity);
    [[nodiscard]] core::Status reconcile_loaded_chunks(world::WorldState& world,
                                                       const RenderCamera& camera);
    void consume_dirty_regions(world::WorldState& world, const RenderCamera& camera);
    [[nodiscard]] core::Status refresh_render_table(world::WorldState& world,
                                                    const RenderCamera& camera);
    [[nodiscard]] core::Status process_completed_meshes(world::WorldState& world,
                                                        const RenderCamera& camera);
    [[nodiscard]] core::Status schedule_mesh_jobs(world::WorldState& world,
                                                  const RenderCamera& camera);
    [[nodiscard]] core::Status process_upload_queue(world::WorldState& world,
                                                    const RenderCamera& camera);
    [[nodiscard]] bool mesh_result_is_current(const ChunkMeshResult& result,
                                              const world::WorldState& world) const;
    [[nodiscard]] bool pending_upload_is_current(const PendingUpload& upload,
                                                 const world::WorldState& world) const;
    void prioritize_mesh_queue(const world::WorldState& world, const RenderCamera& camera);
    void prioritize_upload_queue(const RenderCamera& camera);
    [[nodiscard]] std::vector<terrain::GpuChunkVertex>
    acquire_gpu_vertices(std::size_t minimum_capacity);
    void recycle_upload_storage(PendingUpload& upload) noexcept;
    [[nodiscard]] bool is_visible(world::ChunkCoord coord, math::Bounds3f local_bounds,
                                  const RenderCamera& camera) const;
    [[nodiscard]] bool within_mesh_distance(world::ChunkCoord coord,
                                            const RenderCamera& camera) const;
    [[nodiscard]] bool within_visible_distance(world::ChunkCoord coord,
                                               const RenderCamera& camera) const;
    [[nodiscard]] bool within_gpu_retention_distance(world::ChunkCoord coord,
                                                     const RenderCamera& camera) const;
    [[nodiscard]] core::Status enforce_distance_residency(const RenderCamera& camera);
    [[nodiscard]] core::Status enforce_gpu_residency_budget(const RenderCamera& camera);
    [[nodiscard]] bool should_restore_suppressed_residency(const ChunkGpuEntry& entry,
                                                           const RenderCamera& camera) const;
    [[nodiscard]] float distance_squared(world::ChunkCoord coord, math::Bounds3f local_bounds,
                                         const RenderCamera& camera) const;
    [[nodiscard]] static core::Result<math::Vec3f>
    camera_relative_chunk_origin(world::ChunkCoord coord, const RenderCamera& camera);
    void refresh_queue_stats() noexcept;
    void refresh_timing_stats() noexcept;

    ChunkGpuCache* cache_ = nullptr;
    TerrainPipelineSet terrain_pipelines_;
    const world::VoxelPalette* palette_ = nullptr;
    ChunkRenderConfig config_{};
    std::vector<PendingMesh> pending_meshes_;
    std::vector<PendingUpload> pending_uploads_;
    std::vector<std::vector<terrain::GpuChunkVertex>> gpu_vertex_pool_;
    std::vector<VisibleChunk> visible_chunks_scratch_;
    VisibilityHierarchy visibility_hierarchy_;
    std::unordered_set<VisibilityKey> visibility_keys_;
    std::unique_ptr<ChunkMeshScheduler> mesh_scheduler_;
    std::shared_ptr<const world::BlockRenderTableSnapshot> render_table_;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t visibility_epoch_ = 0;
    ChunkRenderStats stats_{};
    profiling::CpuTimingRecorder timings_{};
};

} // namespace heartstead::renderer
