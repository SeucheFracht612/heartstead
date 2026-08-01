#include "engine/scenarios/scenario_prototype.hpp"

#include "engine/modding/prototype_registry.hpp"

#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace heartstead::scenarios {

namespace {

[[nodiscard]] const std::string* field(const modding::GenericPrototype& prototype,
                                       std::string_view key) {
    const auto found = prototype.fields.find(std::string(key));
    return found == prototype.fields.end() ? nullptr : &found->second;
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view value, char delimiter) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(delimiter, start);
        if (end == std::string_view::npos) {
            result.push_back(value.substr(start));
            break;
        }
        result.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

[[nodiscard]] core::Result<std::vector<core::PrototypeId>>
parse_prototype_list(const modding::GenericPrototype& prototype, std::string_view key) {
    const auto* value = field(prototype, key);
    std::vector<core::PrototypeId> ids;
    if (value == nullptr || value->empty()) {
        return core::Result<std::vector<core::PrototypeId>>::success(std::move(ids));
    }

    for (const auto entry : split(*value, ',')) {
        auto parsed = core::PrototypeId::parse(entry);
        if (!parsed) {
            return core::Result<std::vector<core::PrototypeId>>::failure(
                "scenario_prototype.invalid_reference",
                std::string(key) + " contains invalid prototype id: " + std::string(entry));
        }
        ids.push_back(std::move(parsed).value());
    }
    return core::Result<std::vector<core::PrototypeId>>::success(std::move(ids));
}

[[nodiscard]] core::Result<std::vector<std::string>>
parse_tags(const modding::GenericPrototype& prototype) {
    const auto* value = field(prototype, "tags");
    std::vector<std::string> tags;
    if (value == nullptr || value->empty()) {
        return core::Result<std::vector<std::string>>::success(std::move(tags));
    }

    for (const auto tag : split(*value, ',')) {
        if (!core::is_valid_local_id(tag)) {
            return core::Result<std::vector<std::string>>::failure(
                "scenario_prototype.invalid_tag",
                "tags contains invalid scenario tag: " + std::string(tag));
        }
        tags.emplace_back(tag);
    }
    return core::Result<std::vector<std::string>>::success(std::move(tags));
}

[[nodiscard]] std::vector<std::string> parse_string_list(const modding::GenericPrototype& prototype,
                                                         std::string_view key) {
    const auto* value = field(prototype, key);
    std::vector<std::string> result;
    if (value == nullptr || value->empty()) {
        return result;
    }
    for (const auto entry : split(*value, ',')) {
        result.emplace_back(entry);
    }
    return result;
}

[[nodiscard]] core::Result<bool> parse_bool_field(const modding::GenericPrototype& prototype,
                                                  std::string_view key,
                                                  bool default_value = false) {
    const auto* value = field(prototype, key);
    if (value == nullptr || value->empty()) {
        return core::Result<bool>::success(default_value);
    }
    if (*value == "true") {
        return core::Result<bool>::success(true);
    }
    if (*value == "false") {
        return core::Result<bool>::success(false);
    }
    return core::Result<bool>::failure("scenario_prototype.invalid_boolean",
                                       std::string(key) + " must be true or false");
}

[[nodiscard]] core::Result<std::optional<std::uint64_t>>
parse_optional_u64(const modding::GenericPrototype& prototype, std::string_view key) {
    const auto* value = field(prototype, key);
    if (value == nullptr || value->empty()) {
        return core::Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), parsed);
    if (error != std::errc{} || end != value->data() + value->size()) {
        return core::Result<std::optional<std::uint64_t>>::failure(
            "scenario_prototype.invalid_unsigned_integer",
            std::string(key) + " must be an unsigned integer");
    }
    return core::Result<std::optional<std::uint64_t>>::success(parsed);
}

[[nodiscard]] core::Result<double> parse_finite_double(std::string_view value,
                                                       std::string_view label) {
    double parsed = 0.0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(parsed)) {
        return core::Result<double>::failure("scenario_prototype.invalid_scene_entity",
                                             std::string(label) + " must be a finite number");
    }
    return core::Result<double>::success(parsed);
}

