#include "heartstead/render/menu_ui.hpp"

#include <algorithm>
#include <cstddef>

namespace heartstead {

MenuLayout MenuLayout::from_framebuffer(
    MenuScreen screen, std::int32_t width, std::int32_t height) noexcept {
    if (screen == MenuScreen::main) {
        const auto panel_width = std::clamp(width / 3, 390, 480);
        const auto left = std::max(42, width / 14);
        const auto button_top = std::max(270, height / 2 - 35);
        return {
            .panel = {left, 0, left + panel_width, height},
            .primary = {left + 42, button_top, left + panel_width - 42, button_top + 58},
            .secondary = {left + 42, button_top + 72, left + panel_width - 42, button_top + 130},
            .tertiary = {left + 42, button_top + 144, left + panel_width - 42, button_top + 202},
            .quaternary = {left + 42, button_top + 216, left + panel_width - 42, button_top + 274},
        };
    }

    const auto panel_width = std::clamp(width - 120, 620, 820);
    const auto panel_height = std::clamp(height - 90, 500, 620);
    const auto left = (width - panel_width) / 2;
    const auto top = (height - panel_height) / 2;
    if (screen == MenuScreen::create_world) {
        return {
            .panel = {left, top, left + panel_width, top + panel_height},
            .text_field = {left + 54, top + 154, left + panel_width - 54, top + 204},
            .primary = {left + 54, top + panel_height - 88,
                left + panel_width - 276, top + panel_height - 36},
            .secondary = {left + panel_width - 254, top + panel_height - 88,
                left + panel_width - 54, top + panel_height - 36},
        };
    }
    if (screen == MenuScreen::singleplayer_worlds) {
        MenuLayout layout{
            .panel = {left, top, left + panel_width, top + panel_height},
            .primary = {left + 42, top + panel_height - 82,
                left + 222, top + panel_height - 34},
            .secondary = {left + 238, top + panel_height - 82,
                left + 470, top + panel_height - 34},
            .tertiary = {left + panel_width - 188, top + panel_height - 82,
                left + panel_width - 42, top + panel_height - 34},
        };
        for (std::size_t index = 0; index < layout.world_tabs.size(); ++index) {
            const auto row_top = top + 112 + static_cast<std::int32_t>(index) * 64;
            layout.world_tabs[index] = {
                left + 42, row_top, left + panel_width - 42, row_top + 54};
        }
        return layout;
    }
    if (screen == MenuScreen::pause) {
        const auto pause_width = 460;
        const auto pause_height = 300;
        const auto pause_left = (width - pause_width) / 2;
        const auto pause_top = (height - pause_height) / 2;
        return {
            .panel = {pause_left, pause_top, pause_left + pause_width, pause_top + pause_height},
            .primary = {pause_left + 52, pause_top + 112,
                pause_left + pause_width - 52, pause_top + 168},
            .secondary = {pause_left + 52, pause_top + 188,
                pause_left + pause_width - 52, pause_top + 244},
        };
    }
    return {
        .panel = {left, top, left + panel_width, top + panel_height},
        .primary = {left + 54, top + panel_height - 88,
            left + panel_width - 276, top + panel_height - 36},
        .secondary = {left + panel_width - 254, top + panel_height - 88,
            left + panel_width - 54, top + panel_height - 36},
    };
}

} // namespace heartstead
