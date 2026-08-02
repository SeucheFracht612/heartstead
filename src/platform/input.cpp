#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "heartstead/platform/input.hpp"

#include <utility>

namespace heartstead::platform {

InputRouter::InputRouter(GLFWwindow* window) : window_(window) {
    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, key_callback);
    glfwSetCharCallback(window_, character_callback);
}

InputRouter::~InputRouter() {
    if (window_ == nullptr) return;
    glfwSetKeyCallback(window_, nullptr);
    glfwSetCharCallback(window_, nullptr);
    glfwSetWindowUserPointer(window_, nullptr);
}

InputEvents InputRouter::consume_events() {
    auto result = std::move(events_);
    events_ = {};
    return result;
}

game::PlayerInput InputRouter::player_input() const noexcept {
    const auto pressed = [this](int key) noexcept {
        return glfwGetKey(window_, key) == GLFW_PRESS;
    };
    return {
        .forward = (pressed(GLFW_KEY_W) ? 1.0F : 0.0F) - (pressed(GLFW_KEY_S) ? 1.0F : 0.0F),
        .right = (pressed(GLFW_KEY_D) ? 1.0F : 0.0F) - (pressed(GLFW_KEY_A) ? 1.0F : 0.0F),
        .jump = pressed(GLFW_KEY_SPACE),
        .descend = pressed(GLFW_KEY_LEFT_CONTROL),
        .sprint = pressed(GLFW_KEY_LEFT_SHIFT),
    };
}

void InputRouter::key_callback(GLFWwindow* window, int key, int, int action, int) {
    auto* input = static_cast<InputRouter*>(glfwGetWindowUserPointer(window));
    if (input == nullptr) return;
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE) input->events_.settings_toggle = true;
        if (key == GLFW_KEY_F5) input->events_.view_toggle = true;
        if (key == GLFW_KEY_F3) input->events_.debug_toggle = true;
        if (key == GLFW_KEY_F) input->events_.flight_toggle = true;
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)
            input->events_.numeric_accept = true;
    }
    if (key == GLFW_KEY_BACKSPACE && (action == GLFW_PRESS || action == GLFW_REPEAT))
        input->events_.numeric_backspace = true;
}

void InputRouter::character_callback(GLFWwindow* window, unsigned int codepoint) {
    auto* input = static_cast<InputRouter*>(glfwGetWindowUserPointer(window));
    if (input == nullptr) return;
    if (codepoint >= 32U && codepoint <= 126U)
        input->events_.text_characters.push_back(static_cast<char>(codepoint));
    if (codepoint >= static_cast<unsigned int>('0') && codepoint <= static_cast<unsigned int>('9'))
        input->events_.numeric_characters.push_back(static_cast<char>(codepoint));
}

} // namespace heartstead::platform
