#pragma once

#include "heartstead/platform/input.hpp"
#include "heartstead/render/opengl_renderer.hpp"
#include "heartstead/render/video_settings.hpp"

#include <cstdint>

struct GLFWwindow;

namespace heartstead::app {

class VideoSettingsController {
public:
    void update(
        GLFWwindow* window,
        VideoSettings& settings,
        VideoSettingsUiState& ui_state,
        std::int32_t framebuffer_width,
        std::int32_t framebuffer_height,
        bool mouse_pressed,
        const platform::InputEvents& events);

private:
    double last_value_click_{-1.0};
};

} // namespace heartstead::app
