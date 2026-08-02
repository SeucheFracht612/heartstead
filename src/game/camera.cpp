#include "heartstead/game/camera.hpp"

#include <algorithm>
#include <cmath>

namespace heartstead::game {

Float3 Camera::forward() const noexcept {
    const auto horizontal = std::cos(pitch);
    return normalize({
        horizontal * std::cos(yaw),
        std::sin(pitch),
        horizontal * std::sin(yaw),
    });
}

void update_view_angles(
    Camera& camera,
    MouseLookState& state,
    double mouse_x,
    double mouse_y,
    bool reset_mouse) noexcept {
    constexpr float mouse_sensitivity = 0.0022F;
    constexpr float pitch_limit = 1.55334F;
    if (reset_mouse || !state.initialized) {
        state.previous_x = mouse_x;
        state.previous_y = mouse_y;
        state.initialized = true;
    }

    const auto delta_x = static_cast<float>(mouse_x - state.previous_x);
    const auto delta_y = static_cast<float>(mouse_y - state.previous_y);
    state.previous_x = mouse_x;
    state.previous_y = mouse_y;
    camera.yaw += delta_x * mouse_sensitivity;
    camera.pitch = std::clamp(camera.pitch - delta_y * mouse_sensitivity, -pitch_limit, pitch_limit);
}

} // namespace heartstead::game
