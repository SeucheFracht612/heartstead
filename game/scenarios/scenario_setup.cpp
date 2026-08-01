#include "game/scenarios/scenario_setup.hpp"

#include "engine/scenarios/scenario_fixture.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <utility>

namespace heartstead::game {

namespace {

[[nodiscard]] core::Status renderer_proof_fixture(world::WorldState& state,
                                                  const world::VoxelPalette& palette) {
    auto types = scenarios::resolve_renderer_proof_voxel_types(palette);
    if (!types) {
        return core::Status::failure(types.error().code, types.error().message);
    }
    auto status = scenarios::populate_renderer_proof_fixture(state, types.value());
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
