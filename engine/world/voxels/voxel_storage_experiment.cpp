#include "engine/world/voxels/voxel_storage_experiment.hpp"

#include <algorithm>
#include <bit>
#include <exception>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>

namespace heartstead::world::benchmark {

namespace {

constexpr std::uint16_t maximum_experiment_edge = 32;

[[nodiscard]] core::Result<std::size_t> checked_cell_count(std::uint16_t edge_length) {
    if (edge_length == 0 || edge_length > maximum_experiment_edge) {
        return core::Result<std::size_t>::failure(
            "voxel_storage.invalid_edge",
            "voxel storage experiments require an edge length between one and 32");
    }
    const auto edge = static_cast<std::size_t>(edge_length);
    return core::Result<std::size_t>::success(edge * edge * edge);
}

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::uint64_t cell_random(std::uint64_t seed, std::size_t index,
                                        std::uint64_t stream = 0) noexcept {
    return splitmix64(seed ^ (static_cast<std::uint64_t>(index) * 0x9e3779b97f4a7c15ULL) ^
                      (stream * 0xd6e8feb86659fd93ULL));
}

[[nodiscard]] std::size_t cell_index(std::uint16_t edge, std::uint16_t x, std::uint16_t y,
                                     std::uint16_t z) noexcept {
    const auto side = static_cast<std::size_t>(edge);
    return static_cast<std::size_t>(z) * side * side + static_cast<std::size_t>(y) * side + x;
}

[[nodiscard]] std::uint16_t material(std::uint64_t random, std::uint16_t material_count) noexcept {
    return static_cast<std::uint16_t>((random % material_count) + 1U);
}

[[nodiscard]] std::uint32_t block_key(ExperimentalBlockValue block) noexcept {
    return static_cast<std::uint32_t>(block.type) |
           (static_cast<std::uint32_t>(block.state_bits) << 16U);
}

[[nodiscard]] std::uint8_t required_index_bits(std::size_t palette_size) noexcept {
    return palette_size <= 1
               ? 0
               : static_cast<std::uint8_t>(std::bit_width(palette_size - std::size_t{1}));
}

[[nodiscard]] std::size_t packed_word_count(std::size_t cell_count, std::uint8_t width) noexcept {
    return width == 0 ? 0 : (cell_count * width + 63U) / 64U;
}

[[nodiscard]] std::uint64_t width_mask(std::uint8_t width) noexcept {
    return width == 0 ? 0 : (std::uint64_t{1} << width) - 1U;
}

[[nodiscard]] std::uint32_t read_packed(std::span<const std::uint64_t> words, std::size_t index,
                                        std::uint8_t width) noexcept {
    if (width == 0) {
        return 0;
    }
    const auto bit_offset = index * width;
    const auto word_index = bit_offset / 64U;
    const auto shift = static_cast<std::uint8_t>(bit_offset % 64U);
    std::uint64_t value = words[word_index] >> shift;
    if (static_cast<unsigned>(shift) + width > 64U) {
        value |= words[word_index + 1] << (64U - shift);
    }
    return static_cast<std::uint32_t>(value & width_mask(width));
}

void write_packed(std::span<std::uint64_t> words, std::size_t index, std::uint8_t width,
                  std::uint32_t value) noexcept {
    if (width == 0) {
        return;
    }
    const auto bit_offset = index * width;
    const auto word_index = bit_offset / 64U;
    const auto shift = static_cast<std::uint8_t>(bit_offset % 64U);
    const auto mask = width_mask(width);
    const auto encoded = static_cast<std::uint64_t>(value) & mask;
    words[word_index] =
        (words[word_index] & ~(mask << shift)) | static_cast<std::uint64_t>(encoded << shift);
    if (static_cast<unsigned>(shift) + width > 64U) {
        const auto spill = static_cast<std::uint8_t>(static_cast<unsigned>(shift) + width - 64U);
        const auto spill_mask = width_mask(spill);
        words[word_index + 1] =
            (words[word_index + 1] & ~spill_mask) | ((encoded >> (64U - shift)) & spill_mask);
    }
}

} // namespace

std::string_view voxel_corpus_name(VoxelCorpusKind kind) noexcept {
    switch (kind) {
    case VoxelCorpusKind::empty:
        return "empty";
    case VoxelCorpusKind::uniform_solid:
        return "uniform_solid";
    case VoxelCorpusKind::layered_terrain:
        return "layered_terrain";
    case VoxelCorpusKind::sparse_caves:
        return "sparse_caves";
    case VoxelCorpusKind::lit_settlement:
        return "lit_settlement";
    case VoxelCorpusKind::checkerboard:
        return "checkerboard";
    case VoxelCorpusKind::high_entropy:
        return "high_entropy";
    }
    return "unknown";
}

core::Status VoxelCorpusConfig::validate() const {
    if (!checked_cell_count(edge_length)) {
        return core::Status::failure("voxel_storage.invalid_edge",
                                     "voxel corpus edge length must be between one and 32");
    }
    if (material_count == 0 || material_count == std::numeric_limits<std::uint16_t>::max()) {
        return core::Status::failure(
            "voxel_storage.invalid_material_count",
            "voxel corpus material count must leave type zero reserved for air");
    }
    switch (kind) {
    case VoxelCorpusKind::empty:
    case VoxelCorpusKind::uniform_solid:
    case VoxelCorpusKind::layered_terrain:
    case VoxelCorpusKind::sparse_caves:
    case VoxelCorpusKind::lit_settlement:
    case VoxelCorpusKind::checkerboard:
    case VoxelCorpusKind::high_entropy:
        return core::Status::ok();
    }
    return core::Status::failure("voxel_storage.invalid_corpus_kind",
                                 "voxel corpus kind is not recognized");
}

core::Status VoxelStorageCorpus::validate() const {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    const auto count = checked_cell_count(config.edge_length);
    if (!count || cells.size() != count.value()) {
        return core::Status::failure("voxel_storage.invalid_corpus_cells",
                                     "voxel corpus cell count does not match its edge length");
    }
    return core::Status::ok();
}

VoxelCorpusStats VoxelStorageCorpus::stats() const {
    VoxelCorpusStats result;
    result.cell_count = cells.size();
    std::set<VoxelCell> unique_cells;
    std::set<ExperimentalBlockValue> unique_blocks;
    for (const auto cell : cells) {
        result.non_air_count += cell.is_air() ? 0U : 1U;
        result.metadata_cell_count += cell.metadata_handle == 0 ? 0U : 1U;
        unique_cells.insert(cell);
        unique_blocks.insert({cell.type, cell.state_bits});
    }
    result.unique_cell_count = unique_cells.size();
    result.unique_block_value_count = unique_blocks.size();
    return result;
}

core::Result<VoxelStorageCorpus> generate_voxel_storage_corpus(VoxelCorpusConfig config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<VoxelStorageCorpus>::failure(status.error().code,
                                                         status.error().message);
    }
    const auto count = checked_cell_count(config.edge_length).value();
    VoxelStorageCorpus result;
    result.config = config;
    result.cells.resize(count);
    const auto edge = config.edge_length;
    for (std::uint16_t z = 0; z < edge; ++z) {
        for (std::uint16_t y = 0; y < edge; ++y) {
            for (std::uint16_t x = 0; x < edge; ++x) {
                const auto index = cell_index(edge, x, y, z);
                const auto random = cell_random(config.seed, index);
                auto& cell = result.cells[index];
                switch (config.kind) {
                case VoxelCorpusKind::empty:
                    break;
                case VoxelCorpusKind::uniform_solid:
                    cell.type = 1;
                    break;
                case VoxelCorpusKind::layered_terrain: {
                    const auto surface = static_cast<std::uint16_t>(edge * 5U / 8U);
                    if (y > surface) {
                        cell.light = std::numeric_limits<std::uint8_t>::max();
                        break;
                    }
                    const auto layer = static_cast<std::uint16_t>((y / 3U) % 4U);
                    cell.type = static_cast<std::uint16_t>(1U + layer % config.material_count);
                    cell.state_bits = y == surface ? 1U : 0U;
                    if (index % 4'093U == 0) {
                        cell.metadata_handle = static_cast<std::uint32_t>(index + 1U);
                    }
                    break;
                }
                case VoxelCorpusKind::sparse_caves: {
                    const bool above_ground = y > static_cast<std::uint16_t>(edge * 3U / 4U);
                    const bool cave = !above_ground && random % 100U < 17U;
                    if (above_ground || cave) {
                        cell.light = above_ground ? std::numeric_limits<std::uint8_t>::max() : 0;
                        break;
                    }
                    cell.type =
                        material(random >> 8U, std::min<std::uint16_t>(config.material_count, 8U));
                    cell.state_bits = static_cast<std::uint16_t>((random >> 24U) & 3U);
                    break;
                }
                case VoxelCorpusKind::lit_settlement: {
                    const bool floor = y < edge / 4U;
                    const bool wall = (x == edge / 4U || x + edge / 4U == edge - 1U ||
                                       z == edge / 4U || z + edge / 4U == edge - 1U) &&
                                      y < edge * 3U / 4U;
                    const bool beam = x % 7U == 0 && z % 7U == 0 && y < edge * 3U / 4U;
                    if (floor || wall || beam) {
                        cell.type = material(random >> 16U,
                                             std::min<std::uint16_t>(config.material_count, 12U));
                        cell.state_bits = static_cast<std::uint16_t>((x + z) & 3U);
                        if (random % 1'009U == 0) {
                            cell.metadata_handle = static_cast<std::uint32_t>(index + 1U);
                        }
                    } else {
                        cell.light = static_cast<std::uint8_t>(128U + (random & 127U));
                    }
                    break;
                }
                case VoxelCorpusKind::checkerboard:
                    if (((x + y + z) & 1U) == 0) {
                        cell.type =
                            material(random, std::min<std::uint16_t>(config.material_count, 16U));
                        cell.state_bits = static_cast<std::uint16_t>((random >> 12U) & 7U);
                    } else {
                        cell.light = static_cast<std::uint8_t>(random & 0xffU);
                    }
                    break;
                case VoxelCorpusKind::high_entropy:
                    cell.type = material(random, config.material_count);
                    cell.light = static_cast<std::uint8_t>(random >> 16U);
                    cell.state_bits = static_cast<std::uint16_t>((random >> 24U) & 0x0fffU);
                    if ((random >> 40U) % 16U == 0) {
                        cell.metadata_handle =
                            static_cast<std::uint32_t>((random >> 32U) | std::uint64_t{1});
                    }
                    break;
                }
            }
        }
    }
    return core::Result<VoxelStorageCorpus>::success(std::move(result));
}

