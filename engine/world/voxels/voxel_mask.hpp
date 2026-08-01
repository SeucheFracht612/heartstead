#pragma once

#include "engine/core/result.hpp"
#include "engine/world/coords/world_coords.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace heartstead::world {

struct VoxelCell;

class VoxelOccupancyMask {
  public:
    static constexpr std::size_t cell_count =
        static_cast<std::size_t>(chunk_edge_length) * chunk_edge_length * chunk_edge_length;
    static constexpr std::size_t word_count = (cell_count + 63U) / 64U;
    static constexpr std::size_t payload_bytes = word_count * sizeof(std::uint64_t);

    [[nodiscard]] bool occupied(std::size_t index) const noexcept;
    [[nodiscard]] bool occupied(VoxelCoord coordinate) const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool full() const noexcept;
    [[nodiscard]] std::size_t occupied_count() const noexcept;
    [[nodiscard]] std::uint64_t content_revision() const noexcept;
    [[nodiscard]] std::span<const std::uint64_t, word_count> words() const noexcept;
    [[nodiscard]] core::Status validate_against(std::span<const VoxelCell> cells,
                                                std::uint64_t content_revision) const;

  private:
    friend class VoxelChunk;

    void set_occupied(std::size_t index, bool occupied) noexcept;
    void rebuild(std::span<const VoxelCell> cells) noexcept;
    void retag(std::uint64_t content_revision) noexcept;

    std::array<std::uint64_t, word_count> words_{};
    std::size_t occupied_count_ = 0;
    std::uint64_t content_revision_ = 1;
};

} // namespace heartstead::world
