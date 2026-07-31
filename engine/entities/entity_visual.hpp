#pragma once

#include "engine/assets/asset_catalog.hpp"
#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/modding/generic_prototype.hpp"
#include "engine/modding/prototype_registry.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::entities {

enum class VisualShadowPolicy : std::uint8_t {
    none,
    cast,
};

enum class VisualAnchorKind : std::uint8_t {
    equipment,
    effect,
    light,
};

struct VisualLodDefinition {
    std::uint32_t level = 0;
    std::string model_asset;
    float minimum_distance = 0.0F;
    float maximum_distance = 0.0F;

    [[nodiscard]] bool has_unbounded_maximum() const noexcept {
        return maximum_distance == 0.0F;
    }

    friend bool operator==(const VisualLodDefinition&, const VisualLodDefinition&) = default;
};

struct VisualMaterialOverride {
    std::string slot;
    core::PrototypeId material;

    friend bool operator==(const VisualMaterialOverride&, const VisualMaterialOverride&) = default;
};

struct VisualAnchor {
    std::string name;
    VisualAnchorKind kind = VisualAnchorKind::equipment;
    std::string socket;

    friend bool operator==(const VisualAnchor&, const VisualAnchor&) = default;
};

struct VisualVisibilityGroup {
    std::string name;
    std::vector<std::string> nodes;

    friend bool operator==(const VisualVisibilityGroup&, const VisualVisibilityGroup&) = default;
};

struct VisualStateValue {
    std::string channel;
    std::string value;

    friend bool operator==(const VisualStateValue&, const VisualStateValue&) = default;
};

struct VisualStateRule {
    std::string channel;
    std::string value;
    std::int32_t priority = 0;
    std::string model_asset;
    std::string animation_clip;
    std::unordered_map<std::string, bool> group_visibility;
    std::vector<VisualMaterialOverride> material_overrides;

    friend bool operator==(const VisualStateRule&, const VisualStateRule&) = default;
};

struct VisualPreviewSettings {
    std::string lighting_preset = "studio";
    float camera_distance = 4.0F;
    float yaw_degrees = 30.0F;
    float pitch_degrees = -15.0F;
    std::unordered_map<std::string, std::string> states;

    friend bool operator==(const VisualPreviewSettings&, const VisualPreviewSettings&) = default;
};

struct VisualImpostorData {
    std::string model_asset;
    float start_distance = 0.0F;
    std::uint32_t view_count = 0;

    [[nodiscard]] bool enabled() const noexcept {
        return !model_asset.empty();
    }

    friend bool operator==(const VisualImpostorData&, const VisualImpostorData&) = default;
};

struct EntityVisualDefinition {
    core::PrototypeId id;
    core::PrototypeId entity_prototype;
    std::string model_asset;
    std::unordered_map<std::string, std::string> animation_clips;
    std::unordered_map<std::string, core::PrototypeId> sound_events;
    std::vector<VisualLodDefinition> lods;
    std::vector<VisualMaterialOverride> material_overrides;
    std::unordered_map<std::string, std::string> socket_aliases;
    std::vector<VisualAnchor> anchors;
    std::vector<VisualVisibilityGroup> visibility_groups;
    std::vector<VisualStateRule> state_rules;
    std::unordered_map<std::string, std::uint32_t> animation_transitions;
    std::optional<math::Bounds3f> bounds_override;
    VisualPreviewSettings preview;
    VisualImpostorData impostor;
    VisualShadowPolicy shadow_policy = VisualShadowPolicy::cast;
    float model_scale = 1.0F;
    float bounds_padding = 0.25F;
    std::uint32_t transition_ticks = 9;
    bool cast_shadow = true;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] const std::string* animation(std::string_view role) const noexcept;
    [[nodiscard]] const core::PrototypeId* sound(std::string_view role) const noexcept;
    [[nodiscard]] const VisualVisibilityGroup*
    visibility_group(std::string_view name) const noexcept;
    [[nodiscard]] const VisualStateRule*
    resolve_state_rule(std::span<const VisualStateValue> states) const noexcept;
    [[nodiscard]] std::string_view
    resolve_model(std::span<const VisualStateValue> states) const noexcept;
};

using VisualPrefabDefinition = EntityVisualDefinition;

class VisualDefinitionRegistry {
  public:
    [[nodiscard]] core::Status add(EntityVisualDefinition definition);
    [[nodiscard]] const EntityVisualDefinition*
    find(const core::PrototypeId& visual_id) const noexcept;
    [[nodiscard]] const EntityVisualDefinition*
    find_for_entity(const core::PrototypeId& entity_prototype) const noexcept;
    [[nodiscard]] const std::vector<EntityVisualDefinition>& definitions() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    std::vector<EntityVisualDefinition> definitions_;
    std::unordered_map<std::string, std::size_t> by_id_;
    std::unordered_map<std::string, std::size_t> by_entity_;
};

using VisualPrefabRegistry = VisualDefinitionRegistry;

[[nodiscard]] core::Result<EntityVisualDefinition>
entity_visual_definition_from_prototype(const modding::GenericPrototype& prototype,
                                        const modding::PrototypeRegistry& prototypes,
                                        const assets::AssetCatalog& assets);
[[nodiscard]] core::Result<VisualDefinitionRegistry>
visual_definition_registry_from_prototypes(const modding::PrototypeRegistry& prototypes,
                                           const assets::AssetCatalog& assets);

} // namespace heartstead::entities