core::Result<SplitVoxelSectionExperiment>
SplitVoxelSectionExperiment::encode(std::span<const VoxelCell> cells, std::uint16_t edge_length) {
    const auto count = checked_cell_count(edge_length);
    if (!count || cells.size() != count.value()) {
        return core::Result<SplitVoxelSectionExperiment>::failure(
            "voxel_storage.invalid_split_input",
            "split voxel section input does not match its edge length");
    }
    SplitVoxelSectionExperiment result;
    result.edge_length_ = edge_length;
    result.types_.reserve(cells.size());
    result.lights_.reserve(cells.size());
    result.states_.reserve(cells.size());
    result.metadata_.reserve(cells.size());
    for (const auto cell : cells) {
        result.types_.push_back(cell.type);
        result.lights_.push_back(cell.light);
        result.states_.push_back(cell.state_bits);
        result.metadata_.push_back(cell.metadata_handle);
    }
    return core::Result<SplitVoxelSectionExperiment>::success(std::move(result));
}

core::Status SplitVoxelSectionExperiment::validate() const {
    const auto count = checked_cell_count(edge_length_);
    if (!count || types_.size() != count.value() || lights_.size() != count.value() ||
        states_.size() != count.value() || metadata_.size() != count.value()) {
        return core::Status::failure("voxel_storage.invalid_split_section",
                                     "split voxel section channels have inconsistent sizes");
    }
    return core::Status::ok();
}

