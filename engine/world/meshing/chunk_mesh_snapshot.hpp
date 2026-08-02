#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/world/blocks/block_model.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <array>
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

struct ChunkMeshingMasks {
    static constexpr std::size_t center_word_count = VoxelOccupancyMask::word_count;

    std::uint64_t center_revision = 0;
    std::uint64_t render_table_revision = 0;
    std::uint16_t halo_radius = 0;
    std::uint16_t side_length = VoxelChunk::edge_length;
    std::size_t greedy_cube_count = 0;
    VoxelCoord greedy_minimum{VoxelChunk::edge_length, VoxelChunk::edge_length,
                              VoxelChunk::edge_length};
    VoxelCoord greedy_maximum{};
    bool has_directional_occluders = false;
    std::vector<std::uint64_t> words;

    [[nodiscard]] bool greedy_cube(std::size_t index) const noexcept;
    [[nodiscard]] bool greedy_cube(VoxelCoord coordinate) const noexcept;
    [[nodiscard]] std::uint32_t greedy_cube_x_row(std::uint16_t y, std::uint16_t z) const noexcept;
    [[nodiscard]] bool full_occluder_relative(std::int32_t x, std::int32_t y,
                                              std::int32_t z) const noexcept;
    [[nodiscard]] std::uint32_t full_occluder_x_row(std::int32_t x, std::int32_t y,
                                                    std::int32_t z) const noexcept;
    [[nodiscard]] std::span<const std::uint64_t> greedy_cube_words() const noexcept;
    [[nodiscard]] std::size_t payload_bytes() const noexcept;
    [[nodiscard]] std::size_t allocated_bytes() const noexcept;
    [[nodiscard]] core::Status validate(std::uint64_t expected_center_revision,
                                        std::uint16_t expected_halo_radius,
                                        std::uint16_t expected_side_length) const;
};

struct ChunkNeighborhoodSnapshot {
    ChunkIdentity center_identity{};
    std::uint64_t center_revision = 0;
    VoxelOccupancyMask center_occupancy;
    ChunkMeshingMasks meshing_masks;
    std::uint16_t halo_radius = 0;
    std::uint16_t side_length = VoxelChunk::edge_length;
    std::vector<VoxelCell> cells;
    std::vector<ChunkDependencyRevision> dependencies;

    [[nodiscard]] VoxelCell cell(std::uint16_t x, std::uint16_t y, std::uint16_t z) const noexcept;
    [[nodiscard]] VoxelCell cell_relative(std::int32_t x, std::int32_t y,
                                          std::int32_t z) const noexcept;
    [[nodiscard]] bool center_occupied(std::size_t index) const noexcept;
    [[nodiscard]] bool center_occupied(VoxelCoord coordinate) const noexcept;
    [[nodiscard]] std::size_t cell_count() const noexcept;
    [[nodiscard]] core::Status validate() const;
};

[[nodiscard]] core::Result<std::uint16_t>
required_chunk_halo(std::span<const VoxelCell> center_cells,
                    const BlockRenderTableSnapshot& render_table);

[[nodiscard]] core::Result<std::uint16_t>
required_chunk_halo(std::span<const VoxelCell> center_cells, const VoxelOccupancyMask& occupancy,
                    const BlockRenderTableSnapshot& render_table);

[[nodiscard]] core::Result<ChunkNeighborhoodSnapshot>
build_chunk_neighborhood_snapshot(const ChunkDatabase& chunks, ChunkIdentity center,
                                  const BlockRenderTableSnapshot& render_table,
                                  std::vector<VoxelCell> reusable_cells = {},
                                  std::vector<std::uint64_t> reusable_mask_words = {});

[[nodiscard]] bool
dependency_revisions_match(const ChunkDatabase& chunks,
                           std::span<const ChunkDependencyRevision> dependencies) noexcept;

} // namespace heartstead::world
