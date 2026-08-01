#include "game/scenarios/developer_world_registry.hpp"

#include "game/scenarios/scenario_setup.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <utility>

namespace heartstead::game {

namespace {

[[nodiscard]] std::string lowercase(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char character) { return std::tolower(character); });
    return result;
}

} // namespace

core::Result<DeveloperWorldRegistry>
DeveloperWorldRegistry::create(std::span<const scenarios::ScenarioDefinition> definitions) {
    DeveloperWorldRegistry registry;
    std::unordered_set<std::string> ids;
    for (const auto& definition : definitions) {
        if (!definition.developer_world) {
            continue;
        }
        auto status = definition.validate();
        if (!status) {
            return core::Result<DeveloperWorldRegistry>::failure(status.error().code,
                                                                 status.error().message);
        }
        if (!ids.emplace(definition.prototype_id.value()).second) {
            return core::Result<DeveloperWorldRegistry>::failure(
                "developer_world.duplicate_id",
                "developer world stable id is registered more than once: " +
                    definition.prototype_id.value());
        }
        if (!definition.setup_hook.empty() &&
            !default_scenario_setup_registry().contains(definition.setup_hook)) {
            return core::Result<DeveloperWorldRegistry>::failure(
                "developer_world.setup_hook_missing",
                "developer world references an unregistered setup hook: " + definition.setup_hook);
        }
        registry.entries_.push_back(definition);
    }
    std::ranges::sort(registry.entries_, {}, [](const scenarios::ScenarioDefinition& entry) {
        return std::pair{scenarios::scenario_category_name(entry.category), entry.display_name};
    });
    return core::Result<DeveloperWorldRegistry>::success(std::move(registry));
}

const std::vector<scenarios::ScenarioDefinition>& DeveloperWorldRegistry::entries() const noexcept {
    return entries_;
}

const scenarios::ScenarioDefinition*
DeveloperWorldRegistry::find(std::string_view stable_id) const noexcept {
    const auto found =
        std::ranges::find(entries_, stable_id, [](const scenarios::ScenarioDefinition& entry) {
            return std::string_view(entry.prototype_id.value());
        });
    return found == entries_.end() ? nullptr : &*found;
}

std::vector<const scenarios::ScenarioDefinition*>
DeveloperWorldRegistry::filter(std::optional<scenarios::ScenarioCategory> category,
                               std::string_view search) const {
    const auto query = lowercase(search);
    std::vector<const scenarios::ScenarioDefinition*> result;
    for (const auto& entry : entries_) {
        if (category.has_value() && entry.category != *category) {
            continue;
        }
        auto searchable = lowercase(entry.prototype_id.value() + " " + entry.display_name + " " +
                                    entry.description + " " +
                                    std::string(scenarios::scenario_category_name(entry.category)));
        for (const auto& tag : entry.tags) {
            searchable += " " + lowercase(tag);
        }
        if (!query.empty() && !searchable.contains(query)) {
            continue;
        }
        result.push_back(&entry);
    }
    return result;
}

core::Result<SessionLaunchRequest>
DeveloperWorldRegistry::make_launch_request(std::string_view stable_id, save::SaveMetadata metadata,
                                            bool headless) const {
    const auto* definition = find(stable_id);
    if (definition == nullptr) {
        return core::Result<SessionLaunchRequest>::failure("developer_world.not_found",
                                                           "developer world is not registered: " +
                                                               std::string(stable_id));
    }
    SessionLaunchRequest request;
    request.mode = SessionMode::local_single_player;
    switch (definition->world_source) {
    case scenarios::ScenarioWorldSource::generated:
    case scenarios::ScenarioWorldSource::deterministic_setup:
        request.world_source = WorldSourceKind::developer_scenario;
        break;
    case scenarios::ScenarioWorldSource::packaged_fixture:
        request.world_source = WorldSourceKind::packaged_fixture;
        break;
    case scenarios::ScenarioWorldSource::existing_save:
        return core::Result<SessionLaunchRequest>::failure(
            "developer_world.save_path_required",
            "existing-save developer world requires a concrete save path");
    }
    switch (definition->persistence) {
    case scenarios::ScenarioPersistencePolicy::ephemeral:
        request.persistence = PersistencePolicy::ephemeral;
        break;
    case scenarios::ScenarioPersistencePolicy::temporary_copy:
        request.persistence = PersistencePolicy::temporary_copy;
        break;
    case scenarios::ScenarioPersistencePolicy::persistent:
        request.persistence = PersistencePolicy::persistent;
        break;
    }
    request.world_name = definition->display_name;
    request.scenario_id = definition->prototype_id.value();
    request.seed = definition->world_seed.value_or(metadata.world_seed);
    metadata.world_seed = *request.seed;
    request.metadata = std::move(metadata);
    request.generator_preset = definition->generator_preset;
    request.required_mods = definition->required_mods;
    request.required_resource_packs = definition->required_resource_packs;
    request.initial_runtime_options = definition->enabled_gameplay_modules;
    request.initial_runtime_options.insert(request.initial_runtime_options.end(),
                                           definition->debug_defaults.begin(),
                                           definition->debug_defaults.end());
    if (definition->spawn_position.has_value()) {
        request.player_spawn =
            PlayerSpawnOverride{*definition->spawn_position, definition->spawn_yaw_degrees,
                                definition->spawn_pitch_degrees};
    }
    request.runtime.headless = headless;
    request.runtime.create_renderer = !headless;
    request.runtime.create_audio = !headless;
    request.runtime.physics_backend =
        headless ? physics::PhysicsBackend::headless : physics::PhysicsBackend::jolt;
    return core::Result<SessionLaunchRequest>::success(std::move(request));
}

} // namespace heartstead::game
