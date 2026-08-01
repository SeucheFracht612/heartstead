#pragma once

#include "engine/core/result.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::world {
class VoxelPalette;
class WorldState;
} // namespace heartstead::world

namespace heartstead::game {

using ScenarioSetupHook =
    std::function<core::Status(world::WorldState&, const world::VoxelPalette&)>;

class ScenarioSetupRegistry final {
  public:
    [[nodiscard]] core::Status add(std::string stable_id, ScenarioSetupHook hook);
    [[nodiscard]] core::Status apply(std::string_view stable_id, world::WorldState& world,
                                     const world::VoxelPalette& palette) const;
    [[nodiscard]] bool contains(std::string_view stable_id) const noexcept;
    [[nodiscard]] std::vector<std::string> ids() const;

  private:
    std::unordered_map<std::string, ScenarioSetupHook> hooks_;
};

[[nodiscard]] const ScenarioSetupRegistry& default_scenario_setup_registry();

} // namespace heartstead::game
