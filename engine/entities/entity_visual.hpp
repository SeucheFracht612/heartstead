#pragma once

#include "engine/assets/asset_catalog.hpp"
#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/modding/generic_prototype.hpp"
#include "engine/modding/prototype_registry.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::entities {

struct EntityVisualDefinition {
    core::PrototypeId id;
    core::PrototypeId entity_prototype;
    std::string model_asset;
    std::unordered_map<std::string, std::string> animation_clips;
    std::unordered_map<std::string, core::PrototypeId> sound_events;
    float bounds_padding = 0.25F;
    std::uint32_t transition_ticks = 9;
    bool cast_shadow = true;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] const std::string* animation(std::string_view role) const noexcept;
    [[nodiscard]] const core::PrototypeId* sound(std::string_view role) const noexcept;
};

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

[[nodiscard]] core::Result<EntityVisualDefinition>
entity_visual_definition_from_prototype(const modding::GenericPrototype& prototype,
                                        const modding::PrototypeRegistry& prototypes,
                                        const assets::AssetCatalog& assets);
[[nodiscard]] core::Result<VisualDefinitionRegistry>
visual_definition_registry_from_prototypes(const modding::PrototypeRegistry& prototypes,
                                           const assets::AssetCatalog& assets);

} // namespace heartstead::entities
