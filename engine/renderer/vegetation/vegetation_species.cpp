#include "engine/renderer/vegetation/vegetation_species.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace heartstead::renderer {

namespace {

[[nodiscard]] const std::string* field(const modding::GenericPrototype& prototype,
                                       std::string_view key) {
    const auto found = prototype.fields.find(std::string(key));
    return found == prototype.fields.end() ? nullptr : &found->second;
}

template <typename Value>
[[nodiscard]] core::Result<Value> number_field(const modding::GenericPrototype& prototype,
                                               std::string_view key, Value fallback) {
    const auto* text = field(prototype, key);
    if (text == nullptr) {
        return core::Result<Value>::success(fallback);
    }
    Value value{};
    const auto [end, error] =
        std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc{} || end != text->data() + text->size() ||
        (std::is_floating_point_v<Value> && !std::isfinite(value))) {
        return core::Result<Value>::failure("vegetation_species.invalid_number",
                                            std::string(key) + " must be a finite number");
    }
    return core::Result<Value>::success(value);
}

[[nodiscard]] core::Result<bool> bool_field(const modding::GenericPrototype& prototype,
                                            std::string_view key, bool fallback) {
    const auto* text = field(prototype, key);
    if (text == nullptr) {
        return core::Result<bool>::success(fallback);
    }
    if (*text == "true") {
        return core::Result<bool>::success(true);
    }
    if (*text == "false") {
        return core::Result<bool>::success(false);
    }
    return core::Result<bool>::failure("vegetation_species.invalid_bool",
                                       std::string(key) + " must be true or false");
}

[[nodiscard]] core::Result<std::array<float, 4>>
color_field(const modding::GenericPrototype& prototype, std::string_view key,
            std::array<float, 4> fallback) {
    const auto* text = field(prototype, key);
    if (text == nullptr) {
        return core::Result<std::array<float, 4>>::success(fallback);
    }
    std::array<float, 4> result{};
    std::string_view remaining = *text;
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto separator = remaining.find(',');
        const auto token = remaining.substr(0, separator);
        const auto [end, error] =
            std::from_chars(token.data(), token.data() + token.size(), result[index]);
        if (error != std::errc{} || end != token.data() + token.size() ||
            !std::isfinite(result[index]) ||
            ((separator == std::string_view::npos) != (index + 1U == result.size()))) {
            return core::Result<std::array<float, 4>>::failure(
                "vegetation_species.invalid_color",
                std::string(key) + " must contain four comma-separated finite values");
        }
        if (separator != std::string_view::npos) {
            remaining.remove_prefix(separator + 1U);
        }
    }
    return core::Result<std::array<float, 4>>::success(result);
}

[[nodiscard]] std::vector<std::string> list_field(const modding::GenericPrototype& prototype,
                                                  std::string_view key) {
    const auto* text = field(prototype, key);
    if (text == nullptr || text->empty()) {
        return {};
    }
    std::vector<std::string> result;
    std::string_view remaining = *text;
    while (!remaining.empty()) {
        const auto separator = remaining.find(',');
        const auto token = remaining.substr(0, separator);
        if (!token.empty()) {
            result.emplace_back(token);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1U);
    }
    return result;
}

[[nodiscard]] bool finite_color(const std::array<float, 4>& color) noexcept {
    return std::ranges::all_of(color, [](float component) {
        return std::isfinite(component) && component >= 0.0F && component <= 4.0F;
    });
}

[[nodiscard]] std::string lod_key(std::size_t index, std::string_view suffix) {
    return "lod." + std::to_string(index) + "." + std::string(suffix);
}

[[nodiscard]] core::Status validate_asset(const assets::AssetCatalog& catalog,
                                          std::string_view logical_id,
                                          std::string_view owner) {
    const auto* record = catalog.find_active(logical_id);
    if (record == nullptr || record->kind != assets::AssetKind::model) {
        return core::Status::failure(
            "vegetation_species.missing_model",
            std::string(owner) + " references a missing non-model asset: " +
                std::string(logical_id));
    }
    return core::Status::ok();
}

} // namespace

