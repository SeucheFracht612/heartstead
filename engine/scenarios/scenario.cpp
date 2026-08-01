#include "engine/scenarios/scenario.hpp"

#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace heartstead::scenarios {

core::Status ScenarioEntityPlacement::validate() const {
    if (!prototype_id.is_valid()) {
        return core::Status::failure("scenario_definition.invalid_scene_entity",
                                     "scenario scene entity prototype id must be valid");
    }
    if (!transform.position.is_valid() || !transform.rotation_degrees.is_finite() ||
        !transform.scale.is_finite() || transform.scale.x <= 0.0 || transform.scale.y <= 0.0 ||
        transform.scale.z <= 0.0) {
        return core::Status::failure(
            "scenario_definition.invalid_scene_transform",
            "scenario scene entity position, rotation, and positive scale must be valid");
    }
    return core::Status::ok();
}

core::Status ScenarioDefinition::validate() const {
    if (!prototype_id.is_valid()) {
        return core::Status::failure("scenario_definition.invalid_prototype",
                                     "scenario definition prototype id must be valid");
    }
    if (!core::is_valid_local_id(start_region)) {
        return core::Status::failure("scenario_definition.invalid_start_region",
                                     "scenario start region must be a valid local id");
    }
    if (display_name.empty() || description.empty()) {
        return core::Status::failure("scenario_definition.missing_browser_text",
                                     "scenario display name and description are required");
    }
    if (!generator_preset.empty() && !core::is_valid_local_id(generator_preset)) {
        return core::Status::failure("scenario_definition.invalid_generator_preset",
                                     "scenario generator preset must be a valid local id");
    }
    if (spawn_position.has_value() && !spawn_position->is_valid()) {
        return core::Status::failure("scenario_definition.invalid_spawn_position",
                                     "scenario spawn position must be normalized and finite");
    }
    if (!std::isfinite(spawn_yaw_degrees) || !std::isfinite(spawn_pitch_degrees)) {
        return core::Status::failure("scenario_definition.invalid_spawn_orientation",
                                     "scenario spawn orientation must be finite");
    }
    if (!core::is_valid_local_id(initial_weather)) {
        return core::Status::failure("scenario_definition.invalid_weather",
                                     "scenario initial weather must be a valid local id");
    }
    if (!setup_hook.empty() && !core::is_valid_local_id(setup_hook)) {
        return core::Status::failure("scenario_definition.invalid_setup_hook",
                                     "scenario setup hook must be a valid local id");
    }
    if (world_source == ScenarioWorldSource::packaged_fixture && setup_hook.empty()) {
        return core::Status::failure(
            "scenario_definition.fixture_hook_missing",
            "packaged fixture scenario must name its deterministic setup hook");
    }
    for (const auto& item : starting_items) {
        if (!item.is_valid()) {
            return core::Status::failure("scenario_definition.invalid_starting_item",
                                         "scenario starting item id must be valid");
        }
    }
    for (const auto& cargo : starting_cargo) {
        if (!cargo.is_valid()) {
            return core::Status::failure("scenario_definition.invalid_starting_cargo",
                                         "scenario starting cargo id must be valid");
        }
    }
    for (const auto& placement : scene_entities) {
        auto status = placement.validate();
        if (!status) {
            return status;
        }
    }
    for (const auto& tag : tags) {
        if (!core::is_valid_local_id(tag)) {
            return core::Status::failure("scenario_definition.invalid_tag",
                                         "scenario tag is invalid: " + tag);
        }
    }
    const auto validate_tokens = [](const std::vector<std::string>& values,
                                    std::string_view field) -> core::Status {
        for (const auto& value : values) {
            if (!core::is_valid_local_id(value) && !core::is_valid_namespace_id(value)) {
                return core::Status::failure("scenario_definition.invalid_list_value",
                                             std::string(field) +
                                                 " contains invalid value: " + value);
            }
        }
        return core::Status::ok();
    };
    for (const auto* values :
         {&required_mods, &required_resource_packs, &enabled_gameplay_modules, &debug_defaults}) {
        auto status = validate_tokens(*values, "scenario list");
        if (!status) {
            return status;
        }
    }
    if (!benchmark_profile.empty() && !core::is_valid_local_id(benchmark_profile)) {
        return core::Status::failure("scenario_definition.invalid_benchmark_profile",
                                     "scenario benchmark profile must be a valid local id");
    }
    return core::Status::ok();
}

