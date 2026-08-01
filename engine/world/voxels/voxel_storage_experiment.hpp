#pragma once

#include "engine/core/result.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace heartstead::world::benchmark {

enum class VoxelCorpusKind : std::uint8_t {
    empty,
    uniform_solid,
    layered_terrain,
    sparse_caves,
    lit_settlement,
    checkerboard,
    high_entropy,
};

[[nodiscard]] std::string_view voxel_corpus_name(VoxelCorpusKind kind) noexcept;

struct VoxelCorpusConfig {
    VoxelCorpusKind kind = VoxelCorpusKind::layered_terrain;
    std::uint16_t edge_length = VoxelChunk::edge_length;
    std::uint16_t material_count = 32;
    std::uint64_t seed = 1;

    [[nodiscard]] core::Status validate() const;
};

struct VoxelCorpusStats {
    std::size_t cell_count = 0;
    std::size_t non_air_count = 0;
    std::size_t unique_cell_count = 0;
    std::size_t unique_block_value_count = 0;
    std::size_t metadata_cell_count = 0;
};

struct VoxelStorageCorpus {
    VoxelCorpusConfig config;
    std::vector<VoxelCell> cells;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] VoxelCorpusStats stats() const;
};

[[nodiscard]] core::Result<VoxelStorageCorpus>
generate_voxel_storage_corpus(VoxelCorpusConfig config);

struct ExperimentalBlockValue {
    std::uint16_t type = 0;
    std::uint16_t state_bits = 0;

    friend auto operator<=>(const ExperimentalBlockValue&, const ExperimentalBlockValue&) = default;
};

struct VoxelSectionStorageStats {
    std::size_t cell_count = 0;
    std::size_t payload_bytes = 0;
    std::size_t allocated_bytes = 0;
    std::size_t palette_size = 0;
    std::size_t packed_index_bytes = 0;
    std::size_t light_bytes = 0;
    std::size_t metadata_entry_count = 0;
    std::uint8_t bits_per_index = 0;
    bool uniform_light = false;
};

class SplitVoxelSectionExperiment {
  public:
    [[nodiscard]] static core::Result<SplitVoxelSectionExperiment>
    encode(std::span<const VoxelCell> cells, std::uint16_t edge_length);

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] VoxelCell cell(std::size_t index) const noexcept;
    [[nodiscard]] core::Status set(std::size_t index, VoxelCell cell);
    [[nodiscard]] std::vector<VoxelCell> decode() const;
    [[nodiscard]] std::uint64_t scan_type_checksum() const noexcept;
    [[nodiscard]] VoxelSectionStorageStats stats() const noexcept;

  private:
    std::uint16_t edge_length_ = 0;
    std::vector<std::uint16_t> types_;
    std::vector<std::uint8_t> lights_;
    std::vector<std::uint16_t> states_;
    std::vector<std::uint32_t> metadata_;
};

class PalettePackedVoxelSectionExperiment {
  public:
    [[nodiscard]] static core::Result<PalettePackedVoxelSectionExperiment>
    encode(std::span<const VoxelCell> cells, std::uint16_t edge_length);

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] VoxelCell cell(std::size_t index) const noexcept;
    [[nodiscard]] core::Status set(std::size_t index, VoxelCell cell);
    [[nodiscard]] std::vector<VoxelCell> decode() const;
    [[nodiscard]] std::uint64_t scan_type_checksum() const noexcept;
    [[nodiscard]] VoxelSectionStorageStats stats() const noexcept;

  private:
    [[nodiscard]] std::uint32_t packed_index(std::size_t index) const noexcept;
    void set_packed_index(std::size_t index, std::uint32_t value) noexcept;
    void repack_indices(std::uint8_t new_width);
    [[nodiscard]] std::uint32_t find_or_append_block(ExperimentalBlockValue block);
    void set_light(std::size_t index, std::uint8_t light);
    void set_metadata(std::size_t index, std::uint32_t handle);

    std::uint16_t edge_length_ = 0;
    std::size_t cell_count_ = 0;
    std::vector<ExperimentalBlockValue> palette_;
    std::vector<std::uint64_t> packed_indices_;
    std::vector<std::uint8_t> lights_;
    std::vector<std::uint32_t> metadata_indices_;
    std::vector<std::uint32_t> metadata_handles_;
    std::uint8_t bits_per_index_ = 0;
    std::uint8_t uniform_light_ = 0;
    bool light_is_uniform_ = true;
};

} // namespace heartstead::world::benchmark
