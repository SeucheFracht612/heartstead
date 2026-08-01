#include "engine/renderer/visibility/hierarchical_depth_occlusion.hpp"

#include "engine/math/matrix.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>

namespace heartstead::renderer {

namespace {

struct ProjectedBounds {
    float minimum_x = 1.0F;
    float minimum_y = 1.0F;
    float maximum_x = 0.0F;
    float maximum_y = 0.0F;
    float nearest_depth = 1.0F;
    float farthest_depth = 0.0F;
};

[[nodiscard]] bool project_bounds(const math::Mat4f& view_projection,
                                  const math::Bounds3f& bounds,
                                  ProjectedBounds& projected) noexcept {
    if (!bounds.is_valid()) {
        return false;
    }
    const std::array corners{
        math::Vec3f{bounds.min.x, bounds.min.y, bounds.min.z},
        math::Vec3f{bounds.max.x, bounds.min.y, bounds.min.z},
        math::Vec3f{bounds.min.x, bounds.max.y, bounds.min.z},
        math::Vec3f{bounds.max.x, bounds.max.y, bounds.min.z},
        math::Vec3f{bounds.min.x, bounds.min.y, bounds.max.z},
        math::Vec3f{bounds.max.x, bounds.min.y, bounds.max.z},
        math::Vec3f{bounds.min.x, bounds.max.y, bounds.max.z},
        math::Vec3f{bounds.max.x, bounds.max.y, bounds.max.z},
    };
    for (const auto corner : corners) {
        const auto clip = view_projection * math::Vec4f{corner.x, corner.y, corner.z, 1.0F};
        // Near-plane intersections are kept visible. Clipping an occluder here could otherwise
        // enlarge it and create false-positive occlusion around the camera.
        if (!clip.is_finite() || clip.w <= 0.0001F) {
            return false;
        }
        const auto inverse_w = 1.0F / clip.w;
        const auto x = clip.x * inverse_w * 0.5F + 0.5F;
        const auto y = clip.y * inverse_w * 0.5F + 0.5F;
        const auto depth = clip.z * inverse_w;
        projected.minimum_x = std::min(projected.minimum_x, x);
        projected.minimum_y = std::min(projected.minimum_y, y);
        projected.maximum_x = std::max(projected.maximum_x, x);
        projected.maximum_y = std::max(projected.maximum_y, y);
        projected.nearest_depth = std::min(projected.nearest_depth, depth);
        projected.farthest_depth = std::max(projected.farthest_depth, depth);
    }
    if (projected.maximum_x <= 0.0F || projected.maximum_y <= 0.0F ||
        projected.minimum_x >= 1.0F || projected.minimum_y >= 1.0F ||
        projected.farthest_depth <= 0.0F || projected.nearest_depth >= 1.0F) {
        return false;
    }
    projected.minimum_x = std::clamp(projected.minimum_x, 0.0F, 1.0F);
    projected.minimum_y = std::clamp(projected.minimum_y, 0.0F, 1.0F);
    projected.maximum_x = std::clamp(projected.maximum_x, 0.0F, 1.0F);
    projected.maximum_y = std::clamp(projected.maximum_y, 0.0F, 1.0F);
    projected.nearest_depth = std::clamp(projected.nearest_depth, 0.0F, 1.0F);
    projected.farthest_depth = std::clamp(projected.farthest_depth, 0.0F, 1.0F);
    return projected.maximum_x > projected.minimum_x &&
           projected.maximum_y > projected.minimum_y;
}

[[nodiscard]] std::uint32_t pixel_min(float coordinate, std::uint32_t extent) noexcept {
    return std::min(static_cast<std::uint32_t>(coordinate * static_cast<float>(extent)),
                    extent - 1U);
}

[[nodiscard]] std::uint32_t pixel_max(float coordinate, std::uint32_t extent) noexcept {
    const auto value = static_cast<std::uint32_t>(
        std::ceil(coordinate * static_cast<float>(extent)));
    return std::min(value == 0U ? 0U : value - 1U, extent - 1U);
}

} // namespace

core::Status HierarchicalDepthOcclusionConfig::validate() const {
    if (width == 0U || height == 0U || !std::isfinite(depth_bias) || depth_bias < 0.0F ||
        depth_bias >= 1.0F || confirmation_frames == 0U ||
        !std::isfinite(camera_cut_distance) || camera_cut_distance <= 0.0F ||
        !std::isfinite(camera_cut_angle_radians) || camera_cut_angle_radians <= 0.0F) {
        return core::Status::failure(
            "visibility.invalid_hierarchical_depth_config",
            "hierarchical depth dimensions, bias, history, and camera-cut limits are invalid");
    }
    return core::Status::ok();
}

core::Status HierarchicalDepthOcclusion::initialize(HierarchicalDepthOcclusionConfig config) {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    config_ = config;
    mips_.clear();
    auto width = config.width;
    auto height = config.height;
    while (true) {
        mips_.push_back({width, height,
                         std::vector<float>(static_cast<std::size_t>(width) * height, 1.0F)});
        if (width == 1U && height == 1U) {
            break;
        }
        width = std::max(1U, (width + 1U) / 2U);
        height = std::max(1U, (height + 1U) / 2U);
    }
    history_.clear();
    has_previous_camera_ = false;
    stats_ = {};
    stats_.mip_levels = static_cast<std::uint32_t>(mips_.size());
    return core::Status::ok();
}

core::Status HierarchicalDepthOcclusion::rebuild(
    const RenderCamera& camera, std::span<const math::Bounds3f> occluders) {
    if (!is_initialized() || !camera.view_projection.is_finite() ||
        !std::ranges::all_of(occluders, [](const auto& value) { return value.is_valid(); })) {
        return core::Status::failure("visibility.invalid_hierarchical_depth_input",
                                     "initialized HZB, finite camera, and valid bounds required");
    }
    if (has_previous_camera_) {
        const auto movement = math::length(camera.local_position - previous_camera_.local_position);
        const auto angular_change = std::max(std::abs(camera.yaw_radians - previous_camera_.yaw_radians),
                                             std::abs(camera.pitch_radians - previous_camera_.pitch_radians));
        if (camera.floating_origin.block != previous_camera_.floating_origin.block ||
            movement > config_.camera_cut_distance ||
            angular_change > config_.camera_cut_angle_radians) {
            reset_history();
            ++stats_.camera_cut_resets;
        }
    }
    previous_camera_ = camera;
    has_previous_camera_ = true;
    stats_.submitted_occluders = static_cast<std::uint32_t>(occluders.size());
    stats_.rasterized_occluders = 0;
    stats_.tested_bounds = 0;
    stats_.raw_occluded_bounds = 0;
    stats_.confirmed_occluded_bounds = 0;
    std::ranges::fill(mips_.front().depth, 1.0F);
    for (const auto& occluder : occluders) {
        ProjectedBounds projected;
        if (!project_bounds(camera.view_projection, occluder, projected)) {
            continue;
        }
        const auto min_x = pixel_min(projected.minimum_x, mips_.front().width);
        const auto max_x = pixel_max(projected.maximum_x, mips_.front().width);
        const auto min_y = pixel_min(projected.minimum_y, mips_.front().height);
        const auto max_y = pixel_max(projected.maximum_y, mips_.front().height);
        for (auto y = min_y; y <= max_y; ++y) {
            for (auto x = min_x; x <= max_x; ++x) {
                auto& depth = mips_.front().depth[static_cast<std::size_t>(y) *
                                                      mips_.front().width +
                                                  x];
                depth = std::min(depth, projected.farthest_depth);
            }
        }
        ++stats_.rasterized_occluders;
    }
    for (std::size_t level = 1; level < mips_.size(); ++level) {
        const auto& source = mips_[level - 1U];
        auto& target = mips_[level];
        for (std::uint32_t y = 0; y < target.height; ++y) {
            for (std::uint32_t x = 0; x < target.width; ++x) {
                float maximum = 0.0F;
                for (std::uint32_t offset_y = 0; offset_y < 2U; ++offset_y) {
                    for (std::uint32_t offset_x = 0; offset_x < 2U; ++offset_x) {
                        const auto source_x = std::min(x * 2U + offset_x, source.width - 1U);
                        const auto source_y = std::min(y * 2U + offset_y, source.height - 1U);
                        maximum = std::max(maximum,
                                           source.depth[static_cast<std::size_t>(source_y) *
                                                            source.width +
                                                        source_x]);
                    }
                }
                target.depth[static_cast<std::size_t>(y) * target.width + x] = maximum;
            }
        }
    }
    return core::Status::ok();
}

bool HierarchicalDepthOcclusion::query(std::uint64_t object_id,
                                       const math::Bounds3f& bounds) {
    ++stats_.tested_bounds;
    ProjectedBounds projected;
    bool raw_occluded = false;
    if (object_id != 0U && project_bounds(previous_camera_.view_projection, bounds, projected)) {
        auto min_x = pixel_min(projected.minimum_x, mips_.front().width);
        auto max_x = pixel_max(projected.maximum_x, mips_.front().width);
        auto min_y = pixel_min(projected.minimum_y, mips_.front().height);
        auto max_y = pixel_max(projected.maximum_y, mips_.front().height);
        std::size_t level = 0;
        while (level + 1U < mips_.size() &&
               std::max(max_x - min_x + 1U, max_y - min_y + 1U) > 4U) {
            min_x /= 2U;
            max_x /= 2U;
            min_y /= 2U;
            max_y /= 2U;
            ++level;
        }
        const auto& mip = mips_[level];
        float maximum_depth = 0.0F;
        for (auto y = min_y; y <= std::min(max_y, mip.height - 1U); ++y) {
            for (auto x = min_x; x <= std::min(max_x, mip.width - 1U); ++x) {
                maximum_depth =
                    std::max(maximum_depth,
                             mip.depth[static_cast<std::size_t>(y) * mip.width + x]);
            }
        }
        raw_occluded = maximum_depth + config_.depth_bias < projected.nearest_depth;
    }
    auto& history = history_[object_id];
    if (raw_occluded) {
        ++stats_.raw_occluded_bounds;
        history.consecutive_occluded_frames =
            std::min(history.consecutive_occluded_frames + 1U, config_.confirmation_frames);
        history.confirmed = history.consecutive_occluded_frames >= config_.confirmation_frames;
    } else {
        history = {};
    }
    if (history.confirmed) {
        ++stats_.confirmed_occluded_bounds;
    }
    return history.confirmed;
}

void HierarchicalDepthOcclusion::erase(std::uint64_t object_id) noexcept {
    history_.erase(object_id);
}

void HierarchicalDepthOcclusion::reset_history() noexcept {
    history_.clear();
}

bool HierarchicalDepthOcclusion::is_initialized() const noexcept {
    return !mips_.empty();
}

const HierarchicalDepthOcclusionStats& HierarchicalDepthOcclusion::stats() const noexcept {
    return stats_;
}

} // namespace heartstead::renderer
