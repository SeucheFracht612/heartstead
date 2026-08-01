#pragma once

#include "engine/modding/mod_diagnostic.hpp"
#include "game/application/game_application.hpp"

#include <memory>
#include <vector>

namespace heartstead::game {

struct StartupRecoveryModeConfig {
    std::vector<modding::ModDiagnostic> diagnostics;
    bool headless = false;
    float ui_scale = 1.0F;
};

// A renderer-backed outer shell used when gameplay content is invalid. It intentionally owns no
// GameRuntime, audio presentation, save catalog, or world state.
class StartupRecoveryMode final : public IGameApplicationMode {
  public:
    explicit StartupRecoveryMode(StartupRecoveryModeConfig config);
    ~StartupRecoveryMode() override;

    StartupRecoveryMode(const StartupRecoveryMode&) = delete;
    StartupRecoveryMode& operator=(const StartupRecoveryMode&) = delete;

    [[nodiscard]] core::Status initialize(GameApplicationServices& services) override;
    [[nodiscard]] core::Result<GameApplicationFrameOutput>
    update(GameApplicationServices& services, const GameApplicationFrame& frame) override;
    [[nodiscard]] core::Status shutdown(GameApplicationServices& services) override;
    [[nodiscard]] std::string summary() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace heartstead::game
