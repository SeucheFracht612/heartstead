#pragma once

#include "heartstead/core/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace heartstead {

class Chunk {
public:
    static constexpr std::int32_t edge = 32;
    static constexpr std::size_t volume = static_cast<std::size_t>(edge) * edge * edge;
    static constexpr std::size_t direct_palette_threshold = 256;

    enum class StorageMode : std::uint8_t { uniform, palette, direct };

    explicit Chunk(BlockId fill = air_block);

    [[nodiscard]] BlockId get(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept;
    [[nodiscard]] BlockId get(std::size_t index) const noexcept;
    void set(std::int32_t x, std::int32_t y, std::int32_t z, BlockId block);
    void set(std::size_t index, BlockId block);
    void fill(BlockId block);

    [[nodiscard]] StorageMode storage_mode() const noexcept { return mode_; }
    [[nodiscard]] std::size_t palette_size() const noexcept { return palette_.size(); }
    [[nodiscard]] std::size_t memory_bytes() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    [[nodiscard]] static constexpr bool in_bounds(std::int32_t value) noexcept {
        return static_cast<std::uint32_t>(value) < static_cast<std::uint32_t>(edge);
    }

    [[nodiscard]] static constexpr std::size_t linear_index(
        std::int32_t x, std::int32_t y, std::int32_t z) noexcept {
        return static_cast<std::size_t>(x) + static_cast<std::size_t>(edge) *
            (static_cast<std::size_t>(z) + static_cast<std::size_t>(edge) * static_cast<std::size_t>(y));
    }

private:
    [[nodiscard]] std::uint16_t palette_index(std::size_t index) const noexcept;
    void write_palette_index(std::size_t index, std::uint16_t value) noexcept;
    void promote_uniform(BlockId block);
    void repack(std::uint8_t new_bits);
    void promote_direct();
    [[nodiscard]] static std::uint8_t bits_for(std::size_t count) noexcept;

    StorageMode mode_{StorageMode::uniform};
    BlockId uniform_block_{air_block};
    std::uint8_t bits_per_entry_{0};
    std::vector<BlockId> palette_;
    std::vector<std::uint64_t> packed_;
    std::unique_ptr<std::array<BlockId, volume>> direct_;
    std::uint64_t revision_{0};
};

} // namespace heartstead