VoxelCell SplitVoxelSectionExperiment::cell(std::size_t index) const noexcept {
    if (index >= types_.size()) {
        std::terminate();
    }
    return {types_[index], lights_[index], states_[index], metadata_[index]};
}

core::Status SplitVoxelSectionExperiment::set(std::size_t index, VoxelCell value) {
    if (index >= types_.size()) {
        return core::Status::failure("voxel_storage.split_index_out_of_bounds",
                                     "split voxel section edit index is outside the section");
    }
    types_[index] = value.type;
    lights_[index] = value.light;
    states_[index] = value.state_bits;
    metadata_[index] = value.metadata_handle;
    return core::Status::ok();
}

std::vector<VoxelCell> SplitVoxelSectionExperiment::decode() const {
    std::vector<VoxelCell> result;
    result.reserve(types_.size());
    for (std::size_t index = 0; index < types_.size(); ++index) {
        result.push_back(cell(index));
    }
    return result;
}

std::uint64_t SplitVoxelSectionExperiment::scan_type_checksum() const noexcept {
    std::uint64_t result = 0;
    for (const auto type : types_) {
        result = (result * 1'099'511'628'211ULL) ^ type;
    }
    return result;
}

VoxelSectionStorageStats SplitVoxelSectionExperiment::stats() const noexcept {
    VoxelSectionStorageStats result;
    result.cell_count = types_.size();
    result.payload_bytes =
        types_.size() * sizeof(types_.front()) + lights_.size() * sizeof(lights_.front()) +
        states_.size() * sizeof(states_.front()) + metadata_.size() * sizeof(metadata_.front());
    result.allocated_bytes = sizeof(*this) + types_.capacity() * sizeof(types_.front()) +
                             lights_.capacity() * sizeof(lights_.front()) +
                             states_.capacity() * sizeof(states_.front()) +
                             metadata_.capacity() * sizeof(metadata_.front());
    result.light_bytes = lights_.size();
    result.metadata_entry_count = static_cast<std::size_t>(
        std::ranges::count_if(metadata_, [](auto value) { return value != 0; }));
    return result;
}

