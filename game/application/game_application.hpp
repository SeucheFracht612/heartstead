#pragma once

#include "engine/audio/audio_system.hpp"
#include "engine/core/result.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/platform/platform.hpp"
#include "engine/renderer/renderer.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace heartstead::world {
class VoxelPalette;
}

namespace heartstead::game {

struct GameApplicationConfig {
    bool headless = false;
    std::optional<std::uint64_t> maximum_frames;
    platform::WindowDesc window{"Heartstead", 1280, 720, true};
    std::filesystem::path shader_root;
    const world::VoxelPalette* voxel_palette = nullptr;
    renderer::materials::TerrainMaterialAssetSet terrain_material_assets;
    bool enable_render_validation = true;
    renderer::RendererQualityPreset renderer_quality = renderer::RendererQualityPreset::high;
    std::uint32_t application_worker_count = 2;

    [[nodiscard]] core::Status validate() const;
};

struct GameApplicationFrame {
    std::uint64_t frame_index = 0;
    std::uint64_t delta_microseconds = 0;
    std::int64_t now_milliseconds = 0;
    renderer::rhi::RenderExtent extent{};
    const platform::WindowInputSnapshot* input = nullptr;
    bool headless = false;

    [[nodiscard]] float delta_seconds() const noexcept;
};

struct GameApplicationFrameOutput {
    std::optional<renderer::RenderFrameInput> render;
};

struct GameApplicationRunReport {
    std::uint64_t frame_count = 0;
    bool headless = false;
    std::string mode_summary;
};

class GameApplication;

class GameApplicationServices {
  public:
    [[nodiscard]] renderer::Renderer* renderer() noexcept;
    [[nodiscard]] const renderer::Renderer* renderer() const noexcept;
    [[nodiscard]] audio::IAudioSystem* audio() noexcept;
    [[nodiscard]] const audio::IAudioSystem* audio() const noexcept;
    [[nodiscard]] jobs::IJobSystem* jobs() noexcept;
    [[nodiscard]] const jobs::IJobSystem* jobs() const noexcept;
    [[nodiscard]] core::Status
    install_audio_system(std::unique_ptr<audio::IAudioSystem> audio_system);

    [[nodiscard]] core::Status set_cursor_capture(bool captured);
    [[nodiscard]] core::Status set_clipboard_text(std::string text);
    void request_quit() noexcept;
    [[nodiscard]] bool headless() const noexcept;

  private:
    friend class GameApplication;
    explicit GameApplicationServices(GameApplication& application) noexcept;

    GameApplication* application_ = nullptr;
};

class IGameApplicationMode {
  public:
    virtual ~IGameApplicationMode() = default;

    [[nodiscard]] virtual core::Status initialize(GameApplicationServices& services) = 0;
    [[nodiscard]] virtual core::Result<GameApplicationFrameOutput>
    update(GameApplicationServices& services, const GameApplicationFrame& frame) = 0;
    [[nodiscard]] virtual core::Status shutdown(GameApplicationServices& services) = 0;
    [[nodiscard]] virtual std::string summary() const = 0;
};

class GameApplication {
  public:
    explicit GameApplication(GameApplicationConfig config);
    ~GameApplication();

    GameApplication(const GameApplication&) = delete;
    GameApplication& operator=(const GameApplication&) = delete;

    [[nodiscard]] core::Result<GameApplicationRunReport> run(IGameApplicationMode& mode);

  private:
    friend class GameApplicationServices;

    [[nodiscard]] core::Status initialize_shell();
    [[nodiscard]] core::Status pump_platform_events();
    [[nodiscard]] core::Status shutdown_shell();

    GameApplicationConfig config_;
    std::unique_ptr<platform::IPlatform> platform_;
    platform::WindowId window_;
    renderer::rhi::RenderExtent extent_{};
    renderer::Renderer renderer_;
    std::unique_ptr<audio::IAudioSystem> audio_;
    std::unique_ptr<jobs::IJobSystem> jobs_;
    bool running_ = false;
};

} // namespace heartstead::game