[[nodiscard]] core::Result<std::vector<ScenarioEntityPlacement>>
parse_scene_entities(const modding::GenericPrototype& prototype) {
    const auto* value = field(prototype, "scene_entities");
    std::vector<ScenarioEntityPlacement> placements;
    if (value == nullptr || value->empty()) {
        return core::Result<std::vector<ScenarioEntityPlacement>>::success(std::move(placements));
    }

    for (const auto encoded : split(*value, ';')) {
        const auto separator = encoded.find('@');
        if (separator == std::string_view::npos || separator == 0 ||
            separator + 1 >= encoded.size()) {
            return core::Result<std::vector<ScenarioEntityPlacement>>::failure(
                "scenario_prototype.invalid_scene_entity",
                "scene_entities entries use prototype@x:y:z:yaw:scale");
        }
        auto prototype_id = core::PrototypeId::parse(encoded.substr(0, separator));
        const auto values = split(encoded.substr(separator + 1), ':');
        if (!prototype_id || values.size() != 5) {
            return core::Result<std::vector<ScenarioEntityPlacement>>::failure(
                "scenario_prototype.invalid_scene_entity",
                "scene_entities entries use prototype@x:y:z:yaw:scale");
        }
        auto x = parse_finite_double(values[0], "scene entity x");
        auto y = parse_finite_double(values[1], "scene entity y");
        auto z = parse_finite_double(values[2], "scene entity z");
        auto yaw = parse_finite_double(values[3], "scene entity yaw");
        auto scale = parse_finite_double(values[4], "scene entity scale");
        if (!x || !y || !z || !yaw || !scale) {
            const auto& error = !x     ? x.error()
                                : !y   ? y.error()
                                : !z   ? z.error()
                                : !yaw ? yaw.error()
                                       : scale.error();
            return core::Result<std::vector<ScenarioEntityPlacement>>::failure(error.code,
                                                                               error.message);
        }
        ScenarioEntityPlacement placement;
        placement.prototype_id = std::move(prototype_id).value();
        placement.transform.position = {x.value(), y.value(), z.value()};
        placement.transform.rotation_degrees.y = yaw.value();
        placement.transform.scale = {scale.value(), scale.value(), scale.value()};
        auto status = placement.validate();
        if (!status) {
            return core::Result<std::vector<ScenarioEntityPlacement>>::failure(
                status.error().code, status.error().message);
        }
        placements.push_back(std::move(placement));
    }
    return core::Result<std::vector<ScenarioEntityPlacement>>::success(std::move(placements));
}

[[nodiscard]] core::Result<std::optional<world::WorldPosition>>
parse_spawn_position(const modding::GenericPrototype& prototype) {
    const auto* encoded = field(prototype, "spawn_position");
    if (encoded == nullptr || encoded->empty()) {
        return core::Result<std::optional<world::WorldPosition>>::success(std::nullopt);
    }
    const auto values = split(*encoded, ':');
    if (values.size() != 3) {
        return core::Result<std::optional<world::WorldPosition>>::failure(
            "scenario_prototype.invalid_spawn_position", "spawn_position uses x:y:z");
    }
    auto x = parse_finite_double(values[0], "spawn x");
    auto y = parse_finite_double(values[1], "spawn y");
    auto z = parse_finite_double(values[2], "spawn z");
    if (!x || !y || !z) {
        const auto& error = !x ? x.error() : !y ? y.error() : z.error();
        return core::Result<std::optional<world::WorldPosition>>::failure(error.code,
                                                                          error.message);
    }
    auto position = world::WorldPosition::from_legacy_global({x.value(), y.value(), z.value()});
    if (!position) {
        return core::Result<std::optional<world::WorldPosition>>::failure(position.error().code,
                                                                          position.error().message);
    }
    return core::Result<std::optional<world::WorldPosition>>::success(position.value());
}

} // namespace

