#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/world/coords/world_position.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::scenarios {

enum class ScenarioSpawnMode {
    homestead,
    outpost,
    debug,
};

enum class ScenarioCategory {
    gameplay,
    rendering,
    world,
    movement,
    physics,
    networking,
    performance,
    audio,
    ui,
};

enum class ScenarioWorldSource {
    generated,
    packaged_fixture,
    existing_save,
    deterministic_setup,
};

enum class ScenarioPersistencePolicy {
    ephemeral,
    temporary_copy,
    persistent,
};

struct ScenarioEntityPlacement {
    core::PrototypeId prototype_id;
    world::WorldTransform transform;

    [[nodiscard]] core::Status validate() const;
};

struct ScenarioDefinition {
    core::PrototypeId prototype_id;
    std::string display_name;
    std::string description;
    ScenarioCategory category = ScenarioCategory::gameplay;
    ScenarioWorldSource world_source = ScenarioWorldSource::generated;
    ScenarioPersistencePolicy persistence = ScenarioPersistencePolicy::ephemeral;
    std::string thumbnail_asset;
    bool developer_world = false;
    std::string start_region;
    ScenarioSpawnMode spawn_mode = ScenarioSpawnMode::homestead;
    std::optional<std::uint64_t> world_seed;
    std::string generator_preset;
    std::optional<world::WorldPosition> spawn_position;
    float spawn_yaw_degrees = 0.0F;
    float spawn_pitch_degrees = 0.0F;
    std::uint64_t initial_world_time = 0;
    std::string initial_weather = "clear";
    std::vector<core::PrototypeId> starting_items;
    std::vector<core::PrototypeId> starting_cargo;
    std::vector<ScenarioEntityPlacement> scene_entities;
    std::vector<std::string> tags;
    std::vector<std::string> required_mods;
    std::vector<std::string> required_resource_packs;
    std::vector<std::string> enabled_gameplay_modules;
    std::vector<std::string> debug_defaults;
    std::string setup_hook;
    std::string benchmark_profile;

    [[nodiscard]] core::Status validate() const;
};

[[nodiscard]] core::Result<ScenarioSpawnMode> scenario_spawn_mode_from_name(std::string_view name);
[[nodiscard]] std::string_view scenario_spawn_mode_name(ScenarioSpawnMode mode) noexcept;
[[nodiscard]] core::Result<ScenarioCategory> scenario_category_from_name(std::string_view name);
[[nodiscard]] std::string_view scenario_category_name(ScenarioCategory category) noexcept;
[[nodiscard]] core::Result<ScenarioWorldSource>
scenario_world_source_from_name(std::string_view name);
[[nodiscard]] std::string_view scenario_world_source_name(ScenarioWorldSource source) noexcept;
[[nodiscard]] core::Result<ScenarioPersistencePolicy>
scenario_persistence_policy_from_name(std::string_view name);
[[nodiscard]] std::string_view
scenario_persistence_policy_name(ScenarioPersistencePolicy policy) noexcept;

} // namespace heartstead::scenarios
