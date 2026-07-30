#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/world/blocks/block_model.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace heartstead::world {

enum class MeshingGeometryKind : std::uint8_t {
    full_cube,
    boxes,
    authored_faces,
    cross_plane,
    rich_model,
};

enum class MeshingRenderPhase : std::uint8_t {
    opaque,
    alpha_tested,
    transparent,
    fluid,
};

enum class MeshingBlockFlags : std::uint16_t {
    none = 0,
    emissive = 1U << 0U,
    two_sided = 1U << 1U,
    state_dependent = 1U << 2U,
};

struct MeshingBlockInfo {
    bool defined = false;
    MeshingGeometryKind geometry = MeshingGeometryKind::full_cube;
    MeshingRenderPhase render_phase = MeshingRenderPhase::opaque;
    std::uint16_t material_index = 0;
    std::uint16_t model_index = 0;
    std::uint16_t flags = 0;
    std::uint8_t occlusion_mask = 0;
    bool full_occluder = false;
    std::uint16_t neighbor_dependency_radius = 0;
    std::vector<BlockModelBox> boxes;
    std::vector<BlockModelTriangle> triangles;
    core::PrototypeId model_prototype_id;
    math::Bounds3f render_bounds{};
};

struct BlockRenderTableSnapshot {
    std::uint64_t revision = 1;
    bool legacy_cube_fallback = false;
    std::vector<MeshingBlockInfo> blocks;

    [[nodiscard]] const MeshingBlockInfo* find(std::uint16_t type) const noexcept;
    [[nodiscard]] core::Status validate() const;
};

[[nodiscard]] core::Result<BlockRenderTableSnapshot>
build_block_render_table_snapshot(const VoxelPalette* palette);

struct ChunkDependencyRevision {
    ChunkCoord coordinate{};
    ChunkIdentity identity{};
    std::uint64_t content_revision = 0;
    bool present = false;

    friend auto operator<=>(const ChunkDependencyRevision&,
                            const ChunkDependencyRevision&) = default;
};

struct ChunkNeighborhoodSnapshot {
    ChunkIdentity center_identity{};
    std::uint64_t center_revision = 0;
    std::uint16_t halo_radius = 0;
    std::uint16_t side_length = VoxelChunk::edge_length;
    std::vector<VoxelCell> cells;
    std::vector<ChunkDependencyRevision> dependencies;

    [[nodiscard]] VoxelCell cell(std::uint16_t x, std::uint16_t y, std::uint16_t z) const noexcept;
    [[nodiscard]] VoxelCell cell_relative(std::int32_t x, std::int32_t y,
                                          std::int32_t z) const noexcept;
    [[nodiscard]] std::size_t cell_count() const noexcept;
    [[nodiscard]] core::Status validate() const;
};

[[nodiscard]] core::Result<std::uint16_t>
required_chunk_halo(std::span<const VoxelCell> center_cells,
                    const BlockRenderTableSnapshot& render_table);

[[nodiscard]] core::Result<ChunkNeighborhoodSnapshot>
build_chunk_neighborhood_snapshot(const ChunkDatabase& chunks, ChunkIdentity center,
                                  const BlockRenderTableSnapshot& render_table,
                                  std::vector<VoxelCell> reusable_cells = {});

[[nodiscard]] bool
dependency_revisions_match(const ChunkDatabase& chunks,
                           std::span<const ChunkDependencyRevision> dependencies) noexcept;

} // namespace heartstead::world
