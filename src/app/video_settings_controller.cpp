#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "heartstead/app/video_settings_controller.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>

namespace heartstead::app {

void VideoSettingsController::update(
    GLFWwindow* window,
    VideoSettings& settings,
    VideoSettingsUiState& ui_state,
    std::int32_t framebuffer_width,
    std::int32_t framebuffer_height,
    bool mouse_pressed,
    const platform::InputEvents& events) {
    double cursor_x = 0.0;
    double cursor_y = 0.0;
    glfwGetCursorPos(window, &cursor_x, &cursor_y);
    std::int32_t window_width = 0;
    std::int32_t window_height = 0;
    glfwGetWindowSize(window, &window_width, &window_height);
    if (window_width <= 0 || window_height <= 0) return;
    cursor_x *= static_cast<double>(framebuffer_width) / static_cast<double>(window_width);
    cursor_y *= static_cast<double>(framebuffer_height) / static_cast<double>(window_height);

    const auto layout = VideoSettingsLayout::from_framebuffer(framebuffer_width, framebuffer_height);
    const auto inside_value = cursor_x >= layout.render_value_left && cursor_x <= layout.render_value_right &&
        cursor_y >= layout.render_value_top && cursor_y <= layout.render_value_bottom;
    if (mouse_pressed && inside_value) {
        const auto now = glfwGetTime();
        if (now - last_value_click_ <= 0.38) {
            ui_state.editing_render_distance = true;
            ui_state.numeric_text.clear();
        }
        last_value_click_ = now;
    }

    if (ui_state.editing_render_distance) {
        if (!events.numeric_characters.empty() && ui_state.numeric_text.size() < 3U) {
            ui_state.numeric_text += events.numeric_characters.substr(
                0, 3U - ui_state.numeric_text.size());
        }
        if (events.numeric_backspace && !ui_state.numeric_text.empty())
            ui_state.numeric_text.pop_back();
        if (events.numeric_accept) {
            std::int32_t value = 0;
            const auto* begin = ui_state.numeric_text.data();
            const auto* end = begin + ui_state.numeric_text.size();
            const auto parsed = std::from_chars(begin, end, value);
            if (parsed.ec == std::errc{} && parsed.ptr == end) {
                value = std::clamp(value, 4, 128);
                settings.render_distance_chunks = value;
                settings.render_distance_scale_max = value;
            }
            ui_state.editing_render_distance = false;
        }
        return;
    }

    if (mouse_pressed && cursor_x >= layout.toggle_left && cursor_x <= layout.toggle_right) {
        if (std::abs(cursor_y - static_cast<double>(layout.vsync_y)) <= 20.0) {
            settings.vsync = !settings.vsync;
            return;
        }
        if (std::abs(cursor_y - static_cast<double>(layout.fullscreen_y)) <= 20.0) {
            settings.fullscreen = !settings.fullscreen;
            return;
        }
    }
    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) != GLFW_PRESS) return;
    if (cursor_x < layout.slider_left - 12 || cursor_x > layout.slider_right + 12) return;
    const auto slider_value = std::clamp(
        (cursor_x - static_cast<double>(layout.slider_left)) /
            static_cast<double>(layout.slider_right - layout.slider_left),
        0.0, 1.0);
    if (std::abs(cursor_y - static_cast<double>(layout.render_distance_y)) <= 22.0) {
        const auto range = std::max(1, settings.render_distance_scale_max - 4);
        settings.render_distance_chunks = 4 +
            static_cast<std::int32_t>(std::round(slider_value * range));
    } else if (std::abs(cursor_y - static_cast<double>(layout.smoothing_y)) <= 22.0) {
        const auto steps = static_cast<std::int32_t>(std::round(slider_value * 16.0));
        settings.distance_smoothing_start = 128.0F + static_cast<float>(steps * 32);
    } else if (std::abs(cursor_y - static_cast<double>(layout.fog_start_y)) <= 22.0) {
        const auto percentage = 55 + static_cast<std::int32_t>(std::round(slider_value * 37.0));
        settings.fog_start_fraction = static_cast<float>(percentage) * 0.01F;
    } else if (std::abs(cursor_y - static_cast<double>(layout.shadow_distance_y)) <= 22.0) {
        const auto steps = static_cast<std::int32_t>(std::round(slider_value * 12.0));
        settings.shadow_distance_blocks = 64 + steps * 16;
    }
}

} // namespace heartstead::app
