#include "engine/world/voxels/voxel_mask.hpp"

#include "engine/world/voxels/voxel_chunk.hpp"

#include <algorithm>
#include <bit>
#include <exception>

namespace heartstead::world {

namespace {

[[nodiscard]] constexpr bool contains(VoxelCoord coordinate) noexcept {
    return coordinate.x < chunk_edge_length && coordinate.y < chunk_edge_length &&
           coordinate.z < chunk_edge_length;
}

[[nodiscard]] constexpr std::size_t index_of(VoxelCoord coordinate) noexcept {
    constexpr auto edge = static_cast<std::size_t>(chunk_edge_length);
    return static_cast<std::size_t>(coordinate.z) * edge * edge +
           static_cast<std::size_t>(coordinate.y) * edge + coordinate.x;
}

} // namespace

bool VoxelOccupancyMask::occupied(std::size_t index) const noexcept {
    return index < cell_count && (words_[index / 64U] & (std::uint64_t{1} << (index % 64U))) != 0;
}

bool VoxelOccupancyMask::occupied(VoxelCoord coordinate) const noexcept {
    return contains(coordinate) && occupied(index_of(coordinate));
}

bool VoxelOccupancyMask::empty() const noexcept {
    return occupied_count_ == 0;
}

bool VoxelOccupancyMask::full() const noexcept {
    return occupied_count_ == cell_count;
}

std::size_t VoxelOccupancyMask::occupied_count() const noexcept {
    return occupied_count_;
}

std::uint64_t VoxelOccupancyMask::content_revision() const noexcept {
    return content_revision_;
}

std::span<const std::uint64_t, VoxelOccupancyMask::word_count>
VoxelOccupancyMask::words() const noexcept {
    return words_;
}

core::Status VoxelOccupancyMask::validate_against(std::span<const VoxelCell> cells,
                                                  std::uint64_t content_revision) const {
    if (cells.size() != cell_count || content_revision == 0 ||
        content_revision_ != content_revision) {
        return core::Status::failure(
            "voxel_mask.invalid_revision",
            "voxel occupancy mask does not match the cell count or content revision");
    }
    std::size_t counted = 0;
    for (std::size_t index = 0; index < cells.size(); ++index) {
        const bool expected = !cells[index].is_air();
        if (occupied(index) != expected) {
            return core::Status::failure(
                "voxel_mask.content_mismatch",
                "voxel occupancy mask bit does not match its corresponding cell");
        }
        counted += expected ? 1U : 0U;
    }
    std::size_t word_counted = 0;
    for (const auto word : words_) {
        word_counted += static_cast<std::size_t>(std::popcount(word));
    }
    if (counted != occupied_count_ || word_counted != occupied_count_) {
        return core::Status::failure("voxel_mask.count_mismatch",
                                     "voxel occupancy mask population count is inconsistent");
    }
    return core::Status::ok();
}

void VoxelOccupancyMask::set_occupied(std::size_t index, bool occupied_value) noexcept {
    if (index >= cell_count) {
        std::terminate();
    }
    const auto bit = std::uint64_t{1} << (index % 64U);
    auto& word = words_[index / 64U];
    const bool was_occupied = (word & bit) != 0;
    if (was_occupied == occupied_value) {
        return;
    }
    if (occupied_value) {
        word |= bit;
        ++occupied_count_;
    } else {
        word &= ~bit;
        --occupied_count_;
    }
}

void VoxelOccupancyMask::rebuild(std::span<const VoxelCell> cells) noexcept {
    if (cells.size() != cell_count) {
        std::terminate();
    }
    words_.fill(0);
    occupied_count_ = 0;
    for (std::size_t index = 0; index < cells.size(); ++index) {
        if (!cells[index].is_air()) {
            set_occupied(index, true);
        }
    }
}

void VoxelOccupancyMask::retag(std::uint64_t content_revision) noexcept {
    if (content_revision == 0) {
        std::terminate();
    }
    content_revision_ = content_revision;
}

} // namespace heartstead::world
