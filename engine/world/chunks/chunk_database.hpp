#pragma once

#include "engine/core/result.hpp"
#include "engine/dirty/dirty_region.hpp"
#include "engine/world/chunks/chunk_identity.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace heartstead::world {

class VoxelPalette;

struct VoxelEditRecord {
    ChunkCoord chunk_coord;
    VoxelCoord voxel_coord;
    VoxelCell previous;
    VoxelCell next;
};

struct ChunkDatabaseStats {
    std::size_t chunk_count = 0;
    std::size_t edit_count = 0;
    std::size_t dirty_mesh_count = 0;
    std::size_t dirty_save_count = 0;
    std::size_t dirty_replication_count = 0;
    std::array<ChunkStageCounts, chunk_stage_count> stages{};

    [[nodiscard]] const ChunkStageCounts& stage(ChunkStage chunk_stage) const noexcept {
        const auto index = static_cast<std::size_t>(chunk_stage);
        return stages[index < stages.size() ? index : 0];
    }
};

class ChunkDatabase {
  public:
    [[nodiscard]] VoxelChunk& get_or_create(ChunkCoord coord);
    [[nodiscard]] VoxelChunk* find(ChunkCoord coord) noexcept;
    [[nodiscard]] const VoxelChunk* find(ChunkCoord coord) const noexcept;
    [[nodiscard]] std::vector<const VoxelChunk*> records() const;
    [[nodiscard]] std::vector<ChunkIdentity> identities() const;
    [[nodiscard]] bool contains(ChunkCoord coord) const noexcept;
    [[nodiscard]] std::size_t chunk_count() const noexcept;
    bool erase(ChunkCoord coord);
    [[nodiscard]] core::Status insert_generated(VoxelChunk chunk);
    [[nodiscard]] core::Status insert_generated(VoxelChunk chunk,
                                                dirty::DirtyRegionTracker& dirty_regions);
    [[nodiscard]] core::Status
    insert_generated_with_saved_edits(VoxelChunk chunk, std::span<const VoxelEditRecord> edits);
    [[nodiscard]] core::Status
    insert_generated_with_saved_edits(VoxelChunk chunk, std::span<const VoxelEditRecord> edits,
                                      dirty::DirtyRegionTracker& dirty_regions);

    [[nodiscard]] core::Result<VoxelCell> get(ChunkCoord chunk_coord, VoxelCoord voxel_coord) const;
    [[nodiscard]] core::Status set(ChunkCoord chunk_coord, VoxelCoord voxel_coord, VoxelCell cell);
    [[nodiscard]] core::Status set(ChunkCoord chunk_coord, VoxelCoord voxel_coord, VoxelCell cell,
                                   dirty::DirtyRegionTracker& dirty_regions);
    [[nodiscard]] core::Status set(ChunkCoord chunk_coord, VoxelCoord voxel_coord, VoxelCell cell,
                                   dirty::DirtyRegionTracker& dirty_regions,
                                   const VoxelPalette& palette);
    [[nodiscard]] core::Status apply_saved_edits(std::span<const VoxelEditRecord> edits);
    [[nodiscard]] core::Status apply_saved_edits(std::span<const VoxelEditRecord> edits,
                                                 dirty::DirtyRegionTracker& dirty_regions);

    [[nodiscard]] const std::vector<VoxelEditRecord>& edit_log() const noexcept;
    void clear_edit_log();
    void clear_all_dirty();

    [[nodiscard]] ChunkDatabaseStats stats() const noexcept;

  private:
    struct ChunkCoordHash {
        [[nodiscard]] std::size_t operator()(ChunkCoord coord) const noexcept;
    };

    [[nodiscard]] core::Status insert_generated_impl(VoxelChunk chunk,
                                                     dirty::DirtyRegionTracker* dirty_regions);
    [[nodiscard]] core::Status
    insert_generated_with_saved_edits_impl(VoxelChunk chunk, std::span<const VoxelEditRecord> edits,
                                           dirty::DirtyRegionTracker* dirty_regions);
    [[nodiscard]] core::Status apply_saved_edits_impl(std::span<const VoxelEditRecord> edits,
                                                      dirty::DirtyRegionTracker* dirty_regions);
    [[nodiscard]] static core::Status
    validate_saved_edit_batch(std::span<const VoxelEditRecord> edits,
                              const ChunkCoord* expected_chunk = nullptr);
    [[nodiscard]] std::vector<VoxelEditRecord>
    build_replaced_saved_edit_history(std::span<const VoxelEditRecord> touched_edits,
                                      std::span<const VoxelEditRecord> canonical_edits) const;
    [[nodiscard]] core::Status
    mark_neighbor_dirty_if_boundary(ChunkCoord chunk_coord, VoxelCoord voxel_coord,
                                    dirty::DirtyRegionTracker* dirty_regions);
    [[nodiscard]] std::vector<ChunkCoord>
    rich_mesh_invalidation_neighbors(ChunkCoord chunk_coord, VoxelCoord voxel_coord,
                                     std::uint16_t radius) const;
    [[nodiscard]] static dirty::DirtyRegionCoord dirty_coord_for_chunk(ChunkCoord coord) noexcept;
    [[nodiscard]] static core::Status
    mark_chunk_rebuild_regions(dirty::DirtyRegionTracker& dirty_regions, ChunkCoord coord,
                               std::string reason);
    [[nodiscard]] static std::vector<ChunkCoord> boundary_neighbors(ChunkCoord chunk_coord,
                                                                    VoxelCoord voxel_coord);
    [[nodiscard]] static std::vector<ChunkCoord> face_neighbors(ChunkCoord chunk_coord);

    std::unordered_map<ChunkCoord, VoxelChunk, ChunkCoordHash> chunks_;
    std::vector<VoxelEditRecord> edit_log_;
};

} // namespace heartstead::world
