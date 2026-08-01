#include "engine/items/equipment_loadout.hpp"

#include <algorithm>
#include <ranges>

namespace heartstead::items {
namespace {

[[nodiscard]] bool valid_name(const std::string_view name) noexcept {
    return !name.empty() && name.size() <= 128U &&
           std::ranges::all_of(name, [](const char value) {
               return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                      (value >= '0' && value <= '9') || value == '_' || value == '-';
           });
}

} // namespace

core::Status EquipmentLoadout::equip(EquipmentLoadoutEntry entry) {
    if (!valid_name(entry.slot) || !valid_name(entry.visual_variant)) {
        return core::Status::failure(
            "equipment_loadout.invalid_entry",
            "equipment slot and visual variant require bounded identifier names");
    }
    const auto found = std::ranges::lower_bound(entries_, entry.slot, {},
                                                &EquipmentLoadoutEntry::slot);
    if (found != entries_.end() && found->slot == entry.slot) {
        if (*found == entry) {
            return core::Status::ok();
        }
        *found = std::move(entry);
    } else {
        entries_.insert(found, std::move(entry));
    }
    ++revision_;
    return core::Status::ok();
}

core::Status EquipmentLoadout::unequip(const std::string_view slot) {
    const auto found = std::ranges::lower_bound(entries_, slot, {}, &EquipmentLoadoutEntry::slot);
    if (found == entries_.end() || found->slot != slot) {
        return core::Status::failure("equipment_loadout.missing_slot",
                                     "cannot unequip an empty equipment slot");
    }
    entries_.erase(found);
    ++revision_;
    return core::Status::ok();
}

core::Status EquipmentLoadout::set_stowed(const std::string_view slot, const bool stowed) {
    const auto found = std::ranges::lower_bound(entries_, slot, {}, &EquipmentLoadoutEntry::slot);
    if (found == entries_.end() || found->slot != slot) {
        return core::Status::failure("equipment_loadout.missing_slot",
                                     "cannot stow an empty equipment slot");
    }
    if (found->stowed != stowed) {
        found->stowed = stowed;
        ++revision_;
    }
    return core::Status::ok();
}

const EquipmentLoadoutEntry* EquipmentLoadout::find(const std::string_view slot) const noexcept {
    const auto found = std::ranges::lower_bound(entries_, slot, {}, &EquipmentLoadoutEntry::slot);
    return found == entries_.end() || found->slot != slot ? nullptr : &*found;
}

std::span<const EquipmentLoadoutEntry> EquipmentLoadout::entries() const noexcept {
    return entries_;
}

std::uint64_t EquipmentLoadout::revision() const noexcept {
    return revision_;
}

void EquipmentLoadout::clear() noexcept {
    if (!entries_.empty()) {
        entries_.clear();
        ++revision_;
    }
}

} // namespace heartstead::items
