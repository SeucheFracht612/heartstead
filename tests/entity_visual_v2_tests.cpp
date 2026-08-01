#include "engine/entities/entity_visual.hpp"

#include <array>
#include <cassert>

int main() {
    using namespace heartstead;

    entities::EntityVisualDefinition definition;
    definition.model_asset = "base:model";

    entities::VisualStateRule active;
    active.channel = "activity";
    active.value = "active";
    active.animation_clip = "work";
    active.priority = 10;
    active.group_visibility.emplace("moving", true);

    entities::VisualStateRule damaged;
    damaged.channel = "damage";
    damaged.value = "damaged";
    damaged.model_asset = "base:damaged";
    damaged.priority = 20;
    damaged.group_visibility.emplace("broken", true);
    const auto damaged_material = *core::PrototypeId::parse("base:materials/damaged");
    damaged.material_overrides.push_back({"body", damaged_material});

    entities::VisualStateRule cold;
    cold.channel = "heat";
    cold.value = "cold";
    cold.priority = 5;
    cold.group_visibility.emplace("moving", false);

    definition.state_rules = {active, damaged, cold};
    const std::array states{
        entities::VisualStateValue{.channel = "activity", .value = "active"},
        entities::VisualStateValue{.channel = "damage", .value = "damaged"},
        entities::VisualStateValue{.channel = "heat", .value = "cold"},
    };

    const auto matches = definition.resolve_state_rules(states);
    assert(matches.size() == 3);
    assert(matches[0]->channel == "heat");
    assert(matches[2]->channel == "damage");
    assert(definition.resolve_model(states) == "base:damaged");
    assert(definition.resolve_animation_state_rule(states) == &definition.state_rules[0]);
    assert(definition.resolve_group_visibility(states, "moving", true));
    assert(definition.resolve_group_visibility(states, "broken", false));
    assert(definition.resolve_material_override(states, "body") != nullptr);
    assert(*definition.resolve_material_override(states, "body") == damaged_material);

    entities::VisualEquipmentVariantDefinition equipment;
    equipment.slot = "main_hand";
    equipment.variant = "hammer";
    equipment.model_asset = "base:hammer";
    equipment.socket = "hand_r";
    const auto equipment_material = *core::PrototypeId::parse("base:materials/iron");
    equipment.material_overrides.push_back({"ForgedIron", equipment_material});
    definition.equipment_variants.push_back(equipment);
    assert(definition.equipment_variant("main_hand", "hammer") ==
           &definition.equipment_variants.front());
    assert(definition.equipment_variant("off_hand", "hammer") == nullptr);
    assert(definition.equipment_variant("main_hand", "hammer")->material_overrides.front().material ==
           equipment_material);
}
