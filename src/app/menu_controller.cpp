#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "heartstead/app/menu_controller.hpp"

#include <cctype>
#include <ctime>

namespace heartstead::app {

MenuController::MenuController() = default;

MenuAction MenuController::update(
    GLFWwindow* window,
    MenuUiState& state,
    std::int32_t framebuffer_width,
    std::int32_t framebuffer_height,
    bool mouse_pressed,
    const platform::InputEvents& events) const {
    if (state.creation_date.empty()) {
        const auto now = std::time(nullptr);
        std::tm local_time{};
#if defined(_WIN32)
        localtime_s(&local_time, &now);
#else
        localtime_r(&now, &local_time);
#endif
        char date[16]{};
        std::strftime(date, sizeof(date), "%Y-%m-%d", &local_time);
        state.creation_date = date;
    }
    double cursor_x = 0.0;
    double cursor_y = 0.0;
    glfwGetCursorPos(window, &cursor_x, &cursor_y);
    std::int32_t window_width = 0;
    std::int32_t window_height = 0;
    glfwGetWindowSize(window, &window_width, &window_height);
    if (window_width <= 0 || window_height <= 0) return MenuAction::none;
    cursor_x *= static_cast<double>(framebuffer_width) / static_cast<double>(window_width);
    cursor_y *= static_cast<double>(framebuffer_height) / static_cast<double>(window_height);

    const auto layout = MenuLayout::from_framebuffer(
        state.screen, framebuffer_width, framebuffer_height);
    state.hovered_control = -1;
    if (layout.primary.contains(cursor_x, cursor_y)) state.hovered_control = 0;
    if (layout.secondary.contains(cursor_x, cursor_y)) state.hovered_control = 1;
    if (layout.tertiary.contains(cursor_x, cursor_y)) state.hovered_control = 2;
    if (layout.quaternary.contains(cursor_x, cursor_y)) state.hovered_control = 3;
    if (state.screen == MenuScreen::create_world && layout.text_field.contains(cursor_x, cursor_y))
        state.hovered_control = 4;
    if (state.screen == MenuScreen::singleplayer_worlds) {
        for (std::size_t index = 0;
             index < layout.world_tabs.size() && index < state.saved_worlds.size(); ++index) {
            if (layout.world_tabs[index].contains(cursor_x, cursor_y))
                state.hovered_control = 10 + static_cast<std::int32_t>(index);
        }
    }

    if (state.editing_world_name) {
        for (const auto character : events.text_characters) {
            if (state.world_name.size() >= 24U) break;
            const auto value = static_cast<unsigned char>(character);
            if (std::isalnum(value) != 0 || character == ' ' || character == '-' || character == '_')
                state.world_name.push_back(static_cast<char>(std::toupper(value)));
        }
        if (events.numeric_backspace && !state.world_name.empty()) state.world_name.pop_back();
        if (events.numeric_accept) state.editing_world_name = false;
    }

    if (events.settings_toggle) {
        if (state.screen == MenuScreen::pause) return MenuAction::resume;
        if (state.editing_world_name) {
            state.editing_world_name = false;
        } else if (state.screen == MenuScreen::create_world) {
            state.screen = MenuScreen::singleplayer_worlds;
        } else if (state.screen != MenuScreen::main) {
            state.screen = MenuScreen::main;
        }
        return MenuAction::none;
    }
    if (!mouse_pressed) return MenuAction::none;

    if (state.screen == MenuScreen::main) {
        if (state.hovered_control == 0) {
            state.screen = MenuScreen::singleplayer_worlds;
            return MenuAction::refresh_worlds;
        }
        if (state.hovered_control == 1) state.screen = MenuScreen::multiplayer;
        if (state.hovered_control == 2) return MenuAction::open_settings;
        if (state.hovered_control == 3) return MenuAction::quit;
        return MenuAction::none;
    }
    if (state.screen == MenuScreen::singleplayer_worlds) {
        if (state.hovered_control >= 10) {
            state.selected_world = state.hovered_control - 10;
            return MenuAction::none;
        }
        if (state.hovered_control == 0 && state.selected_world >= 0 &&
            static_cast<std::size_t>(state.selected_world) < state.saved_worlds.size())
            return MenuAction::play_world;
        if (state.hovered_control == 1) {
            state.screen = MenuScreen::create_world;
            state.world_name = "NEW WORLD";
            state.editing_world_name = false;
        }
        if (state.hovered_control == 2) state.screen = MenuScreen::main;
        return MenuAction::none;
    }
    if (state.screen == MenuScreen::create_world) {
        if (state.hovered_control == 4) {
            if (!state.editing_world_name && state.world_name == "NEW WORLD")
                state.world_name.clear();
            state.editing_world_name = true;
            return MenuAction::none;
        }
        if (state.hovered_control == 0 && !state.world_name.empty())
            return MenuAction::create_world;
        if (state.hovered_control == 1) state.screen = MenuScreen::singleplayer_worlds;
        return MenuAction::none;
    }
    if (state.screen == MenuScreen::pause) {
        if (state.hovered_control == 0) return MenuAction::open_settings;
        if (state.hovered_control == 1) return MenuAction::return_main_menu;
        return MenuAction::none;
    }
    if (state.hovered_control == 0)
        state.multiplayer_status = "MULTIPLAYER NETWORKING IS COMING NEXT";
    if (state.hovered_control == 1) state.screen = MenuScreen::main;
    return MenuAction::none;
}

} // namespace heartstead::app
