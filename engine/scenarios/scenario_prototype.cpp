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

[[nodiscard]] core::Result<double> parse_finite_double(std::string_view value,
                                                       std::string_view label) {
    double parsed = 0.0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() ||
        !std::isfinite(parsed)) {
        return core::Result<double>::failure(
            "scenario_prototype.invalid_scene_entity",
            std::string(label) + " must be a finite number");
    }
    return core::Result<double>::success(parsed);
}

[[nodiscard]] core::Result<std::vector<ScenarioEntityPlacement>>
parse_scene_entities(const modding::GenericPrototype& prototype) {
    const auto* value = field(prototype, "scene_entities");
    std::vector<ScenarioEntityPlacement> placements;
    if (value == nullptr || value->empty()) {
        return core::Result<std::vector<ScenarioEntityPlacement>>::success(
            std::move(placements));
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
            const auto& error = !x       ? x.error()
                                : !y     ? y.error()
                                : !z     ? z.error()
                                : !yaw   ? yaw.error()
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

    ScenarioDefinition definition;
    definition.prototype_id = prototype.id;
    definition.start_region = *start_region_value;
    definition.spawn_mode = spawn_mode.value();
    definition.starting_items = std::move(starting_items).value();
    definition.starting_cargo = std::move(starting_cargo).value();
    definition.scene_entities = std::move(scene_entities).value();
    definition.tags = std::move(tags).value();

    auto status = definition.validate();
    if (!status) {
        return core::Result<ScenarioDefinition>::failure(status.error().code,
                                                         status.error().message);
    }
    return core::Result<ScenarioDefinition>::success(std::move(definition));
}

} // namespace heartstead::scenarios