std::string_view vegetation_kind_name(VegetationKind kind) noexcept {
    switch (kind) {
    case VegetationKind::grass:
        return "grass";
    case VegetationKind::flower:
        return "flower";
    case VegetationKind::crop:
        return "crop";
    case VegetationKind::bush:
        return "bush";
    case VegetationKind::forage:
        return "forage";
    case VegetationKind::tree:
        return "tree";
    case VegetationKind::fallen_tree:
        return "fallen_tree";
    case VegetationKind::vine:
        return "vine";
    case VegetationKind::kelp:
        return "kelp";
    case VegetationKind::magical_plant:
        return "magical_plant";
    }
    return "grass";
}

core::Result<VegetationKind> parse_vegetation_kind(std::string_view name) {
    static constexpr std::array values{
        VegetationKind::grass,       VegetationKind::flower,
        VegetationKind::crop,        VegetationKind::bush,
        VegetationKind::forage,      VegetationKind::tree,
        VegetationKind::fallen_tree, VegetationKind::vine,
        VegetationKind::kelp,        VegetationKind::magical_plant,
    };
    const auto found =
        std::ranges::find_if(values, [name](auto value) { return vegetation_kind_name(value) == name; });
    if (found == values.end()) {
        return core::Result<VegetationKind>::failure(
            "vegetation_species.invalid_kind",
            "vegetation_kind must be grass, flower, crop, bush, forage, tree, fallen_tree, vine, "
            "kelp, or magical_plant");
    }
    return core::Result<VegetationKind>::success(*found);
}

core::Status VegetationLod::validate() const {
    if (model_asset.empty() || !std::isfinite(maximum_distance) || maximum_distance <= 0.0F ||
        !std::isfinite(transition_width) || transition_width < 0.0F ||
        transition_width >= maximum_distance || !std::isfinite(density) || density <= 0.0F ||
        density > 1.0F) {
        return core::Status::failure(
            "vegetation_species.invalid_lod",
            "vegetation LOD requires a model, positive range, bounded transition, and density");
    }
    return core::Status::ok();
}

core::Status VegetationGrowthState::validate() const {
    if (name.empty() || !std::isfinite(scale_multiplier) || scale_multiplier <= 0.0F ||
        scale_multiplier > 8.0F) {
        return core::Status::failure(
            "vegetation_species.invalid_growth_state",
            "vegetation growth state requires a name and a finite positive scale");
    }
    return core::Status::ok();
}

core::Status VegetationSpecies::validate() const {
    if (!id.is_valid() || display_name.empty() || lods.empty() || !std::isfinite(scale_min) ||
        !std::isfinite(scale_max) || scale_min <= 0.0F || scale_max < scale_min ||
        scale_max > 16.0F || !std::isfinite(yaw_variation_degrees) ||
        yaw_variation_degrees < 0.0F || yaw_variation_degrees > 360.0F ||
        !finite_color(color_min) || !finite_color(color_max) ||
        !std::isfinite(wind_stiffness) || wind_stiffness < 0.0F || wind_stiffness > 1.0F ||
        !std::isfinite(foliage_transmission) || foliage_transmission < 0.0F ||
        foliage_transmission > 4.0F || !std::isfinite(density_fade_start) ||
        !std::isfinite(density_fade_end) || density_fade_start < 0.0F ||
        density_fade_end < density_fade_start || shadow_lod >= lods.size()) {
        return core::Status::failure("vegetation_species.invalid_definition",
                                     "vegetation species contains invalid material, variation, "
                                     "wind, density, or shadow settings");
    }
    float previous_distance = 0.0F;
    for (const auto& lod : lods) {
        auto status = lod.validate();
        if (!status) {
            return status;
        }
        if (lod.maximum_distance <= previous_distance) {
            return core::Status::failure(
                "vegetation_species.unsorted_lods",
                "vegetation LOD maximum distances must be strictly increasing");
        }
        previous_distance = lod.maximum_distance;
    }
    std::unordered_set<std::string> names;
    for (const auto& growth : growth_states) {
        auto status = growth.validate();
        if (!status) {
            return status;
        }
        if (!names.insert(growth.name).second) {
            return core::Status::failure("vegetation_species.duplicate_growth_state",
                                         "vegetation growth state names must be unique");
        }
    }
    return core::Status::ok();
}

