#pragma once

#include "heartstead/platform/input.hpp"
#include "heartstead/render/menu_ui.hpp"

#include <cstdint>

struct GLFWwindow;

namespace heartstead::app {

enum class MenuAction {
    none,
    open_settings,
    refresh_worlds,
    play_world,
    create_world,
    return_main_menu,
    resume,
    quit,
};

class MenuController {
public:
    MenuController();

    [[nodiscard]] MenuAction update(
        GLFWwindow* window,
        MenuUiState& state,
        std::int32_t framebuffer_width,
        std::int32_t framebuffer_height,
        bool mouse_pressed,
        const platform::InputEvents& events) const;
};

} // namespace heartstead::app
