#include "engine/renderer/render_camera.hpp"

#include <cmath>
#include <numbers>

namespace heartstead::renderer {

core::Status RenderCamera::update_matrices() noexcept {
    if (!local_position.is_finite() || !std::isfinite(yaw_radians) ||
        !std::isfinite(pitch_radians) || !std::isfinite(vertical_fov_radians) ||
        !std::isfinite(aspect_ratio) || !std::isfinite(near_plane) ||
        !std::isfinite(far_plane) || aspect_ratio <= 0.0F || near_plane <= 0.0F ||
        far_plane <= near_plane || vertical_fov_radians <= 0.0F ||
        vertical_fov_radians >= std::numbers::pi_v<float> ||
        pitch_radians <= -std::numbers::pi_v<float> * 0.5F ||
        pitch_radians >= std::numbers::pi_v<float> * 0.5F) {
        return core::Status::failure("renderer.invalid_camera",
                                     "render camera parameters must define a finite perspective");
    }

    view = math::view_matrix(local_position, yaw_radians, pitch_radians);
    projection = math::perspective_projection(vertical_fov_radians, aspect_ratio, near_plane,
                                              far_plane);
    view_projection = projection * view;
    if (!view_projection.is_finite()) {
        return core::Status::failure("renderer.invalid_camera_matrix",
                                     "render camera produced a non-finite matrix");
    }
    return core::Status::ok();
}

core::Status RenderCamera::set_aspect_ratio(float aspect) noexcept {
    if (!std::isfinite(aspect) || aspect <= 0.0F) {
        return core::Status::failure("renderer.invalid_camera_aspect",
                                     "render camera aspect ratio must be finite and positive");
    }
    aspect_ratio = aspect;
    return update_matrices();
}

math::Mat4f RenderCamera::camera_relative_view_projection() const noexcept {
    auto relative_view = view;
    relative_view.at(0, 3) = 0.0F;
    relative_view.at(1, 3) = 0.0F;
    relative_view.at(2, 3) = 0.0F;
    return projection * relative_view;
}

math::Vec3f RenderCamera::forward() const noexcept {
    return math::camera_forward(yaw_radians, pitch_radians);
}

math::Vec3f RenderCamera::right() const noexcept {
    return math::camera_right(yaw_radians);
}

} // namespace heartstead::renderer
