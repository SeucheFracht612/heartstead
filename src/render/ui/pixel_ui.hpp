#pragma once

#include "heartstead/render/menu_ui.hpp"
#include "heartstead/render/opengl_renderer.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::render::ui {

void build_menu_pixels(
    std::vector<std::uint8_t>& pixels,
    std::int32_t width,
    std::int32_t height,
    const MenuUiState& state,
    const VideoSettings& settings);

void build_settings_pixels(
    std::vector<std::uint8_t>& pixels,
    std::int32_t width,
    std::int32_t height,
    const VideoSettings& settings,
    const VideoSettingsUiState& ui_state);

[[nodiscard]] std::string debug_text(const DebugStats& stats);

void build_debug_pixels(
    std::vector<std::uint8_t>& pixels,
    std::int32_t width,
    std::int32_t height,
    std::string_view stats_text);

} // namespace heartstead::render::ui
