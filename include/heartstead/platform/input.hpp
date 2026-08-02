#pragma once

#include "heartstead/game/player.hpp"

#include <string>

struct GLFWwindow;

namespace heartstead::platform {

struct InputEvents {
    bool settings_toggle{};
    bool view_toggle{};
    bool debug_toggle{};
    bool flight_toggle{};
    bool numeric_accept{};
    bool numeric_backspace{};
    std::string numeric_characters;
    std::string text_characters;
};

class InputRouter {
public:
    explicit InputRouter(GLFWwindow* window);
    ~InputRouter();

    InputRouter(const InputRouter&) = delete;
    InputRouter& operator=(const InputRouter&) = delete;

    [[nodiscard]] InputEvents consume_events();
    [[nodiscard]] game::PlayerInput player_input() const noexcept;

private:
    static void key_callback(GLFWwindow* window, int key, int scan_code, int action, int modifiers);
    static void character_callback(GLFWwindow* window, unsigned int codepoint);

    GLFWwindow* window_{};
    InputEvents events_;
};

} // namespace heartstead::platform
