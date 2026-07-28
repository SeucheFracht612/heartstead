#pragma once

#include "engine/core/result.hpp"
#include "engine/platform/platform.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace heartstead::input {

enum class InputContext : std::uint8_t {
    gameplay,
    inventory,
    menu,
};

enum class InputAction : std::uint8_t {
    move_forward,
    move_backward,
    move_left,
    move_right,
    jump,
    sprint,
    crouch,
    dash,
    roll,
    interact,
    primary_action,
    secondary_action,
    open_inventory,
    close_or_pause,
    hotbar_1,
    hotbar_2,
    hotbar_3,
    hotbar_4,
    hotbar_5,
    hotbar_6,
    hotbar_7,
    hotbar_8,
    hotbar_9,
    drop_item,
    toggle_debug,
    count,
};

struct InputActionState {
    bool held = false;
    bool pressed = false;
    bool released = false;
};

struct InputActionFrame {
    InputContext context = InputContext::gameplay;
    std::array<InputActionState, static_cast<std::size_t>(InputAction::count)> states{};
    std::int32_t look_delta_x = 0;
    std::int32_t look_delta_y = 0;
    std::int32_t wheel_delta = 0;

    [[nodiscard]] const InputActionState& operator[](InputAction action) const noexcept;
};

struct InputBinding {
    InputAction action = InputAction::move_forward;
    InputContext context = InputContext::gameplay;
    platform::KeyCode key = platform::KeyCode::unknown;
    platform::MouseButton mouse_button = platform::MouseButton::unknown;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] static InputBinding keyboard(InputAction action, InputContext context,
                                               platform::KeyCode key) noexcept;
    [[nodiscard]] static InputBinding mouse(InputAction action, InputContext context,
                                            platform::MouseButton button) noexcept;
};

class InputActionMap {
  public:
    [[nodiscard]] static InputActionMap gameplay_defaults();

    [[nodiscard]] core::Status bind(InputBinding binding);
    [[nodiscard]] core::Status rebind(InputBinding binding);
    std::size_t unbind(InputAction action, InputContext context) noexcept;
    void set_context(InputContext context) noexcept;

    [[nodiscard]] InputContext context() const noexcept;
    [[nodiscard]] std::span<const InputBinding> bindings() const noexcept;
    [[nodiscard]] InputActionFrame evaluate(const platform::WindowInputSnapshot& input) const;

  private:
    std::vector<InputBinding> bindings_;
    InputContext context_ = InputContext::gameplay;
};

[[nodiscard]] std::string_view input_action_name(InputAction action) noexcept;
[[nodiscard]] std::string_view input_context_name(InputContext context) noexcept;

} // namespace heartstead::input