core::Result<PalettePackedVoxelSectionExperiment>
PalettePackedVoxelSectionExperiment::encode(std::span<const VoxelCell> cells,
                                            std::uint16_t edge_length) {
    const auto count = checked_cell_count(edge_length);
    if (!count || cells.size() != count.value()) {
        return core::Result<PalettePackedVoxelSectionExperiment>::failure(
            "voxel_storage.invalid_palette_input",
            "palette-packed voxel section input does not match its edge length");
    }
    PalettePackedVoxelSectionExperiment result;
    result.edge_length_ = edge_length;
    result.cell_count_ = cells.size();
    std::unordered_map<std::uint32_t, std::uint32_t> palette_lookup;
    palette_lookup.reserve(cells.size());
    for (const auto cell : cells) {
        const ExperimentalBlockValue block{cell.type, cell.state_bits};
        const auto [entry, inserted] = palette_lookup.try_emplace(
            block_key(block), static_cast<std::uint32_t>(result.palette_.size()));
        static_cast<void>(entry);
        if (inserted) {
            result.palette_.push_back(block);
        }
    }
    result.bits_per_index_ = required_index_bits(result.palette_.size());
    result.packed_indices_.resize(packed_word_count(cells.size(), result.bits_per_index_));
    result.uniform_light_ = cells.front().light;
    result.light_is_uniform_ = std::ranges::all_of(
        cells, [&result](VoxelCell cell) { return cell.light == result.uniform_light_; });
    if (!result.light_is_uniform_) {
        result.lights_.reserve(cells.size());
    }
    for (std::size_t index = 0; index < cells.size(); ++index) {
        const ExperimentalBlockValue block{cells[index].type, cells[index].state_bits};
        const auto found = palette_lookup.find(block_key(block));
        if (found == palette_lookup.end()) {
            std::terminate();
        }
        const auto palette_index = found->second;
        result.set_packed_index(index, palette_index);
        if (!result.light_is_uniform_) {
            result.lights_.push_back(cells[index].light);
        }
        if (cells[index].metadata_handle != 0) {
            result.metadata_indices_.push_back(static_cast<std::uint32_t>(index));
            result.metadata_handles_.push_back(cells[index].metadata_handle);
        }
    }
    return core::Result<PalettePackedVoxelSectionExperiment>::success(std::move(result));
}

