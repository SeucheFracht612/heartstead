#pragma once

#include "game/application/application_settings.hpp"
#include "game/application/game_application.hpp"
#include "game/application/launch_options.hpp"
#include "game/runtime/runtime_session.hpp"

#include <filesystem>
#include <memory>
#include <optional>

namespace heartstead::assets {
class CookedAssetStore;
}

namespace heartstead::content {
struct ContentValidationReport;
}

namespace heartstead::game {

struct HeartsteadApplicationModeConfig {
    const content::ContentValidationReport* content_report = nullptr;
    const assets::CookedAssetStore* cooked_assets = nullptr;
    std::filesystem::path cooked_asset_root;
    std::filesystem::path user_data_root;
    ApplicationSettings initial_settings;
    std::optional<InitialLaunchDirective> initial_launch;
    std::optional<SessionLaunchRequest> initial_session;
    std::int64_t autosave_interval_ms = 30'000;
    bool headless = false;
    bool safe_mode = false;
};

class HeartsteadApplicationMode final : public IGameApplicationMode {
  public:
    explicit HeartsteadApplicationMode(HeartsteadApplicationModeConfig config);
    ~HeartsteadApplicationMode() override;

    HeartsteadApplicationMode(const HeartsteadApplicationMode&) = delete;
    HeartsteadApplicationMode& operator=(const HeartsteadApplicationMode&) = delete;

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
