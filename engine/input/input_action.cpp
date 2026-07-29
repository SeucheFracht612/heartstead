#include "engine/input/input_action.hpp"

#include <algorithm>

namespace heartstead::input {

namespace {

template <typename T> [[nodiscard]] bool contains(std::span<const T> values, T value) noexcept {
    return std::ranges::find(values, value) != values.end();
}

[[nodiscard]] constexpr std::size_t index(InputAction action) noexcept {
    return static_cast<std::size_t>(action);
}

} // namespace

const InputActionState& InputActionFrame::operator[](InputAction action) const noexcept {
    return states[index(action)];
}

core::Status InputBinding::validate() const {
    if (action >= InputAction::count) {
        return core::Status::failure("input_binding.invalid_action",
                                     "input binding action is invalid");
    }
    const auto has_key = key != platform::KeyCode::unknown;
    const auto has_mouse = mouse_button != platform::MouseButton::unknown;
    if (has_key == has_mouse) {
        return core::Status::failure("input_binding.invalid_source",
                                     "input binding must select exactly one input source");
    }
    return core::Status::ok();
}

InputBinding InputBinding::keyboard(InputAction action, InputContext context,
                                    platform::KeyCode key) noexcept {
    return {action, context, key, platform::MouseButton::unknown};
}

InputBinding InputBinding::mouse(InputAction action, InputContext context,
                                 platform::MouseButton button) noexcept {
    return {action, context, platform::KeyCode::unknown, button};
}

InputActionMap InputActionMap::gameplay_defaults() {
    InputActionMap result;
    const auto add_key = [&result](InputAction action, platform::KeyCode key) {
        (void)result.bind(InputBinding::keyboard(action, InputContext::gameplay, key));
    };
    add_key(InputAction::move_forward, platform::KeyCode::w);
    add_key(InputAction::move_backward, platform::KeyCode::s);
    add_key(InputAction::move_left, platform::KeyCode::a);
    add_key(InputAction::move_right, platform::KeyCode::d);
    add_key(InputAction::jump, platform::KeyCode::space);
    add_key(InputAction::sprint, platform::KeyCode::left_shift);
    add_key(InputAction::crouch, platform::KeyCode::left_control);
    add_key(InputAction::dash, platform::KeyCode::q);
    add_key(InputAction::roll, platform::KeyCode::left_alt);
    add_key(InputAction::interact, platform::KeyCode::e);
    add_key(InputAction::open_inventory, platform::KeyCode::tab);
    add_key(InputAction::close_or_pause, platform::KeyCode::escape);
    add_key(InputAction::hotbar_1, platform::KeyCode::digit_1);
    add_key(InputAction::hotbar_2, platform::KeyCode::digit_2);
    add_key(InputAction::hotbar_3, platform::KeyCode::digit_3);
    add_key(InputAction::hotbar_4, platform::KeyCode::digit_4);
    add_key(InputAction::hotbar_5, platform::KeyCode::digit_5);
    add_key(InputAction::hotbar_6, platform::KeyCode::digit_6);
    add_key(InputAction::hotbar_7, platform::KeyCode::digit_7);
    add_key(InputAction::hotbar_8, platform::KeyCode::digit_8);
    add_key(InputAction::hotbar_9, platform::KeyCode::digit_9);
    add_key(InputAction::drop_item, platform::KeyCode::f5);
    add_key(InputAction::toggle_debug, platform::KeyCode::f3);
    add_key(InputAction::toggle_camera, platform::KeyCode::f1);
    add_key(InputAction::toggle_debug_geometry, platform::KeyCode::f4);
    (void)result.bind(InputBinding::mouse(InputAction::primary_action, InputContext::gameplay,
                                          platform::MouseButton::left));
    (void)result.bind(InputBinding::mouse(InputAction::secondary_action, InputContext::gameplay,
                                          platform::MouseButton::right));
    (void)result.bind(InputBinding::keyboard(InputAction::close_or_pause, InputContext::inventory,
                                             platform::KeyCode::escape));
    (void)result.bind(InputBinding::keyboard(InputAction::open_inventory, InputContext::inventory,
                                             platform::KeyCode::tab));
    (void)result.bind(InputBinding::keyboard(InputAction::close_or_pause, InputContext::menu,
                                             platform::KeyCode::escape));
    return result;
}

core::Status InputActionMap::bind(InputBinding binding) {
    auto status = binding.validate();
    if (!status) {
        return status;
    }
    if (std::ranges::any_of(bindings_, [&binding](const InputBinding& current) {
            return current.context == binding.context && current.key == binding.key &&
                   current.mouse_button == binding.mouse_button;
        })) {
        return core::Status::failure("input_action_map.duplicate_source",
                                     "input source is already bound in this context");
    }
    bindings_.push_back(binding);
    return core::Status::ok();
}

core::Status InputActionMap::rebind(InputBinding binding) {
    auto status = binding.validate();
    if (!status) {
        return status;
    }
    auto replacement = bindings_;
    replacement.erase(std::remove_if(replacement.begin(), replacement.end(),
                                     [&binding](const InputBinding& current) {
                                         return current.action == binding.action &&
                                                current.context == binding.context;
                                     }),
                      replacement.end());
    if (std::ranges::any_of(replacement, [&binding](const InputBinding& current) {
            return current.context == binding.context && current.key == binding.key &&
                   current.mouse_button == binding.mouse_button;
        })) {
        return core::Status::failure("input_action_map.duplicate_source",
                                     "input source is already bound in this context");
    }
    replacement.push_back(binding);
    bindings_ = std::move(replacement);
    return core::Status::ok();
}

std::size_t InputActionMap::unbind(InputAction action, InputContext context) noexcept {
    const auto before = bindings_.size();
    bindings_.erase(std::remove_if(bindings_.begin(), bindings_.end(),
                                   [action, context](const InputBinding& current) {
                                       return current.action == action &&
                                              current.context == context;
                                   }),
                    bindings_.end());
    return before - bindings_.size();
}

void InputActionMap::set_context(InputContext context) noexcept {
    context_ = context;
}

InputContext InputActionMap::context() const noexcept {
    return context_;
}

std::span<const InputBinding> InputActionMap::bindings() const noexcept {
    return bindings_;
}

InputActionFrame InputActionMap::evaluate(const platform::WindowInputSnapshot& input) const {
    InputActionFrame result;
    result.context = context_;
    result.look_delta_x = input.mouse_delta_x;
    result.look_delta_y = input.mouse_delta_y;
    result.wheel_delta = input.wheel_delta_y;
    for (const auto& binding : bindings_) {
        if (binding.context != context_) {
            continue;
        }
        auto& state = result.states[index(binding.action)];
        if (binding.key != platform::KeyCode::unknown) {
            state.held = state.held ||
                         contains(std::span<const platform::KeyCode>(input.down_keys), binding.key);
            state.pressed =
                state.pressed ||
                contains(std::span<const platform::KeyCode>(input.pressed_keys), binding.key);
            state.released =
                state.released ||
                contains(std::span<const platform::KeyCode>(input.released_keys), binding.key);
        } else {
            state.held = state.held ||
                         contains(std::span<const platform::MouseButton>(input.down_mouse_buttons),
                                  binding.mouse_button);
            state.pressed =
                state.pressed ||
                contains(std::span<const platform::MouseButton>(input.pressed_mouse_buttons),
                         binding.mouse_button);
            state.released =
                state.released ||
                contains(std::span<const platform::MouseButton>(input.released_mouse_buttons),
                         binding.mouse_button);
        }
    }
    return result;
}

std::string_view input_action_name(InputAction action) noexcept {
    constexpr std::array names{
        "move_forward",   "move_backward",    "move_left",      "move_right",     "jump",
        "sprint",         "crouch",           "dash",           "roll",           "interact",
        "primary_action", "secondary_action", "open_inventory", "close_or_pause", "hotbar_1",
        "hotbar_2",       "hotbar_3",         "hotbar_4",       "hotbar_5",       "hotbar_6",
        "hotbar_7",       "hotbar_8",         "hotbar_9",       "drop_item",      "toggle_debug",
        "toggle_camera",  "toggle_debug_geometry",
    };
    const auto action_index = index(action);
    return action_index < names.size() ? names[action_index] : "unknown";
}

std::string_view input_context_name(InputContext context) noexcept {
    switch (context) {
    case InputContext::gameplay:
        return "gameplay";
    case InputContext::inventory:
        return "inventory";
    case InputContext::menu:
        return "menu";
    }
    return "unknown";
}

} // namespace heartstead::input