core::Status PalettePackedVoxelSectionExperiment::validate() const {
    const auto count = checked_cell_count(edge_length_);
    if (!count || cell_count_ != count.value() || palette_.empty() ||
        bits_per_index_ != required_index_bits(palette_.size()) ||
        packed_indices_.size() != packed_word_count(cell_count_, bits_per_index_) ||
        (!light_is_uniform_ && lights_.size() != cell_count_) ||
        (light_is_uniform_ && !lights_.empty()) ||
        metadata_indices_.size() != metadata_handles_.size()) {
        return core::Status::failure(
            "voxel_storage.invalid_palette_section",
            "palette-packed voxel section channels or metadata are inconsistent");
    }
    for (std::size_t index = 0; index < cell_count_; ++index) {
        if (packed_index(index) >= palette_.size()) {
            return core::Status::failure("voxel_storage.invalid_palette_index",
                                         "palette-packed voxel section contains an invalid index");
        }
    }
    std::uint32_t previous = 0;
    bool has_previous = false;
    for (std::size_t index = 0; index < metadata_indices_.size(); ++index) {
        if (metadata_indices_[index] >= cell_count_ || metadata_handles_[index] == 0 ||
            (has_previous && metadata_indices_[index] <= previous)) {
            return core::Status::failure(
                "voxel_storage.invalid_sparse_metadata",
                "palette-packed voxel metadata must be nonzero, in range, and sorted");
        }
        previous = metadata_indices_[index];
        has_previous = true;
    }
    return core::Status::ok();
}

VoxelCell PalettePackedVoxelSectionExperiment::cell(std::size_t index) const noexcept {
    if (index >= cell_count_) {
        std::terminate();
    }
    const auto block = palette_[packed_index(index)];
    const auto light = light_is_uniform_ ? uniform_light_ : lights_[index];
    std::uint32_t metadata = 0;
    const auto found =
        std::ranges::lower_bound(metadata_indices_, static_cast<std::uint32_t>(index));
    if (found != metadata_indices_.end() && *found == static_cast<std::uint32_t>(index)) {
        metadata = metadata_handles_[static_cast<std::size_t>(found - metadata_indices_.begin())];
    }
    return {block.type, light, block.state_bits, metadata};
}

core::Status PalettePackedVoxelSectionExperiment::set(std::size_t index, VoxelCell value) {
    if (index >= cell_count_) {
        return core::Status::failure("voxel_storage.palette_index_out_of_bounds",
                                     "palette-packed voxel edit index is outside the section");
    }
    const auto palette_index = find_or_append_block({value.type, value.state_bits});
    set_packed_index(index, palette_index);
    set_light(index, value.light);
    set_metadata(index, value.metadata_handle);
    return core::Status::ok();
}

std::vector<VoxelCell> PalettePackedVoxelSectionExperiment::decode() const {
    std::vector<VoxelCell> result;
    result.reserve(cell_count_);
    for (std::size_t index = 0; index < cell_count_; ++index) {
        result.push_back(cell(index));
    }
    return result;
}

std::uint64_t PalettePackedVoxelSectionExperiment::scan_type_checksum() const noexcept {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < cell_count_; ++index) {
        result = (result * 1'099'511'628'211ULL) ^ palette_[packed_index(index)].type;
    }
    return result;
}

