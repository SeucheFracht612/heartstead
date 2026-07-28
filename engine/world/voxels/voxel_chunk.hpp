#pragma once

#include "engine/core/result.hpp"
#include "engine/world/chunks/chunk_identity.hpp"
#include "engine/world/coords/world_coords.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace heartstead::world {

class ChunkDatabase;

struct VoxelCell {
    std::uint16_t type = 0;
    std::uint8_t light = 0;
    std::uint16_t state_bits = 0;
    std::uint32_t metadata_handle = 0;

    [[nodiscard]] static constexpr VoxelCell air() noexcept {
        return VoxelCell{};
    }

    [[nodiscard]] constexpr bool is_air() const noexcept {
        return type == 0;
    }

    friend auto operator<=>(const VoxelCell&, const VoxelCell&) = default;
};

enum class ChunkDirtyFlag : std::uint8_t {
    mesh = 1u << 0u,
    collision = 1u << 1u,
    lighting = 1u << 2u,
    save = 1u << 3u,
    replication = 1u << 4u,
};

class ChunkDirtyState {
  public:
    void mark(ChunkDirtyFlag flag) noexcept;
    void clear(ChunkDirtyFlag flag) noexcept;
    void clear_all() noexcept;

    [[nodiscard]] bool contains(ChunkDirtyFlag flag) const noexcept;
    [[nodiscard]] std::uint8_t bits() const noexcept;

  private:
    std::uint8_t bits_ = 0;
};

class VoxelChunk {
  public:
    static constexpr std::uint16_t edge_length = chunk_edge_length;
    static constexpr std::size_t total_cells =
        static_cast<std::size_t>(edge_length) * edge_length * edge_length;

    explicit VoxelChunk(ChunkCoord coord);

    [[nodiscard]] ChunkCoord coord() const noexcept;
    [[nodiscard]] ChunkIdentity identity() const noexcept;
    [[nodiscard]] std::uint64_t content_revision() const noexcept;
    [[nodiscard]] const ChunkDirtyState& dirty() const noexcept;
    [[nodiscard]] std::span<const VoxelCell> cells() const noexcept;
    [[nodiscard]] core::Result<VoxelCell> get(VoxelCoord coord) const;

    [[nodiscard]] core::Status set(VoxelCoord coord, VoxelCell cell);
    [[nodiscard]] core::Status apply_saved_cell(VoxelCoord coord, VoxelCell cell);
    [[nodiscard]] core::Result<std::size_t>
    apply_derived_light(std::span<const std::uint8_t> light);
    [[nodiscard]] core::Status load_generated_cells(std::vector<VoxelCell> cells);
    void fill(VoxelCell cell);
    void mark_dirty(ChunkDirtyFlag flag) noexcept;
    void clear_dirty(ChunkDirtyFlag flag) noexcept;
    void clear_all_dirty() noexcept;

  private:
    friend class ChunkDatabase;

    [[nodiscard]] static bool contains(VoxelCoord coord) noexcept;
    [[nodiscard]] static std::size_t index_of(VoxelCoord coord) noexcept;
    void assign_load_generation(std::uint64_t generation) noexcept;
    void advance_content_revision() noexcept;

    ChunkCoord coord_;
    std::vector<VoxelCell> cells_;
    ChunkDirtyState dirty_;
    std::uint64_t content_revision_ = 1;
    std::uint64_t load_generation_ = 0;
};

} // namespace heartstead::world