const VegetationGrowthState*
VegetationSpecies::growth_state(std::string_view name) const noexcept {
    const auto found =
        std::ranges::find_if(growth_states, [name](const auto& state) { return state.name == name; });
    return found == growth_states.end() ? nullptr : &*found;
}

core::Status VegetationSpeciesRegistry::add(VegetationSpecies species) {
    auto status = species.validate();
    if (!status) {
        return status;
    }
    if (find(species.id) != nullptr) {
        return core::Status::failure("vegetation_species.duplicate_id",
                                     "vegetation species id is already registered: " +
                                         species.id.value());
    }
    species_.push_back(std::move(species));
    std::ranges::sort(species_, {}, [](const auto& value) { return value.id.value(); });
    return core::Status::ok();
}

const VegetationSpecies*
VegetationSpeciesRegistry::find(const core::PrototypeId& id) const noexcept {
    const auto found = std::ranges::lower_bound(
        species_, id.value(), {}, [](const auto& value) { return value.id.value(); });
    return found == species_.end() || found->id != id ? nullptr : &*found;
}

std::span<const VegetationSpecies> VegetationSpeciesRegistry::species() const noexcept {
    return species_;
}

std::size_t VegetationSpeciesRegistry::size() const noexcept {
    return species_.size();
}

core::Result<VegetationSpecies>
vegetation_species_from_generic(const modding::GenericPrototype& prototype) {
    if (prototype.kind != modding::PrototypeKinds::vegetation_species) {
        return core::Result<VegetationSpecies>::failure(
            "vegetation_species.invalid_prototype_kind",
            "prototype kind must be vegetation_species");
    }
    const auto* kind_text = field(prototype, "vegetation_kind");
    auto kind = parse_vegetation_kind(kind_text == nullptr ? "grass" : *kind_text);
    auto scale_min = number_field<float>(prototype, "variation.scale_min", 0.9F);
    auto scale_max = number_field<float>(prototype, "variation.scale_max", 1.1F);
    auto yaw = number_field<float>(prototype, "variation.yaw_degrees", 360.0F);
    auto mirror = bool_field(prototype, "variation.mirror", false);
    auto color_min =
        color_field(prototype, "variation.color_min", {0.9F, 0.9F, 0.9F, 1.0F});
    auto color_max =
        color_field(prototype, "variation.color_max", {1.0F, 1.0F, 1.0F, 1.0F});
    auto stiffness = number_field<float>(prototype, "wind.stiffness", 0.25F);
    auto transmission = number_field<float>(prototype, "foliage.transmission", 0.35F);
    auto density_start = number_field<float>(prototype, "density.fade_start", 48.0F);
    auto density_end = number_field<float>(prototype, "density.fade_end", 96.0F);
    auto shadow_lod = number_field<std::uint32_t>(prototype, "shadow.lod", 0);
    auto alpha_to_coverage = bool_field(prototype, "alpha_to_coverage", true);
    auto receives_weather = bool_field(prototype, "receives_weather", true);
    if (!kind || !scale_min || !scale_max || !yaw || !mirror || !color_min || !color_max ||
        !stiffness || !transmission || !density_start || !density_end || !shadow_lod ||
        !alpha_to_coverage || !receives_weather) {
        return core::Result<VegetationSpecies>::failure(
            "vegetation_species.invalid_fields",
            "vegetation species contains an invalid kind, variation, wind, density, or shadow "
            "field");
    }

    VegetationSpecies result;
    result.id = prototype.id;
    result.display_name = field(prototype, "display_name") == nullptr
                              ? prototype.id.value()
                              : *field(prototype, "display_name");
    result.kind = kind.value();
    result.scale_min = scale_min.value();
    result.scale_max = scale_max.value();
    result.yaw_variation_degrees = yaw.value();
    result.mirror_variation = mirror.value();
    result.color_min = color_min.value();
    result.color_max = color_max.value();
    result.wind_stiffness = stiffness.value();
    result.foliage_transmission = transmission.value();
    result.density_fade_start = density_start.value();
    result.density_fade_end = density_end.value();
    result.shadow_lod = shadow_lod.value();
    result.alpha_to_coverage = alpha_to_coverage.value();
    result.receives_weather = receives_weather.value();

    for (std::size_t index = 0;; ++index) {
        const auto model = field(prototype, lod_key(index, "model"));
        if (model == nullptr) {
            break;
        }
        VegetationLod lod;
        lod.model_asset = *model;
        auto maximum =
            number_field<float>(prototype, lod_key(index, "maximum_distance"), 64.0F);
        auto transition =
            number_field<float>(prototype, lod_key(index, "transition_width"), 4.0F);
        auto density = number_field<float>(prototype, lod_key(index, "density"), 1.0F);
        auto impostor = bool_field(prototype, lod_key(index, "impostor"), false);
        if (!maximum || !transition || !density || !impostor) {
            return core::Result<VegetationSpecies>::failure(
                "vegetation_species.invalid_lod_fields",
                "vegetation LOD contains invalid range, transition, density, or impostor fields");
        }
        lod.maximum_distance = maximum.value();
        lod.transition_width = transition.value();
        lod.density = density.value();
        lod.impostor = impostor.value();
        result.lods.push_back(std::move(lod));
    }

    for (const auto& name : list_field(prototype, "growth.states")) {
        VegetationGrowthState state;
        state.name = name;
        const auto prefix = "growth." + name + ".";
        auto scale = number_field<float>(prototype, prefix + "scale", 1.0F);
        if (!scale) {
            return core::Result<VegetationSpecies>::failure(scale.error().code,
                                                            scale.error().message);
        }
        state.scale_multiplier = scale.value();
        if (const auto* model = field(prototype, prefix + "model"); model != nullptr) {
            state.model_override = *model;
        }
        result.growth_states.push_back(std::move(state));
    }

    auto status = result.validate();
    if (!status) {
        return core::Result<VegetationSpecies>::failure(status.error().code,
                                                        status.error().message);
    }
    return core::Result<VegetationSpecies>::success(std::move(result));
}

