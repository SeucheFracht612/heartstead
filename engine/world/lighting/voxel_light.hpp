#pragma once

#include "engine/core/result.hpp"
#include "engine/dirty/dirty_region.hpp"
#include "engine/world/chunks/chunk_identity.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace heartstead::world {

class ChunkDatabase;

inline constexpr std::uint8_t maximum_voxel_light = 255;
inline constexpr std::uint8_t minimum_voxel_light_attenuation = 16;

struct VoxelLightBlockInfo {
    std::uint8_t emission = 0;
    std::uint8_t absorption = maximum_voxel_light;
};

struct VoxelLightBlockTable {
    std::uint64_t revision = 1;
    std::vector<VoxelLightBlockInfo> blocks;

    [[nodiscard]] VoxelLightBlockInfo block(std::uint16_t type) const noexcept;
    [[nodiscard]] core::Status validate() const;
};

[[nodiscard]] VoxelLightBlockTable build_voxel_light_block_table(const VoxelPalette& palette);

struct ChunkLightSnapshot {
    ChunkIdentity identity{};
    ChunkStageTicket stage_ticket{};
    std::uint64_t content_revision = 0;
    std::vector<VoxelCell> cells;

    [[nodiscard]] core::Status validate() const;
};

struct VoxelLightSource {
    BlockCoord position{};
    std::uint8_t light = 0;

    friend auto operator<=>(const VoxelLightSource&, const VoxelLightSource&) = default;
};

struct VoxelLightSnapshot {
    std::vector<ChunkLightSnapshot> chunks;
    std::vector<VoxelLightSource> sources;

    [[nodiscard]] core::Status validate() const;
};

[[nodiscard]] VoxelLightSnapshot build_voxel_light_snapshot(const ChunkDatabase& chunks);

struct ChunkLightPatch {
    ChunkIdentity identity{};
    ChunkStageTicket stage_ticket{};
    std::uint64_t source_content_revision = 0;
    std::vector<std::uint8_t> lights;

    [[nodiscard]] core::Status validate() const;
};

struct VoxelLightSolveStats {
    std::size_t chunk_count = 0;
    std::size_t cell_count = 0;
    std::size_t sunlight_seed_count = 0;
    std::size_t block_light_seed_count = 0;
    std::size_t sunlight_queue_visits = 0;
    std::size_t block_light_queue_visits = 0;
};

struct VoxelLightSolveResult {
    std::vector<ChunkLightPatch> patches;
    VoxelLightSolveStats stats;
};

[[nodiscard]] core::Result<VoxelLightSolveResult>
solve_voxel_light(const VoxelLightSnapshot& snapshot, const VoxelLightBlockTable& blocks);

struct VoxelLightApplyReport {
    std::size_t patch_count = 0;
    std::size_t changed_chunk_count = 0;
    std::size_t changed_cell_count = 0;
    std::vector<ChunkCoord> changed_chunks;
};

[[nodiscard]] core::Result<VoxelLightApplyReport>
apply_voxel_light(ChunkDatabase& chunks, dirty::DirtyRegionTracker& dirty_regions,
                  const VoxelLightSolveResult& result);

} // namespace heartstead::world
