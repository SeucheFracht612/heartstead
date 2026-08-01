#include "game/scenarios/scenario_setup.hpp"

#include "engine/core/ids.hpp"
#include "engine/scenarios/scenario_fixture.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace heartstead::game {

namespace {

[[nodiscard]] core::Result<std::uint16_t> voxel_type(const world::VoxelPalette& palette,
                                                     std::string_view prototype_id) {
    auto parsed = core::PrototypeId::parse(prototype_id);
    const auto type = parsed.has_value() ? palette.type_for(*parsed) : std::nullopt;
    if (!type.has_value()) {
        return core::Result<std::uint16_t>::failure("scenario_setup.voxel_missing",
                                                    "scenario setup voxel is not available: " +
                                                        std::string(prototype_id));
    }
    return core::Result<std::uint16_t>::success(*type);
}

[[nodiscard]] core::Status renderer_proof_fixture(world::WorldState& state,
                                                  const world::VoxelPalette& palette) {
    auto grass = voxel_type(palette, "base:voxels/grass");
    auto dirt = voxel_type(palette, "base:voxels/dirt");
    auto stone = voxel_type(palette, "base:voxels/stone");
    if (!grass || !dirt || !stone) {
        const auto& error = !grass ? grass.error() : !dirt ? dirt.error() : stone.error();
        return core::Status::failure(error.code, error.message);
    }
    auto status = scenarios::populate_renderer_proof_fixture(
        state, {grass.value(), dirt.value(), stone.value()});
    if (!status) {
        return status;
    }
    return state.mod_states().insert({"engine", "scenario.fixture", "renderer_proof"});
}

} // namespace

core::Status ScenarioSetupRegistry::add(std::string stable_id, ScenarioSetupHook hook) {
    if (!core::is_valid_local_id(stable_id) || !hook) {
        return core::Status::failure("scenario_setup.invalid_hook",
                                     "scenario setup hook requires a valid id and callback");
    }
    if (!hooks_.emplace(std::move(stable_id), std::move(hook)).second) {
        return core::Status::failure("scenario_setup.duplicate_hook",
                                     "scenario setup hook id is already registered");
    }
    return core::Status::ok();
}

core::Status ScenarioSetupRegistry::apply(std::string_view stable_id, world::WorldState& world,
                                          const world::VoxelPalette& palette) const {
    const auto found = hooks_.find(std::string(stable_id));
    if (found == hooks_.end()) {
        return core::Status::failure("scenario_setup.hook_missing",
                                     "scenario setup hook is not registered: " +
                                         std::string(stable_id));
    }
    return found->second(world, palette);
}

bool ScenarioSetupRegistry::contains(std::string_view stable_id) const noexcept {
    return hooks_.contains(std::string(stable_id));
}

std::vector<std::string> ScenarioSetupRegistry::ids() const {
    std::vector<std::string> result;
    result.reserve(hooks_.size());
    for (const auto& [id, _] : hooks_) {
        result.push_back(id);
    }
    std::ranges::sort(result);
    return result;
}

const ScenarioSetupRegistry& default_scenario_setup_registry() {
    static const ScenarioSetupRegistry registry = [] {
        ScenarioSetupRegistry value;
        (void)value.add("renderer_proof", renderer_proof_fixture);
        return value;
    }();
    return registry;
}

} // namespace heartstead::game