core::Result<VegetationSpeciesRegistry>
vegetation_species_registry_from_prototypes(const modding::PrototypeRegistry& prototypes,
                                            const assets::AssetCatalog& assets) {
    VegetationSpeciesRegistry result;
    for (const auto* prototype :
         prototypes.prototypes_of_kind(modding::PrototypeKinds::vegetation_species)) {
        auto parsed = vegetation_species_from_generic(*prototype);
        if (!parsed) {
            return core::Result<VegetationSpeciesRegistry>::failure(
                parsed.error().code,
                prototype->source.generic_string() + ": " + parsed.error().message);
        }
        for (const auto& lod : parsed.value().lods) {
            auto status = validate_asset(assets, lod.model_asset, parsed.value().id.value());
            if (!status) {
                return core::Result<VegetationSpeciesRegistry>::failure(
                    status.error().code,
                    prototype->source.generic_string() + ": " + status.error().message);
            }
        }
        for (const auto& growth : parsed.value().growth_states) {
            if (growth.model_override.empty()) {
                continue;
            }
            auto status =
                validate_asset(assets, growth.model_override, parsed.value().id.value());
            if (!status) {
                return core::Result<VegetationSpeciesRegistry>::failure(
                    status.error().code,
                    prototype->source.generic_string() + ": " + status.error().message);
            }
        }
        auto status = result.add(std::move(parsed).value());
        if (!status) {
            return core::Result<VegetationSpeciesRegistry>::failure(status.error().code,
                                                                    status.error().message);
        }
    }
    return core::Result<VegetationSpeciesRegistry>::success(std::move(result));
}

} // namespace heartstead::renderer
