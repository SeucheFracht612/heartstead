#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/quality/renderer_quality.hpp"
#include "engine/renderer/ui/ui_renderer.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace heartstead::game {

struct ApplicationSettings {
    std::uint32_t window_width = 1280;
    std::uint32_t window_height = 720;
    bool windowed = true;
    renderer::RendererQualityPreset rendering_quality = renderer::RendererQualityPreset::high;
    float master_volume = 1.0F;
    float music_volume = 1.0F;
    float effects_volume = 1.0F;
    float mouse_sensitivity = 1.0F;
    float controller_sensitivity = 1.0F;
    bool controller_enabled = true;
    float ui_scale = 1.0F;
    float ui_contrast = 1.0F;
    float ui_saturation = 1.0F;
    renderer::UiColorVisionMode color_vision_mode = renderer::UiColorVisionMode::none;
    bool reduced_motion = false;
    std::string last_world_slot;
    std::vector<std::string> recent_servers;

    [[nodiscard]] core::Status validate() const;
};

class ApplicationSettingsStore final {
  public:
    explicit ApplicationSettingsStore(std::filesystem::path path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] core::Result<ApplicationSettings> load() const;
    [[nodiscard]] core::Status save(const ApplicationSettings& settings) const;

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::filesystem::path default_application_data_root();

} // namespace heartstead::game