core::Result<ScenarioDefinition>
scenario_definition_from_prototype(const modding::GenericPrototype& prototype) {
    if (prototype.kind != modding::PrototypeKinds::scenario) {
        return core::Result<ScenarioDefinition>::failure("scenario_prototype.kind_mismatch",
                                                         "prototype is not a scenario");
    }
    if (!prototype.id.is_valid()) {
        return core::Result<ScenarioDefinition>::failure("scenario_prototype.invalid_id",
                                                         "scenario prototype id is invalid");
    }

    const auto* start_region_value = field(prototype, "start_region");
    if (start_region_value == nullptr || start_region_value->empty()) {
        return core::Result<ScenarioDefinition>::failure(
            "scenario_prototype.missing_start_region",
            "scenario prototype must declare start_region");
    }
    if (!core::is_valid_local_id(*start_region_value)) {
        return core::Result<ScenarioDefinition>::failure(
            "scenario_prototype.invalid_start_region",
            "scenario start_region must be a valid local id");
    }

    const auto* spawn_mode_value = field(prototype, "spawn_mode");
    if (spawn_mode_value == nullptr || spawn_mode_value->empty()) {
        return core::Result<ScenarioDefinition>::failure(
            "scenario_prototype.missing_spawn_mode", "scenario prototype must declare spawn_mode");
    }
    auto spawn_mode = scenario_spawn_mode_from_name(*spawn_mode_value);
    auto starting_items = parse_prototype_list(prototype, "starting_items");
    auto starting_cargo = parse_prototype_list(prototype, "starting_cargo");
    auto scene_entities = parse_scene_entities(prototype);
    auto tags = parse_tags(prototype);
    auto developer_world = parse_bool_field(prototype, "developer_world");
    auto world_seed = parse_optional_u64(prototype, "world_seed");
    auto initial_world_time = parse_optional_u64(prototype, "initial_world_time");
    auto spawn_position = parse_spawn_position(prototype);
    const auto* category_value = field(prototype, "category");
    const auto* world_source_value = field(prototype, "world_source");
    const auto* persistence_value = field(prototype, "persistence_policy");
    auto category = scenario_category_from_name(
        category_value == nullptr || category_value->empty() ? "gameplay" : *category_value);
    auto world_source = scenario_world_source_from_name(
        world_source_value == nullptr || world_source_value->empty() ? "generated"
                                                                     : *world_source_value);
    auto persistence = scenario_persistence_policy_from_name(
        persistence_value == nullptr || persistence_value->empty() ? "ephemeral"
                                                                   : *persistence_value);
    if (!spawn_mode) {
        return core::Result<ScenarioDefinition>::failure(spawn_mode.error().code,
                                                         spawn_mode.error().message);
    }
    if (!starting_items) {
        return core::Result<ScenarioDefinition>::failure(starting_items.error().code,
                                                         starting_items.error().message);
    }
    if (!starting_cargo) {
        return core::Result<ScenarioDefinition>::failure(starting_cargo.error().code,
                                                         starting_cargo.error().message);
    }
    if (!scene_entities) {
        return core::Result<ScenarioDefinition>::failure(scene_entities.error().code,
                                                         scene_entities.error().message);
    }
    if (!tags) {
        return core::Result<ScenarioDefinition>::failure(tags.error().code, tags.error().message);
    }
    if (!developer_world || !world_seed || !initial_world_time || !spawn_position || !category ||
        !world_source || !persistence) {
        const auto& error = !developer_world      ? developer_world.error()
                            : !world_seed         ? world_seed.error()
                            : !initial_world_time ? initial_world_time.error()
                            : !spawn_position     ? spawn_position.error()
                            : !category           ? category.error()
                            : !world_source       ? world_source.error()
                                                  : persistence.error();
        return core::Result<ScenarioDefinition>::failure(error.code, error.message);
    }

    ScenarioDefinition definition;
    definition.prototype_id = prototype.id;
    definition.display_name = prototype.display_name;
    const auto* description = field(prototype, "description");
    definition.description =
        description == nullptr || description->empty() ? prototype.display_name : *description;
    definition.category = category.value();
    definition.world_source = world_source.value();
    definition.persistence = persistence.value();
    const auto* thumbnail = field(prototype, "thumbnail");
    definition.thumbnail_asset = thumbnail == nullptr ? std::string{} : *thumbnail;
    definition.developer_world = developer_world.value();
    definition.start_region = *start_region_value;
    definition.spawn_mode = spawn_mode.value();
    definition.world_seed = world_seed.value();
    const auto* generator_preset = field(prototype, "generator_preset");
    definition.generator_preset = generator_preset == nullptr ? std::string{} : *generator_preset;
    definition.spawn_position = spawn_position.value();
    const auto* spawn_yaw = field(prototype, "spawn_yaw");
    const auto* spawn_pitch = field(prototype, "spawn_pitch");
    if (spawn_yaw != nullptr && !spawn_yaw->empty()) {
        auto parsed = parse_finite_double(*spawn_yaw, "spawn yaw");
        if (!parsed) {
            return core::Result<ScenarioDefinition>::failure(parsed.error().code,
                                                             parsed.error().message);
        }
        definition.spawn_yaw_degrees = static_cast<float>(parsed.value());
    }
    if (spawn_pitch != nullptr && !spawn_pitch->empty()) {
        auto parsed = parse_finite_double(*spawn_pitch, "spawn pitch");
        if (!parsed) {
            return core::Result<ScenarioDefinition>::failure(parsed.error().code,
                                                             parsed.error().message);
        }
        definition.spawn_pitch_degrees = static_cast<float>(parsed.value());
    }
    definition.initial_world_time = initial_world_time.value().value_or(0);
    const auto* initial_weather = field(prototype, "initial_weather");
    definition.initial_weather =
        initial_weather == nullptr || initial_weather->empty() ? "clear" : *initial_weather;
    definition.starting_items = std::move(starting_items).value();
    definition.starting_cargo = std::move(starting_cargo).value();
    definition.scene_entities = std::move(scene_entities).value();
    definition.tags = std::move(tags).value();
    definition.required_mods = parse_string_list(prototype, "required_mods");
    definition.required_resource_packs = parse_string_list(prototype, "required_resource_packs");
    definition.enabled_gameplay_modules = parse_string_list(prototype, "enabled_gameplay_modules");
    definition.debug_defaults = parse_string_list(prototype, "debug_defaults");
    const auto* setup_hook = field(prototype, "setup_hook");
    definition.setup_hook = setup_hook == nullptr ? std::string{} : *setup_hook;
    const auto* benchmark_profile = field(prototype, "benchmark_profile");
    definition.benchmark_profile =
        benchmark_profile == nullptr ? std::string{} : *benchmark_profile;

    auto status = definition.validate();
    if (!status) {
        return core::Result<ScenarioDefinition>::failure(status.error().code,
                                                         status.error().message);
    }
    return core::Result<ScenarioDefinition>::success(std::move(definition));
}

} // namespace heartstead::scenarios
