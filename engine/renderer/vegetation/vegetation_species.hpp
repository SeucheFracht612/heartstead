#pragma once

#include "engine/assets/asset_catalog.hpp"
#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/modding/prototype_registry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer {

enum class VegetationKind : std::uint8_t {
    grass,
    flower,
    crop,
    bush,
    forage,
    tree,
    fallen_tree,
    vine,
    kelp,
    magical_plant,
};

[[nodiscard]] std::string_view vegetation_kind_name(VegetationKind kind) noexcept;
[[nodiscard]] core::Result<VegetationKind> parse_vegetation_kind(std::string_view name);

struct VegetationLod {
    std::string model_asset;
    float maximum_distance = 64.0F;
    float transition_width = 4.0F;
    float density = 1.0F;
    bool impostor = false;

    [[nodiscard]] core::Status validate() const;
};

struct VegetationGrowthState {
    std::string name;
    float scale_multiplier = 1.0F;
    std::string model_override;

    [[nodiscard]] core::Status validate() const;
};

struct VegetationSpecies {
    core::PrototypeId id;
    std::string display_name;
    VegetationKind kind = VegetationKind::grass;
    std::vector<VegetationLod> lods;
    std::vector<VegetationGrowthState> growth_states;
    float scale_min = 0.9F;
    float scale_max = 1.1F;
    float yaw_variation_degrees = 360.0F;
    bool mirror_variation = false;
    std::array<float, 4> color_min{0.9F, 0.9F, 0.9F, 1.0F};
    std::array<float, 4> color_max{1.0F, 1.0F, 1.0F, 1.0F};
    float wind_stiffness = 0.25F;
    float foliage_transmission = 0.35F;
    float density_fade_start = 48.0F;
    float density_fade_end = 96.0F;
    std::uint32_t shadow_lod = 0;
    bool receives_weather = true;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] const VegetationGrowthState* growth_state(std::string_view name) const noexcept;
};

class VegetationSpeciesRegistry {
  public:
    [[nodiscard]] core::Status add(VegetationSpecies species);
    [[nodiscard]] const VegetationSpecies*
    find(const core::PrototypeId& id) const noexcept;
    [[nodiscard]] std::span<const VegetationSpecies> species() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    std::vector<VegetationSpecies> species_;
};

[[nodiscard]] core::Result<VegetationSpecies>
vegetation_species_from_generic(const modding::GenericPrototype& prototype);
[[nodiscard]] core::Result<VegetationSpeciesRegistry>
vegetation_species_registry_from_prototypes(const modding::PrototypeRegistry& prototypes,
                                            const assets::AssetCatalog& assets);

} // namespace heartstead::renderer
