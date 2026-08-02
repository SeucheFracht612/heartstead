#pragma once

#include "heartstead/core/types.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace heartstead {

enum class BlockFlags : std::uint8_t {
    none = 0,
    renderable = 1U << 0U,
    occluding = 1U << 1U,
    solid = 1U << 2U,
};

[[nodiscard]] constexpr BlockFlags operator|(BlockFlags lhs, BlockFlags rhs) noexcept {
    return static_cast<BlockFlags>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr bool has_flag(BlockFlags value, BlockFlags flag) noexcept {
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0;
}

struct BlockDefinition {
    std::string_view name{"unknown"};
    BlockFlags flags{BlockFlags::none};
};

class BlockRegistry {
public:
    static constexpr std::size_t capacity = 256;

    constexpr BlockRegistry() noexcept {
        definitions_[air_block] = {"air", BlockFlags::none};
    }

    constexpr void set(BlockId id, BlockDefinition definition) noexcept {
        if (id < capacity) {
            definitions_[id] = definition;
        }
    }

    [[nodiscard]] constexpr const BlockDefinition& get(BlockId id) const noexcept {
        return definitions_[id < capacity ? id : air_block];
    }

    [[nodiscard]] constexpr bool is_renderable(BlockId id) const noexcept {
        return has_flag(get(id).flags, BlockFlags::renderable);
    }

    [[nodiscard]] constexpr bool is_occluding(BlockId id) const noexcept {
        return has_flag(get(id).flags, BlockFlags::occluding);
    }

    [[nodiscard]] constexpr bool is_solid(BlockId id) const noexcept {
        return has_flag(get(id).flags, BlockFlags::solid);
    }

    [[nodiscard]] static constexpr BlockRegistry defaults() noexcept {
        BlockRegistry registry;
        constexpr auto opaque = BlockFlags::renderable | BlockFlags::occluding | BlockFlags::solid;
        registry.set(1, {"stone", opaque});
        registry.set(2, {"dirt", opaque});
        registry.set(3, {"grass", opaque});
        registry.set(4, {"glass", BlockFlags::renderable | BlockFlags::solid});
        registry.set(5, {"oak_log", opaque});
        registry.set(6, {"oak_leaves", BlockFlags::renderable | BlockFlags::solid});
        registry.set(7, {"sand", opaque});
        registry.set(8, {"snow", opaque});
        return registry;
    }

private:
    std::array<BlockDefinition, capacity> definitions_{};
};

} // namespace heartstead
