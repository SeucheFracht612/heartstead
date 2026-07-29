#pragma once

#include "engine/net/transport.hpp"
#include "game/application/game_application.hpp"

#include <filesystem>
#include <memory>
#include <optional>

namespace heartstead::content {
struct ContentValidationReport;
}

namespace heartstead::dev_game {

struct DevGameModeConfig {
    const content::ContentValidationReport* content_report = nullptr;
    std::filesystem::path cooked_asset_root;
    std::optional<std::filesystem::path> save_root;
    std::optional<net::TransportEndpoint> connect_endpoint;
    bool headless = false;
};

class DevGameMode final : public game::IGameApplicationMode {
  public:
    explicit DevGameMode(DevGameModeConfig config);
    ~DevGameMode() override;

    DevGameMode(const DevGameMode&) = delete;
    DevGameMode& operator=(const DevGameMode&) = delete;

    [[nodiscard]] core::Status initialize(game::GameApplicationServices& services) override;
    [[nodiscard]] core::Result<game::GameApplicationFrameOutput>
    update(game::GameApplicationServices& services,
           const game::GameApplicationFrame& frame) override;
    [[nodiscard]] core::Status shutdown(game::GameApplicationServices& services) override;
    [[nodiscard]] std::string summary() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace heartstead::dev_game