VoxelSectionStorageStats PalettePackedVoxelSectionExperiment::stats() const noexcept {
    VoxelSectionStorageStats result;
    result.cell_count = cell_count_;
    result.palette_size = palette_.size();
    result.bits_per_index = bits_per_index_;
    result.packed_index_bytes = packed_indices_.size() * sizeof(packed_indices_.front());
    result.light_bytes = light_is_uniform_ ? sizeof(uniform_light_) : lights_.size();
    result.metadata_entry_count = metadata_indices_.size();
    result.uniform_light = light_is_uniform_;
    result.payload_bytes = palette_.size() * sizeof(palette_.front()) + result.packed_index_bytes +
                           result.light_bytes +
                           metadata_indices_.size() * sizeof(metadata_indices_.front()) +
                           metadata_handles_.size() * sizeof(metadata_handles_.front());
    result.allocated_bytes = sizeof(*this) + palette_.capacity() * sizeof(palette_.front()) +
                             packed_indices_.capacity() * sizeof(packed_indices_.front()) +
                             lights_.capacity() * sizeof(lights_.front()) +
                             metadata_indices_.capacity() * sizeof(metadata_indices_.front()) +
                             metadata_handles_.capacity() * sizeof(metadata_handles_.front());
    return result;
}

std::uint32_t PalettePackedVoxelSectionExperiment::packed_index(std::size_t index) const noexcept {
    return read_packed(packed_indices_, index, bits_per_index_);
}

void PalettePackedVoxelSectionExperiment::set_packed_index(std::size_t index,
                                                           std::uint32_t value) noexcept {
    write_packed(packed_indices_, index, bits_per_index_, value);
}

void PalettePackedVoxelSectionExperiment::repack_indices(std::uint8_t new_width) {
    std::vector<std::uint64_t> replacement(packed_word_count(cell_count_, new_width));
    for (std::size_t index = 0; index < cell_count_; ++index) {
        write_packed(replacement, index, new_width, packed_index(index));
    }
    packed_indices_ = std::move(replacement);
    bits_per_index_ = new_width;
}

std::uint32_t
PalettePackedVoxelSectionExperiment::find_or_append_block(ExperimentalBlockValue block) {
    const auto found = std::ranges::find(palette_, block);
    if (found != palette_.end()) {
        return static_cast<std::uint32_t>(found - palette_.begin());
    }
    palette_.push_back(block);
    const auto required_width = required_index_bits(palette_.size());
    if (required_width != bits_per_index_) {
        repack_indices(required_width);
    }
    return static_cast<std::uint32_t>(palette_.size() - 1U);
}

void PalettePackedVoxelSectionExperiment::set_light(std::size_t index, std::uint8_t light) {
    if (light_is_uniform_ && light == uniform_light_) {
        return;
    }
    if (light_is_uniform_) {
        lights_.assign(cell_count_, uniform_light_);
        light_is_uniform_ = false;
    }
    lights_[index] = light;
}

void PalettePackedVoxelSectionExperiment::set_metadata(std::size_t index, std::uint32_t handle) {
    const auto encoded_index = static_cast<std::uint32_t>(index);
    const auto found = std::ranges::lower_bound(metadata_indices_, encoded_index);
    const auto offset = static_cast<std::size_t>(found - metadata_indices_.begin());
    if (found != metadata_indices_.end() && *found == encoded_index) {
        if (handle == 0) {
            metadata_indices_.erase(found);
            metadata_handles_.erase(metadata_handles_.begin() +
                                    static_cast<std::ptrdiff_t>(offset));
        } else {
            metadata_handles_[offset] = handle;
        }
        return;
    }
    if (handle == 0) {
        return;
    }
    metadata_indices_.insert(found, encoded_index);
    metadata_handles_.insert(metadata_handles_.begin() + static_cast<std::ptrdiff_t>(offset),
                             handle);
}

} // namespace heartstead::world::benchmark
