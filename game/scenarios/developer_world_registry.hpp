#pragma once

#include "engine/core/result.hpp"
#include "engine/save/save_metadata.hpp"
#include "engine/scenarios/scenario.hpp"
#include "game/runtime/runtime_session.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace heartstead::game {

class DeveloperWorldRegistry final {
  public:
    DeveloperWorldRegistry() = default;
    [[nodiscard]] static core::Result<DeveloperWorldRegistry>
    create(std::span<const scenarios::ScenarioDefinition> definitions);

    [[nodiscard]] const std::vector<scenarios::ScenarioDefinition>& entries() const noexcept;
    [[nodiscard]] const scenarios::ScenarioDefinition*
    find(std::string_view stable_id) const noexcept;
    [[nodiscard]] std::vector<const scenarios::ScenarioDefinition*>
    filter(std::optional<scenarios::ScenarioCategory> category = std::nullopt,
           std::string_view search = {}) const;
    [[nodiscard]] core::Result<SessionLaunchRequest>
    make_launch_request(std::string_view stable_id, save::SaveMetadata metadata,
                        bool headless) const;

  private:
    std::vector<scenarios::ScenarioDefinition> entries_;
};

} // namespace heartstead::game
