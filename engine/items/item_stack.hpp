#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace heartstead::items {

struct ItemStack;

struct ItemDefinition {
    core::PrototypeId prototype_id;
    std::uint32_t stack_limit = 1;
    std::vector<std::string> tags;
    std::uint64_t mass_grams = 0;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] core::Result<ItemStack> create_stack(std::uint32_t count,
                                                       std::uint16_t quality = 100) const;
};

struct ItemStack {
    core::PrototypeId prototype_id;
    std::uint32_t count = 0;
    std::uint32_t max_count = 1;
    std::uint16_t quality = 100;

    [[nodiscard]] static core::Result<ItemStack> create(core::PrototypeId prototype_id,
                                                        std::uint32_t count,
                                                        std::uint32_t max_count,
                                                        std::uint16_t quality = 100);

    [[nodiscard]] bool is_empty() const noexcept;
    [[nodiscard]] std::uint32_t remaining_capacity() const noexcept;
    [[nodiscard]] bool can_merge_with(const ItemStack& other) const noexcept;
    [[nodiscard]] core::Status add_from(ItemStack& source, std::uint32_t requested_count);
    [[nodiscard]] core::Result<ItemStack> split(std::uint32_t requested_count);

    friend bool operator==(const ItemStack&, const ItemStack&) = default;
};

} // namespace heartstead::items
