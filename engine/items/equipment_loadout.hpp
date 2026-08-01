#pragma once

#include "engine/core/result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::items {

struct EquipmentLoadoutEntry {
    std::string slot;
    std::string visual_variant;
    bool stowed{false};

    [[nodiscard]] bool operator==(const EquipmentLoadoutEntry&) const = default;
};

class EquipmentLoadout final {
  public:
    [[nodiscard]] core::Status equip(EquipmentLoadoutEntry entry);
    [[nodiscard]] core::Status unequip(std::string_view slot);
    [[nodiscard]] core::Status set_stowed(std::string_view slot, bool stowed);
    [[nodiscard]] const EquipmentLoadoutEntry* find(std::string_view slot) const noexcept;
    [[nodiscard]] std::span<const EquipmentLoadoutEntry> entries() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    void clear() noexcept;

  private:
    std::vector<EquipmentLoadoutEntry> entries_;
    std::uint64_t revision_{0};
};

} // namespace heartstead::items
