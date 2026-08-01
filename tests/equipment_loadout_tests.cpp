#include "engine/items/equipment_loadout.hpp"

#include <cassert>

int main() {
    using namespace heartstead::items;

    EquipmentLoadout loadout;
    assert(loadout.equip({.slot = "off_hand", .visual_variant = "shield", .stowed = false}));
    assert(loadout.equip({.slot = "main_hand", .visual_variant = "hammer", .stowed = false}));
    assert(loadout.entries().size() == 2);
    assert(loadout.entries()[0].slot == "main_hand");
    assert(loadout.entries()[1].slot == "off_hand");
    const auto revision = loadout.revision();
    assert(loadout.equip({.slot = "main_hand", .visual_variant = "hammer", .stowed = false}));
    assert(loadout.revision() == revision);
    assert(loadout.set_stowed("main_hand", true));
    assert(loadout.find("main_hand")->stowed);
    assert(loadout.unequip("off_hand"));
    assert(loadout.find("off_hand") == nullptr);
    assert(!loadout.equip({.slot = "bad slot", .visual_variant = "hammer", .stowed = false}));
    assert(!loadout.unequip("empty"));
}
