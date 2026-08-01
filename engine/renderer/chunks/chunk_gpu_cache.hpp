#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/renderer/memory/gpu_buffer_arena.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/terrain/gpu_chunk_vertex.hpp"
#include "engine/world/chunks/chunk_identity.hpp"
#include "engine/world/meshing/chunk_mesh_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace heartstead::renderer {

enum class ChunkGpuState {
    missing,
    cpu_mesh_ready,
    upload_pending,
    resident,
    failed,
};

struct ChunkGpuMesh {
    GpuAllocation vertices;
    GpuAllocation indices;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    rhi::RenderIndexType index_type = rhi::RenderIndexType::uint16;
    std::vector<world::ChunkMeshSection> sections;

    [[nodiscard]] bool is_empty() const noexcept;
};

struct ChunkGpuEntry {
    world::ChunkIdentity identity;
    std::uint64_t resident_content_revision = 0;
    std::uint64_t resident_render_table_revision = 0;
    std::vector<world::ChunkDependencyRevision> resident_dependency_revisions;

    ChunkGpuMesh mesh;

    math::Bounds3f local_bounds{};
    ChunkGpuState state = ChunkGpuState::missing;
    std::uint64_t last_visible_epoch = 0;
    std::size_t last_resident_bytes = 0;
    bool residency_suppressed = false;

    [[nodiscard]] bool has_drawable_mesh() const noexcept;
    [[nodiscard]] std::size_t resident_bytes() const noexcept;
};

struct ChunkGpuCacheConfig {
    std::uint64_t vertex_initial_bytes = 16U * 1024U * 1024U;
    std::uint64_t vertex_maximum_bytes = 256U * 1024U * 1024U;
    std::uint64_t index_initial_bytes = 8U * 1024U * 1024U;
    std::uint64_t index_maximum_bytes = 128U * 1024U * 1024U;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkGpuCacheStats {
    std::size_t entry_count = 0;
    std::size_t resident_chunk_count = 0;
    std::size_t empty_chunk_count = 0;
    std::size_t resident_buffer_count = 0;
    std::size_t resident_allocation_count = 0;
    std::size_t resident_bytes = 0;
    std::size_t uint16_index_chunk_count = 0;
    std::size_t uint32_index_chunk_count = 0;
    std::size_t resident_section_count = 0;
    std::uint64_t uploaded_chunk_count = 0;
    std::uint64_t uploaded_bytes = 0;
    std::uint64_t upload_batch_count = 0;
    std::size_t last_upload_chunk_count = 0;
    std::size_t last_upload_write_count = 0;
    std::uint64_t failed_upload_count = 0;
    GpuBufferArenaStats vertex_arena;
    GpuBufferArenaStats index_arena;
};

struct ChunkGpuUploadResult {
    std::size_t uploaded_bytes = 0;
    bool empty = false;
    bool replaced_resident_mesh = false;
};

struct ChunkGpuMeshUpload {
    world::ChunkIdentity identity;
    std::uint64_t content_revision = 0;
    std::uint64_t render_table_revision = 0;
    math::Bounds3f local_bounds{};
    std::span<const terrain::GpuChunkVertex> vertices;
    std::span<const std::uint32_t> indices;
    std::span<const world::ChunkDependencyRevision> dependency_revisions;
    std::span<const world::ChunkMeshSection> sections;
};

struct ChunkGpuBatchUploadResult {
    std::vector<ChunkGpuUploadResult> uploads;
    std::size_t uploaded_bytes = 0;
    std::size_t write_count = 0;
    double cpu_gpu_wait_ms = 0.0;
};

class ChunkGpuCache {
  public:
    explicit ChunkGpuCache(rhi::IRenderDevice& device) noexcept;
    ~ChunkGpuCache();

    ChunkGpuCache(const ChunkGpuCache&) = delete;
    ChunkGpuCache& operator=(const ChunkGpuCache&) = delete;

    [[nodiscard]] core::Status initialize(ChunkGpuCacheConfig config = {});
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] core::Status insert(world::ChunkIdentity identity);
    [[nodiscard]] core::Status erase(world::ChunkIdentity identity);
    [[nodiscard]] core::Status release_mesh(world::ChunkIdentity identity);
    [[nodiscard]] core::Status clear();

    [[nodiscard]] bool contains(world::ChunkIdentity identity) const noexcept;
    [[nodiscard]] const ChunkGpuEntry* find(world::ChunkIdentity identity) const noexcept;
    [[nodiscard]] ChunkGpuEntry* find(world::ChunkIdentity identity) noexcept;
    [[nodiscard]] const ChunkGpuEntry* find_by_coordinate(world::ChunkCoord coord) const noexcept;
    [[nodiscard]] std::vector<const ChunkGpuEntry*> entries() const;

    void mark_cpu_mesh_ready(world::ChunkIdentity identity) noexcept;
    void mark_upload_pending(world::ChunkIdentity identity) noexcept;
    void mark_failed(world::ChunkIdentity identity) noexcept;
    void mark_visible(world::ChunkIdentity identity, std::uint64_t epoch) noexcept;

    [[nodiscard]] core::Result<ChunkGpuUploadResult>
    replace_mesh(world::ChunkIdentity identity, std::uint64_t content_revision,
                 math::Bounds3f local_bounds, std::span<const terrain::GpuChunkVertex> vertices,
                 std::span<const std::uint32_t> indices, std::uint64_t render_table_revision = 1,
                 std::span<const world::ChunkDependencyRevision> dependency_revisions = {},
                 std::span<const world::ChunkMeshSection> sections = {});
    [[nodiscard]] core::Result<ChunkGpuBatchUploadResult>
    replace_meshes(std::span<const ChunkGpuMeshUpload> uploads);

    [[nodiscard]] ChunkGpuCacheStats stats() const noexcept;
    void reset_session_stats() noexcept;

  private:
    [[nodiscard]] core::Status retire_entry_allocations(const ChunkGpuEntry& entry,
                                                        std::uint64_t submission_serial);
    void collect_retired() noexcept;
    void refresh_current_stats() noexcept;

    rhi::IRenderDevice* device_ = nullptr;
    std::unique_ptr<GpuBufferArena> vertex_arena_;
    std::unique_ptr<GpuBufferArena> index_arena_;
    std::map<world::ChunkIdentity, ChunkGpuEntry> entries_;
    ChunkGpuCacheStats stats_{};
};

} // namespace heartstead::renderer