core::Result<ScenarioSpawnMode> scenario_spawn_mode_from_name(std::string_view name) {
    if (name == "homestead") {
        return core::Result<ScenarioSpawnMode>::success(ScenarioSpawnMode::homestead);
    }
    if (name == "outpost") {
        return core::Result<ScenarioSpawnMode>::success(ScenarioSpawnMode::outpost);
    }
    if (name == "debug") {
        return core::Result<ScenarioSpawnMode>::success(ScenarioSpawnMode::debug);
    }
    return core::Result<ScenarioSpawnMode>::failure(
        "scenario.invalid_spawn_mode", "unsupported scenario spawn mode: " + std::string(name));
}

std::string_view scenario_spawn_mode_name(ScenarioSpawnMode mode) noexcept {
    switch (mode) {
    case ScenarioSpawnMode::homestead:
        return "homestead";
    case ScenarioSpawnMode::outpost:
        return "outpost";
    case ScenarioSpawnMode::debug:
        return "debug";
    }
    return "unknown";
}

core::Result<ScenarioCategory> scenario_category_from_name(std::string_view name) {
    constexpr std::array values{
        std::pair{"gameplay", ScenarioCategory::gameplay},
        std::pair{"rendering", ScenarioCategory::rendering},
        std::pair{"world", ScenarioCategory::world},
        std::pair{"movement", ScenarioCategory::movement},
        std::pair{"physics", ScenarioCategory::physics},
        std::pair{"networking", ScenarioCategory::networking},
        std::pair{"performance", ScenarioCategory::performance},
        std::pair{"audio", ScenarioCategory::audio},
        std::pair{"ui", ScenarioCategory::ui},
    };
    for (const auto& [value, category] : values) {
        if (name == value) {
            return core::Result<ScenarioCategory>::success(category);
        }
    }
    return core::Result<ScenarioCategory>::failure(
        "scenario.invalid_category", "unsupported scenario category: " + std::string(name));
}

std::string_view scenario_category_name(ScenarioCategory category) noexcept {
    switch (category) {
    case ScenarioCategory::gameplay:
        return "gameplay";
    case ScenarioCategory::rendering:
        return "rendering";
    case ScenarioCategory::world:
        return "world";
    case ScenarioCategory::movement:
        return "movement";
    case ScenarioCategory::physics:
        return "physics";
    case ScenarioCategory::networking:
        return "networking";
    case ScenarioCategory::performance:
        return "performance";
    case ScenarioCategory::audio:
        return "audio";
    case ScenarioCategory::ui:
        return "ui";
    }
    return "unknown";
}

core::Result<ScenarioWorldSource> scenario_world_source_from_name(std::string_view name) {
    if (name == "generated") {
        return core::Result<ScenarioWorldSource>::success(ScenarioWorldSource::generated);
    }
    if (name == "packaged_fixture") {
        return core::Result<ScenarioWorldSource>::success(ScenarioWorldSource::packaged_fixture);
    }
    if (name == "existing_save") {
        return core::Result<ScenarioWorldSource>::success(ScenarioWorldSource::existing_save);
    }
    if (name == "deterministic_setup") {
        return core::Result<ScenarioWorldSource>::success(ScenarioWorldSource::deterministic_setup);
    }
    return core::Result<ScenarioWorldSource>::failure(
        "scenario.invalid_world_source", "unsupported scenario world source: " + std::string(name));
}

std::string_view scenario_world_source_name(ScenarioWorldSource source) noexcept {
    switch (source) {
    case ScenarioWorldSource::generated:
        return "generated";
    case ScenarioWorldSource::packaged_fixture:
        return "packaged_fixture";
    case ScenarioWorldSource::existing_save:
        return "existing_save";
    case ScenarioWorldSource::deterministic_setup:
        return "deterministic_setup";
    }
    return "unknown";
}

core::Result<ScenarioPersistencePolicy>
scenario_persistence_policy_from_name(std::string_view name) {
    if (name == "ephemeral") {
        return core::Result<ScenarioPersistencePolicy>::success(
            ScenarioPersistencePolicy::ephemeral);
    }
    if (name == "temporary_copy") {
        return core::Result<ScenarioPersistencePolicy>::success(
            ScenarioPersistencePolicy::temporary_copy);
    }
    if (name == "persistent") {
        return core::Result<ScenarioPersistencePolicy>::success(
            ScenarioPersistencePolicy::persistent);
    }
    return core::Result<ScenarioPersistencePolicy>::failure(
        "scenario.invalid_persistence_policy",
        "unsupported scenario persistence policy: " + std::string(name));
}

std::string_view scenario_persistence_policy_name(ScenarioPersistencePolicy policy) noexcept {
    switch (policy) {
    case ScenarioPersistencePolicy::ephemeral:
        return "ephemeral";
    case ScenarioPersistencePolicy::temporary_copy:
        return "temporary_copy";
    case ScenarioPersistencePolicy::persistent:
        return "persistent";
    }
    return "unknown";
}

} // namespace heartstead::scenarios
