#pragma once

#include "engine/core/result.hpp"
#include "engine/math/matrix.hpp"
#include "engine/world/coords/world_position.hpp"

namespace heartstead::renderer {

struct RenderCamera {
    world::FloatingOrigin floating_origin{};

    // Camera position relative to floating_origin. Exact world coordinates stay in the origin.
    math::Vec3f local_position{};
    float yaw_radians = 0.0F;
    float pitch_radians = 0.0F;

    float vertical_fov_radians = 1.0472F;
    float near_plane = 0.05F;
    float far_plane = 2'000.0F;
    float aspect_ratio = 16.0F / 9.0F;

    math::Mat4f view = math::Mat4f::identity();
    math::Mat4f projection = math::Mat4f::identity();
    math::Mat4f view_projection = math::Mat4f::identity();

    [[nodiscard]] core::Status update_matrices() noexcept;
    [[nodiscard]] core::Status set_aspect_ratio(float aspect) noexcept;
    // Visibility bounds are expressed relative to floating_origin, then translated by
    // local_position before testing. Preserve the exact supplied view orientation and projection
    // while moving that test camera to the origin.
    [[nodiscard]] math::Mat4f camera_relative_view_projection() const noexcept;
    [[nodiscard]] math::Vec3f forward() const noexcept;
    [[nodiscard]] math::Vec3f right() const noexcept;
};

} // namespace heartstead::renderer
