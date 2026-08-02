#pragma once

#include "heartstead/core/math.hpp"

namespace heartstead::game {

struct Camera {
    Float3 position{0.0F, 80.0F, 0.0F};
    float yaw{-1.57079633F};
    float pitch{-0.22F};

    [[nodiscard]] Float3 forward() const noexcept;
};

enum class CameraMode { first_person, third_person };

struct MouseLookState {
    double previous_x{};
    double previous_y{};
    bool initialized{};
};

void update_view_angles(
    Camera& camera,
    MouseLookState& state,
    double mouse_x,
    double mouse_y,
    bool reset_mouse) noexcept;

} // namespace heartstead::game
