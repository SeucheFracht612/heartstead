#include "heartstead/voxel/chunk.hpp"

#include <algorithm>
#include <bit>

namespace heartstead {

Chunk::Chunk(BlockId fill_block) : uniform_block_(fill_block) {}

BlockId Chunk::get(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept {
    if (!in_bounds(x) || !in_bounds(y) || !in_bounds(z)) {
        return air_block;
    }
    return get(linear_index(x, y, z));
}

BlockId Chunk::get(std::size_t index) const noexcept {
    if (index >= volume) {
        return air_block;
    }

    switch (mode_) {
    case StorageMode::uniform:
        return uniform_block_;
    case StorageMode::palette:
        return palette_[palette_index(index)];
    case StorageMode::direct:
        return (*direct_)[index];
    }
    return air_block;
}

void Chunk::set(std::int32_t x, std::int32_t y, std::int32_t z, BlockId block) {
    if (in_bounds(x) && in_bounds(y) && in_bounds(z)) {
        set(linear_index(x, y, z), block);
    }
}

void Chunk::set(std::size_t index, BlockId block) {
    if (index >= volume || get(index) == block) {
        return;
    }

    if (mode_ == StorageMode::uniform) {
        promote_uniform(block);
    }

    if (mode_ == StorageMode::direct) {
        (*direct_)[index] = block;
        ++revision_;
        return;
    }

    const auto found = std::find(palette_.begin(), palette_.end(), block);
    std::uint16_t value{};
    if (found == palette_.end()) {
        if (palette_.size() >= direct_palette_threshold) {
            promote_direct();
            (*direct_)[index] = block;
            ++revision_;
            return;
        }

        value = static_cast<std::uint16_t>(palette_.size());
        palette_.push_back(block);
        const auto required_bits = bits_for(palette_.size());
        if (required_bits != bits_per_entry_) {
            repack(required_bits);
        }
    } else {
        value = static_cast<std::uint16_t>(std::distance(palette_.begin(), found));
    }

    write_palette_index(index, value);
    ++revision_;
}

void Chunk::fill(BlockId block) {
    if (mode_ == StorageMode::uniform && uniform_block_ == block) {
        return;
    }
    mode_ = StorageMode::uniform;
    uniform_block_ = block;
    bits_per_entry_ = 0;
    palette_.clear();
    packed_.clear();
    direct_.reset();
    ++revision_;
}

std::size_t Chunk::memory_bytes() const noexcept {
    return sizeof(Chunk) + palette_.capacity() * sizeof(BlockId) + packed_.capacity() * sizeof(std::uint64_t) +
        (direct_ ? sizeof(*direct_) : 0U);
}

std::uint16_t Chunk::palette_index(std::size_t index) const noexcept {
    const auto bit_position = index * bits_per_entry_;
    const auto word_index = bit_position / 64U;
    const auto offset = static_cast<std::uint8_t>(bit_position % 64U);
    const auto mask = (std::uint64_t{1} << bits_per_entry_) - 1U;
    auto value = packed_[word_index] >> offset;
    if (static_cast<std::uint16_t>(offset) + bits_per_entry_ > 64U) {
        value |= packed_[word_index + 1U] << (64U - offset);
    }
    return static_cast<std::uint16_t>(value & mask);
}

void Chunk::write_palette_index(std::size_t index, std::uint16_t value) noexcept {
    const auto bit_position = index * bits_per_entry_;
    const auto word_index = bit_position / 64U;
    const auto offset = static_cast<std::uint8_t>(bit_position % 64U);
    const auto value_mask = (std::uint64_t{1} << bits_per_entry_) - 1U;
    const auto masked_value = static_cast<std::uint64_t>(value) & value_mask;

    packed_[word_index] &= ~(value_mask << offset);
    packed_[word_index] |= masked_value << offset;

    if (static_cast<std::uint16_t>(offset) + bits_per_entry_ > 64U) {
        const auto high_bits = static_cast<std::uint8_t>(offset + bits_per_entry_ - 64U);
        const auto high_mask = (std::uint64_t{1} << high_bits) - 1U;
        packed_[word_index + 1U] &= ~high_mask;
        packed_[word_index + 1U] |= masked_value >> (64U - offset);
    }
}

void Chunk::promote_uniform(BlockId block) {
    mode_ = StorageMode::palette;
    palette_ = {uniform_block_, block};
    bits_per_entry_ = 1;
    packed_.assign((volume * bits_per_entry_ + 63U) / 64U, 0U);
}

void Chunk::repack(std::uint8_t new_bits) {
    std::vector<std::uint16_t> old_values;
    old_values.reserve(volume);
    for (std::size_t index = 0; index < volume; ++index) {
        old_values.push_back(palette_index(index));
    }

    bits_per_entry_ = new_bits;
    packed_.assign((volume * bits_per_entry_ + 63U) / 64U, 0U);
    for (std::size_t index = 0; index < volume; ++index) {
        write_palette_index(index, old_values[index]);
    }
}

void Chunk::promote_direct() {
    auto storage = std::make_unique<std::array<BlockId, volume>>();
    for (std::size_t index = 0; index < volume; ++index) {
        (*storage)[index] = palette_[palette_index(index)];
    }
    direct_ = std::move(storage);
    palette_.clear();
    packed_.clear();
    bits_per_entry_ = 0;
    mode_ = StorageMode::direct;
}

std::uint8_t Chunk::bits_for(std::size_t count) noexcept {
    return static_cast<std::uint8_t>(std::max<std::size_t>(1U, std::bit_width(count - 1U)));
}

} // namespace heartstead
