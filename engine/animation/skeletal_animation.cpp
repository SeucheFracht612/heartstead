#include "engine/animation/skeletal_animation.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace heartstead::animation {

namespace {

constexpr float minimum_quaternion_length_squared = 1.0e-12F;
constexpr float minimum_affine_determinant = 1.0e-8F;

[[nodiscard]] assets::ModelQuaternion normalized(assets::ModelQuaternion value) noexcept {
    const auto length_squared = value.length_squared();
    if (!std::isfinite(length_squared) || length_squared <= minimum_quaternion_length_squared) {
        return {};
    }
    const auto inverse_length = 1.0F / std::sqrt(length_squared);
    value.x *= inverse_length;
    value.y *= inverse_length;
    value.z *= inverse_length;
    value.w *= inverse_length;
    return value;
}

[[nodiscard]] assets::ModelQuaternion slerp(assets::ModelQuaternion first,
                                            assets::ModelQuaternion second, float alpha) noexcept {
    auto cosine = first.x * second.x + first.y * second.y + first.z * second.z + first.w * second.w;
    if (cosine < 0.0F) {
        second.x = -second.x;
        second.y = -second.y;
        second.z = -second.z;
        second.w = -second.w;
        cosine = -cosine;
    }
    cosine = std::clamp(cosine, -1.0F, 1.0F);
    if (cosine > 0.9995F) {
        return normalized({
            first.x + (second.x - first.x) * alpha,
            first.y + (second.y - first.y) * alpha,
            first.z + (second.z - first.z) * alpha,
            first.w + (second.w - first.w) * alpha,
        });
    }
    const auto angle = std::acos(cosine);
    const auto sine = std::sin(angle);
    if (!std::isfinite(sine) || std::abs(sine) <= std::numeric_limits<float>::epsilon()) {
        return first;
    }
    const auto first_weight = std::sin((1.0F - alpha) * angle) / sine;
    const auto second_weight = std::sin(alpha * angle) / sine;
    return normalized({
        first.x * first_weight + second.x * second_weight,
        first.y * first_weight + second.y * second_weight,
        first.z * first_weight + second.z * second_weight,
        first.w * first_weight + second.w * second_weight,
    });
}

[[nodiscard]] assets::ModelQuaternion multiply(assets::ModelQuaternion left,
                                                assets::ModelQuaternion right) noexcept {
    return normalized({
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
    });
}

[[nodiscard]] assets::ModelQuaternion inverse_rotation(
    assets::ModelQuaternion value) noexcept {
    value = normalized(value);
    return {-value.x, -value.y, -value.z, value.w};
}

[[nodiscard]] math::Vec4f linear(math::Vec4f first, math::Vec4f second, float alpha) noexcept {
    return {
        first.x + (second.x - first.x) * alpha,
        first.y + (second.y - first.y) * alpha,
        first.z + (second.z - first.z) * alpha,
        first.w + (second.w - first.w) * alpha,
    };
}

[[nodiscard]] math::Vec4f cubic_hermite(math::Vec4f first, math::Vec4f first_tangent,
                                        math::Vec4f second, math::Vec4f second_tangent, float alpha,
                                        float keyframe_span) noexcept {
    const auto alpha_squared = alpha * alpha;
    const auto alpha_cubed = alpha_squared * alpha;
    const auto first_basis = 2.0F * alpha_cubed - 3.0F * alpha_squared + 1.0F;
    const auto first_tangent_basis = alpha_cubed - 2.0F * alpha_squared + alpha;
    const auto second_basis = -2.0F * alpha_cubed + 3.0F * alpha_squared;
    const auto second_tangent_basis = alpha_cubed - alpha_squared;
    return {
        first_basis * first.x + first_tangent_basis * keyframe_span * first_tangent.x +
            second_basis * second.x + second_tangent_basis * keyframe_span * second_tangent.x,
        first_basis * first.y + first_tangent_basis * keyframe_span * first_tangent.y +
            second_basis * second.y + second_tangent_basis * keyframe_span * second_tangent.y,
        first_basis * first.z + first_tangent_basis * keyframe_span * first_tangent.z +
            second_basis * second.z + second_tangent_basis * keyframe_span * second_tangent.z,
        first_basis * first.w + first_tangent_basis * keyframe_span * first_tangent.w +
            second_basis * second.w + second_tangent_basis * keyframe_span * second_tangent.w,
    };
}

[[nodiscard]] math::Vec4f channel_value(const assets::ModelAnimationChannel& channel,
                                        float time_seconds) noexcept {
    const auto value_at = [&](std::size_t keyframe) {
        return channel
            .values[channel.interpolation == assets::ModelAnimationInterpolation::cubic_spline
                        ? keyframe * 3U + 1U
                        : keyframe];
    };
    if (time_seconds <= channel.times.front() || channel.times.size() == 1) {
        return value_at(0);
    }
    if (time_seconds >= channel.times.back()) {
        return value_at(channel.times.size() - 1U);
    }
    const auto upper = std::upper_bound(channel.times.begin(), channel.times.end(), time_seconds);
    const auto second_index = static_cast<std::size_t>(std::distance(channel.times.begin(), upper));
    const auto first_index = second_index - 1U;
    if (channel.interpolation == assets::ModelAnimationInterpolation::step) {
        return value_at(first_index);
    }
    const auto span = channel.times[second_index] - channel.times[first_index];
    const auto alpha = (time_seconds - channel.times[first_index]) / span;
    if (channel.interpolation == assets::ModelAnimationInterpolation::linear) {
        if (channel.path == assets::ModelAnimationPath::rotation) {
            const auto first = value_at(first_index);
            const auto second = value_at(second_index);
            const auto result = slerp({first.x, first.y, first.z, first.w},
                                      {second.x, second.y, second.z, second.w}, alpha);
            return {result.x, result.y, result.z, result.w};
        }
        return linear(value_at(first_index), value_at(second_index), alpha);
    }
    const auto first_value = channel.values[first_index * 3U + 1U];
    const auto first_tangent = channel.values[first_index * 3U + 2U];
    const auto second_tangent = channel.values[second_index * 3U];
    const auto second_value = channel.values[second_index * 3U + 1U];
    auto result =
        cubic_hermite(first_value, first_tangent, second_value, second_tangent, alpha, span);
    if (channel.path == assets::ModelAnimationPath::rotation) {
        const auto quaternion = normalized({result.x, result.y, result.z, result.w});
        result = {quaternion.x, quaternion.y, quaternion.z, quaternion.w};
    }
    return result;
}

[[nodiscard]] float weight_channel_value(const assets::ModelAnimationChannel& channel,
                                         std::size_t weight_index, float time_seconds) noexcept {
    const auto value_at = [&](std::size_t keyframe, std::size_t spline_component = 1U) {
        const auto frame_stride =
            static_cast<std::size_t>(channel.weight_count) *
            (channel.interpolation == assets::ModelAnimationInterpolation::cubic_spline ? 3U : 1U);
        const auto component_offset =
            channel.interpolation == assets::ModelAnimationInterpolation::cubic_spline
                ? spline_component * channel.weight_count
                : 0U;
        return channel.weight_values[keyframe * frame_stride + component_offset + weight_index];
    };
    if (time_seconds <= channel.times.front() || channel.times.size() == 1) {
        return value_at(0);
    }
    if (time_seconds >= channel.times.back()) {
        return value_at(channel.times.size() - 1U);
    }
    const auto upper = std::upper_bound(channel.times.begin(), channel.times.end(), time_seconds);
    const auto second_index = static_cast<std::size_t>(std::distance(channel.times.begin(), upper));
    const auto first_index = second_index - 1U;
    if (channel.interpolation == assets::ModelAnimationInterpolation::step) {
        return value_at(first_index);
    }
    const auto span = channel.times[second_index] - channel.times[first_index];
    const auto alpha = (time_seconds - channel.times[first_index]) / span;
    if (channel.interpolation == assets::ModelAnimationInterpolation::linear) {
        const auto first = value_at(first_index);
        return first + (value_at(second_index) - first) * alpha;
    }
    const auto alpha_squared = alpha * alpha;
    const auto alpha_cubed = alpha_squared * alpha;
    return (2.0F * alpha_cubed - 3.0F * alpha_squared + 1.0F) * value_at(first_index) +
           (alpha_cubed - 2.0F * alpha_squared + alpha) * span * value_at(first_index, 2U) +
           (-2.0F * alpha_cubed + 3.0F * alpha_squared) * value_at(second_index) +
           (alpha_cubed - alpha_squared) * span * value_at(second_index, 0U);
}

[[nodiscard]] float sampled_time(const assets::ModelAnimationClip& clip,
                                 const AnimationClipPlayback& playback) noexcept {
    if (!playback.looping || clip.duration_seconds <= 0.0F) {
        return std::clamp(playback.time_seconds, 0.0F, clip.duration_seconds);
    }
    auto result = std::fmod(playback.time_seconds, clip.duration_seconds);
    if (result < 0.0F) {
        result += clip.duration_seconds;
    }
    return result;
}

[[nodiscard]] math::Mat4f quaternion_matrix(assets::ModelQuaternion quaternion) noexcept {
    quaternion = normalized(quaternion);
    const auto xx = quaternion.x * quaternion.x;
    const auto yy = quaternion.y * quaternion.y;
    const auto zz = quaternion.z * quaternion.z;
    const auto xy = quaternion.x * quaternion.y;
    const auto xz = quaternion.x * quaternion.z;
    const auto yz = quaternion.y * quaternion.z;
    const auto xw = quaternion.x * quaternion.w;
    const auto yw = quaternion.y * quaternion.w;
    const auto zw = quaternion.z * quaternion.w;
    auto result = math::Mat4f::identity();
    result.at(0, 0) = 1.0F - 2.0F * (yy + zz);
    result.at(0, 1) = 2.0F * (xy - zw);
    result.at(0, 2) = 2.0F * (xz + yw);
    result.at(1, 0) = 2.0F * (xy + zw);
    result.at(1, 1) = 1.0F - 2.0F * (xx + zz);
    result.at(1, 2) = 2.0F * (yz - xw);
    result.at(2, 0) = 2.0F * (xz - yw);
    result.at(2, 1) = 2.0F * (yz + xw);
    result.at(2, 2) = 1.0F - 2.0F * (xx + yy);
    return result;
}

[[nodiscard]] math::Mat4f node_matrix(const assets::ModelNodeTransform& transform) noexcept {
    return math::translation_matrix(transform.translation) * quaternion_matrix(transform.rotation) *
           math::scale_matrix(transform.scale);
}

[[nodiscard]] core::Result<math::Mat4f> inverse_affine(const math::Mat4f& matrix) {
    const auto determinant =
        matrix.at(0, 0) * (matrix.at(1, 1) * matrix.at(2, 2) - matrix.at(1, 2) * matrix.at(2, 1)) -
        matrix.at(0, 1) * (matrix.at(1, 0) * matrix.at(2, 2) - matrix.at(1, 2) * matrix.at(2, 0)) +
        matrix.at(0, 2) * (matrix.at(1, 0) * matrix.at(2, 1) - matrix.at(1, 1) * matrix.at(2, 0));
    if (!std::isfinite(determinant) || std::abs(determinant) <= minimum_affine_determinant ||
        std::abs(matrix.at(3, 0)) > minimum_affine_determinant ||
        std::abs(matrix.at(3, 1)) > minimum_affine_determinant ||
        std::abs(matrix.at(3, 2)) > minimum_affine_determinant ||
        std::abs(matrix.at(3, 3) - 1.0F) > minimum_affine_determinant) {
        return core::Result<math::Mat4f>::failure(
            "skeletal_animation.non_invertible_mesh_node",
            "skinning requires an invertible affine mesh-node transform");
    }
    const auto inverse_determinant = 1.0F / determinant;
    math::Mat4f inverse = math::Mat4f::identity();
    inverse.at(0, 0) = (matrix.at(1, 1) * matrix.at(2, 2) - matrix.at(1, 2) * matrix.at(2, 1)) *
                       inverse_determinant;
    inverse.at(0, 1) = (matrix.at(0, 2) * matrix.at(2, 1) - matrix.at(0, 1) * matrix.at(2, 2)) *
                       inverse_determinant;
    inverse.at(0, 2) = (matrix.at(0, 1) * matrix.at(1, 2) - matrix.at(0, 2) * matrix.at(1, 1)) *
                       inverse_determinant;
    inverse.at(1, 0) = (matrix.at(1, 2) * matrix.at(2, 0) - matrix.at(1, 0) * matrix.at(2, 2)) *
                       inverse_determinant;
    inverse.at(1, 1) = (matrix.at(0, 0) * matrix.at(2, 2) - matrix.at(0, 2) * matrix.at(2, 0)) *
                       inverse_determinant;
    inverse.at(1, 2) = (matrix.at(0, 2) * matrix.at(1, 0) - matrix.at(0, 0) * matrix.at(1, 2)) *
                       inverse_determinant;
    inverse.at(2, 0) = (matrix.at(1, 0) * matrix.at(2, 1) - matrix.at(1, 1) * matrix.at(2, 0)) *
                       inverse_determinant;
    inverse.at(2, 1) = (matrix.at(0, 1) * matrix.at(2, 0) - matrix.at(0, 0) * matrix.at(2, 1)) *
                       inverse_determinant;
    inverse.at(2, 2) = (matrix.at(0, 0) * matrix.at(1, 1) - matrix.at(0, 1) * matrix.at(1, 0)) *
                       inverse_determinant;
    const math::Vec3f translation{matrix.at(0, 3), matrix.at(1, 3), matrix.at(2, 3)};
    inverse.at(0, 3) = -(inverse.at(0, 0) * translation.x + inverse.at(0, 1) * translation.y +
                         inverse.at(0, 2) * translation.z);
    inverse.at(1, 3) = -(inverse.at(1, 0) * translation.x + inverse.at(1, 1) * translation.y +
                         inverse.at(1, 2) * translation.z);
    inverse.at(2, 3) = -(inverse.at(2, 0) * translation.x + inverse.at(2, 1) * translation.y +
                         inverse.at(2, 2) * translation.z);
    return core::Result<math::Mat4f>::success(inverse);
}

} // namespace

NodePose bind_node_pose(const assets::ModelAsset& model) {
    NodePose result;
    result.local_transforms.reserve(model.nodes.size());
    result.morph_weights.reserve(model.nodes.size());
    for (const auto& node : model.nodes) {
        result.local_transforms.push_back(node.bind_transform);
        result.morph_weights.push_back(node.morph_weights);
    }
    return result;
}

core::Status validate_node_pose(const assets::ModelAsset& model, const NodePose& pose) {
    if (pose.local_transforms.size() != model.nodes.size() ||
        pose.morph_weights.size() != model.nodes.size() ||
        !std::ranges::all_of(pose.local_transforms,
                             [](const auto& transform) { return transform.is_valid(); })) {
        return core::Status::failure(
            "skeletal_animation.invalid_pose",
            "skeletal pose must provide one valid local TRS transform per model node");
    }
    for (std::size_t node = 0; node < model.nodes.size(); ++node) {
        if (pose.morph_weights[node].size() != model.nodes[node].morph_weights.size() ||
            !std::ranges::all_of(pose.morph_weights[node],
                                 [](float value) { return std::isfinite(value); })) {
            return core::Status::failure(
                "skeletal_animation.invalid_pose",
                "skeletal pose morph weights must match the model nodes and be finite");
        }
    }
    return core::Status::ok();
}

NodePose bind_pose(const assets::ModelAsset& model) {
    return bind_node_pose(model);
}

core::Status validate_skeletal_pose(const assets::ModelAsset& model, const SkeletalPose& pose) {
    return validate_node_pose(model, pose);
}

core::Result<NodePose> sample_animation_clip(const assets::ModelAsset& model,
                                             const AnimationClipPlayback& playback) {
    if (playback.clip >= model.animations.size() || !std::isfinite(playback.time_seconds)) {
        return core::Result<NodePose>::failure(
            "skeletal_animation.invalid_playback",
            "animation playback requires a valid clip and finite time");
    }
    auto pose = bind_node_pose(model);
    const auto& clip = model.animations[playback.clip];
    const auto time_seconds = sampled_time(clip, playback);
    for (const auto& channel : clip.channels) {
        const auto value_multiplier =
            channel.interpolation == assets::ModelAnimationInterpolation::cubic_spline ? 3U : 1U;
        const auto is_weights = channel.path == assets::ModelAnimationPath::weights;
        const auto expected_weight_values =
            channel.times.size() * value_multiplier * channel.weight_count;
        if (channel.node >= pose.local_transforms.size() || channel.times.empty() ||
            (is_weights ? channel.weight_count != pose.morph_weights[channel.node].size() ||
                              channel.weight_values.size() != expected_weight_values ||
                              !channel.values.empty()
                        : channel.values.size() != channel.times.size() * value_multiplier)) {
            return core::Result<NodePose>::failure(
                "skeletal_animation.invalid_channel",
                "animation channel has invalid runtime bounds or references a missing model node");
        }
        auto& transform = pose.local_transforms[channel.node];
        if (is_weights) {
            for (std::size_t weight = 0; weight < channel.weight_count; ++weight) {
                pose.morph_weights[channel.node][weight] =
                    weight_channel_value(channel, weight, time_seconds);
            }
            continue;
        }
        const auto value = channel_value(channel, time_seconds);
        switch (channel.path) {
        case assets::ModelAnimationPath::translation:
            transform.translation = {value.x, value.y, value.z};
            break;
        case assets::ModelAnimationPath::rotation:
            transform.rotation = normalized({value.x, value.y, value.z, value.w});
            break;
        case assets::ModelAnimationPath::scale:
            transform.scale = {value.x, value.y, value.z};
            break;
        case assets::ModelAnimationPath::weights:
            break;
        }
    }
    auto status = validate_node_pose(model, pose);
    if (!status) {
        return core::Result<NodePose>::failure(status.error().code, status.error().message);
    }
    return core::Result<NodePose>::success(std::move(pose));
}

core::Result<NodePose> blend_node_poses(const assets::ModelAsset& model, const NodePose& first,
                                        const NodePose& second, float second_weight) {
    auto status = validate_node_pose(model, first);
    if (!status) {
        return core::Result<NodePose>::failure(status.error().code, status.error().message);
    }
    status = validate_node_pose(model, second);
    if (!status) {
        return core::Result<NodePose>::failure(status.error().code, status.error().message);
    }
    if (!std::isfinite(second_weight) || second_weight < 0.0F || second_weight > 1.0F) {
        return core::Result<NodePose>::failure(
            "skeletal_animation.invalid_blend",
            "animation blend weight must be finite and in the inclusive [0, 1] range");
    }
    NodePose result;
    result.local_transforms.resize(first.local_transforms.size());
    result.morph_weights.resize(first.morph_weights.size());
    for (std::size_t index = 0; index < result.local_transforms.size(); ++index) {
        const auto& left = first.local_transforms[index];
        const auto& right = second.local_transforms[index];
        result.local_transforms[index] = {
            left.translation + (right.translation - left.translation) * second_weight,
            slerp(left.rotation, right.rotation, second_weight),
            left.scale + (right.scale - left.scale) * second_weight,
        };
    }
    for (std::size_t node = 0; node < result.morph_weights.size(); ++node) {
        result.morph_weights[node].resize(first.morph_weights[node].size());
        for (std::size_t weight = 0; weight < result.morph_weights[node].size(); ++weight) {
            result.morph_weights[node][weight] =
                first.morph_weights[node][weight] +
                (second.morph_weights[node][weight] - first.morph_weights[node][weight]) *
                    second_weight;
        }
    }
    return core::Result<NodePose>::success(std::move(result));
}

core::Status apply_root_motion_policy(const assets::ModelAsset& model, NodePose& pose,
                                      RootMotionPolicy policy) {
    auto status = validate_node_pose(model, pose);
    if (!status) {
        return status;
    }
    switch (policy) {
    case RootMotionPolicy::preserve:
        return core::Status::ok();
    case RootMotionPolicy::ignore_horizontal_translation: {
        std::vector<std::vector<std::uint32_t>> children(model.nodes.size());
        std::vector<std::size_t> subtree_primitive_counts(model.nodes.size(), 0);
        std::vector<std::size_t> direct_primitive_counts(model.nodes.size(), 0);
        std::vector<bool> skeleton_roots(model.nodes.size(), false);
        for (std::size_t node = 0; node < model.nodes.size(); ++node) {
            const auto parent = model.nodes[node].parent;
            if (parent != assets::no_model_index) {
                children[parent].push_back(static_cast<std::uint32_t>(node));
            }
        }
        for (const auto& primitive : model.primitives) {
            ++subtree_primitive_counts[primitive.node];
            ++direct_primitive_counts[primitive.node];
        }
        for (const auto& skin : model.skins) {
            if (skin.skeleton_root != assets::no_model_index) {
                skeleton_roots[skin.skeleton_root] = true;
            }
        }
        const std::function<std::size_t(std::size_t)> count_subtree_primitives =
            [&](std::size_t node) {
                auto count = subtree_primitive_counts[node];
                for (const auto child : children[node]) {
                    count += count_subtree_primitives(child);
                }
                subtree_primitive_counts[node] = count;
                return count;
            };
        for (std::size_t node = 0; node < model.nodes.size(); ++node) {
            if (model.nodes[node].parent == assets::no_model_index) {
                (void)count_subtree_primitives(node);
            }
        }
        for (std::size_t node = 0; node < model.nodes.size(); ++node) {
            const auto is_hierarchy_root = model.nodes[node].parent == assets::no_model_index;
            const auto is_common_geometry_ancestor =
                subtree_primitive_counts[node] == model.primitives.size() &&
                direct_primitive_counts[node] == 0;
            if (!is_hierarchy_root && !is_common_geometry_ancestor && !skeleton_roots[node]) {
                continue;
            }
            pose.local_transforms[node].translation.x =
                model.nodes[node].bind_transform.translation.x;
            pose.local_transforms[node].translation.z =
                model.nodes[node].bind_transform.translation.z;
        }
        return core::Status::ok();
    }
    }
    return core::Status::failure("skeletal_animation.invalid_root_motion_policy",
                                 "node pose root-motion policy is unknown");
}

core::Result<NodePose> blend_skeletal_poses(const assets::ModelAsset& model,
                                            const SkeletalPose& first, const SkeletalPose& second,
                                            float second_weight) {
    return blend_node_poses(model, first, second, second_weight);
}

core::Result<NodePose> sample_blended_animation(const assets::ModelAsset& model,
                                                const AnimationClipPlayback& first,
                                                const AnimationClipPlayback& second,
                                                float second_weight) {
    auto first_pose = sample_animation_clip(model, first);
    if (!first_pose) {
        return first_pose;
    }
    auto second_pose = sample_animation_clip(model, second);
    if (!second_pose) {
        return second_pose;
    }
    return blend_node_poses(model, first_pose.value(), second_pose.value(), second_weight);
}

core::Result<AnimationNodeMask>
make_descendant_animation_mask(const assets::ModelAsset& model,
                               std::span<const std::string_view> root_nodes) {
    if (root_nodes.empty()) {
        return core::Result<AnimationNodeMask>::failure(
            "skeletal_animation.empty_mask",
            "animation mask requires at least one named root node");
    }
    std::vector<bool> roots(model.nodes.size(), false);
    for (const auto name : root_nodes) {
        const auto found = std::ranges::find(model.nodes, name, &assets::ModelNode::name);
        if (found == model.nodes.end()) {
            return core::Result<AnimationNodeMask>::failure(
                "skeletal_animation.missing_mask_node",
                "animation mask references a missing model node: " + std::string(name));
        }
        roots[static_cast<std::size_t>(std::distance(model.nodes.begin(), found))] = true;
    }
    AnimationNodeMask result;
    result.node_weights.resize(model.nodes.size(), 0.0F);
    for (std::size_t node = 0; node < model.nodes.size(); ++node) {
        auto ancestor = static_cast<std::uint32_t>(node);
        while (ancestor != assets::no_model_index) {
            if (ancestor >= model.nodes.size()) {
                return core::Result<AnimationNodeMask>::failure(
                    "skeletal_animation.invalid_mask_hierarchy",
                    "animation mask encountered an invalid model parent");
            }
            if (roots[ancestor]) {
                result.node_weights[node] = 1.0F;
                break;
            }
            ancestor = model.nodes[ancestor].parent;
        }
    }
    return core::Result<AnimationNodeMask>::success(std::move(result));
}

core::Result<NodePose>
compose_animation_layers(const assets::ModelAsset& model, const NodePose& base_pose,
                         std::span<const AnimationLayer> layers) {
    auto status = validate_node_pose(model, base_pose);
    if (!status) {
        return core::Result<NodePose>::failure(status.error().code, status.error().message);
    }
    NodePose result = base_pose;
    for (const auto& layer : layers) {
        if (!std::isfinite(layer.weight) || layer.weight < 0.0F || layer.weight > 1.0F ||
            (layer.mask != nullptr && layer.mask->node_weights.size() != model.nodes.size()) ||
            (layer.mask != nullptr &&
             !std::ranges::all_of(layer.mask->node_weights, [](float value) {
                 return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
             }))) {
            return core::Result<NodePose>::failure(
                "skeletal_animation.invalid_layer",
                "animation layer requires a finite weight and a model-sized normalized mask");
        }
        if (layer.weight == 0.0F) {
            continue;
        }
        auto sampled = sample_animation_clip(model, layer.playback);
        if (!sampled) {
            return sampled;
        }
        NodePose reference;
        if (layer.mode == AnimationLayerMode::additive) {
            if (layer.additive_reference.has_value()) {
                auto sampled_reference =
                    sample_animation_clip(model, *layer.additive_reference);
                if (!sampled_reference) {
                    return sampled_reference;
                }
                reference = std::move(sampled_reference).value();
            } else {
                reference = bind_node_pose(model);
            }
        }
        for (std::size_t node = 0; node < model.nodes.size(); ++node) {
            const auto mask_weight =
                layer.mask == nullptr ? 1.0F : layer.mask->node_weights[node];
            const auto weight = layer.weight * mask_weight;
            if (weight == 0.0F) {
                continue;
            }
            auto& target = result.local_transforms[node];
            const auto& source = sampled.value().local_transforms[node];
            if (layer.mode == AnimationLayerMode::override_pose) {
                target.translation += (source.translation - target.translation) * weight;
                target.rotation = slerp(target.rotation, source.rotation, weight);
                target.scale += (source.scale - target.scale) * weight;
                for (std::size_t morph = 0; morph < result.morph_weights[node].size(); ++morph) {
                    auto& target_weight = result.morph_weights[node][morph];
                    target_weight +=
                        (sampled.value().morph_weights[node][morph] - target_weight) * weight;
                }
                continue;
            }

            const auto& base = reference.local_transforms[node];
            target.translation += (source.translation - base.translation) * weight;
            const auto rotation_delta =
                multiply(inverse_rotation(base.rotation), source.rotation);
            target.rotation = multiply(
                target.rotation,
                slerp(assets::ModelQuaternion{}, rotation_delta, weight));
            target.scale += (source.scale - base.scale) * weight;
            for (std::size_t morph = 0; morph < result.morph_weights[node].size(); ++morph) {
                result.morph_weights[node][morph] +=
                    (sampled.value().morph_weights[node][morph] -
                     reference.morph_weights[node][morph]) *
                    weight;
            }
        }
    }
    status = validate_node_pose(model, result);
    if (!status) {
        return core::Result<NodePose>::failure(status.error().code, status.error().message);
    }
    return core::Result<NodePose>::success(std::move(result));
}

core::Result<std::vector<std::string>>
crossed_animation_events(std::span<const AnimationEventMarker> markers,
                         float previous_normalized_phase, float current_normalized_phase,
                         bool looping) {
    if (!std::isfinite(previous_normalized_phase) || !std::isfinite(current_normalized_phase) ||
        previous_normalized_phase < 0.0F || previous_normalized_phase >= 1.0F ||
        current_normalized_phase < 0.0F || current_normalized_phase >= 1.0F ||
        std::ranges::any_of(markers, [](const AnimationEventMarker& marker) {
            return marker.name.empty() || marker.name.size() > 256U ||
                   !std::isfinite(marker.normalized_phase) || marker.normalized_phase < 0.0F ||
                   marker.normalized_phase >= 1.0F;
        })) {
        return core::Result<std::vector<std::string>>::failure(
            "skeletal_animation.invalid_events",
            "animation events require bounded names and normalized phases in [0, 1)");
    }
    if (previous_normalized_phase == current_normalized_phase) {
        return core::Result<std::vector<std::string>>::success({});
    }
    std::vector<const AnimationEventMarker*> crossed;
    crossed.reserve(markers.size());
    const auto wrapped = looping && current_normalized_phase < previous_normalized_phase;
    for (const auto& marker : markers) {
        const auto in_first_segment = marker.normalized_phase > previous_normalized_phase;
        const auto in_second_segment = marker.normalized_phase <= current_normalized_phase;
        if ((!wrapped && marker.normalized_phase > previous_normalized_phase &&
             marker.normalized_phase <= current_normalized_phase) ||
            (wrapped && (in_first_segment || in_second_segment))) {
            crossed.push_back(&marker);
        }
    }
    std::ranges::sort(crossed, [wrapped, previous_normalized_phase](const auto* left,
                                                                   const auto* right) {
        const auto order = [wrapped, previous_normalized_phase](float phase) {
            return wrapped && phase <= previous_normalized_phase ? phase + 1.0F : phase;
        };
        return order(left->normalized_phase) < order(right->normalized_phase);
    });
    std::vector<std::string> result;
    result.reserve(crossed.size());
    for (const auto* marker : crossed) {
        result.push_back(marker->name);
    }
    return core::Result<std::vector<std::string>>::success(std::move(result));
}

core::Result<std::vector<math::Mat4f>> evaluate_model_node_matrices(const assets::ModelAsset& model,
                                                                    const NodePose& pose) {
    auto status = validate_node_pose(model, pose);
    if (!status) {
        return core::Result<std::vector<math::Mat4f>>::failure(status.error().code,
                                                               status.error().message);
    }
    std::vector<math::Mat4f> global_matrices(model.nodes.size());
    std::vector<bool> resolved(model.nodes.size(), false);
    std::vector<bool> visiting(model.nodes.size(), false);
    const std::function<core::Status(std::size_t)> resolve =
        [&](std::size_t node_index) -> core::Status {
        if (resolved[node_index]) {
            return core::Status::ok();
        }
        if (visiting[node_index]) {
            return core::Status::failure(
                "skeletal_animation.hierarchy_cycle",
                "model node hierarchy contains a cycle during pose evaluation");
        }
        visiting[node_index] = true;
        const auto local = node_matrix(pose.local_transforms[node_index]);
        const auto parent = model.nodes[node_index].parent;
        if (parent != assets::no_model_index) {
            if (parent >= model.nodes.size()) {
                return core::Status::failure("skeletal_animation.invalid_parent",
                                             "model node hierarchy references a missing parent");
            }
            auto parent_status = resolve(parent);
            if (!parent_status) {
                return parent_status;
            }
            global_matrices[node_index] = global_matrices[parent] * local;
        } else {
            global_matrices[node_index] = local;
        }
        visiting[node_index] = false;
        resolved[node_index] = true;
        return core::Status::ok();
    };
    for (std::size_t index = 0; index < model.nodes.size(); ++index) {
        status = resolve(index);
        if (!status) {
            return core::Result<std::vector<math::Mat4f>>::failure(status.error().code,
                                                                   status.error().message);
        }
    }
    return core::Result<std::vector<math::Mat4f>>::success(std::move(global_matrices));
}

core::Result<SkinningPalette> build_skinning_palette(const assets::ModelAsset& model,
                                                     std::uint32_t skin, std::uint32_t mesh_node,
                                                     const NodePose& pose) {
    if (skin >= model.skins.size() || mesh_node >= model.nodes.size()) {
        return core::Result<SkinningPalette>::failure(
            "skeletal_animation.invalid_skin_binding",
            "skinning palette requires valid skin and mesh-node indices");
    }
    auto globals = evaluate_model_node_matrices(model, pose);
    if (!globals) {
        return core::Result<SkinningPalette>::failure(globals.error().code,
                                                      globals.error().message);
    }
    auto inverse_mesh = inverse_affine(globals.value()[mesh_node]);
    if (!inverse_mesh) {
        return core::Result<SkinningPalette>::failure(inverse_mesh.error().code,
                                                      inverse_mesh.error().message);
    }
    SkinningPalette result;
    result.skin = skin;
    result.mesh_node = mesh_node;
    const auto& source_skin = model.skins[skin];
    result.joint_matrices.reserve(source_skin.joints.size());
    for (std::size_t joint = 0; joint < source_skin.joints.size(); ++joint) {
        const auto node = source_skin.joints[joint];
        if (node >= globals.value().size() || joint >= source_skin.inverse_bind_matrices.size()) {
            return core::Result<SkinningPalette>::failure(
                "skeletal_animation.invalid_skin",
                "skin joint or inverse-bind matrix is outside the model");
        }
        result.joint_matrices.push_back(inverse_mesh.value() * globals.value()[node] *
                                        source_skin.inverse_bind_matrices[joint]);
    }
    return core::Result<SkinningPalette>::success(std::move(result));
}

core::Result<SkinningPalette> build_model_space_skinning_palette(const assets::ModelAsset& model,
                                                                 std::uint32_t skin,
                                                                 std::uint32_t mesh_node,
                                                                 const NodePose& pose) {
    if (skin >= model.skins.size() || mesh_node >= model.nodes.size()) {
        return core::Result<SkinningPalette>::failure(
            "skeletal_animation.invalid_skin_binding",
            "skinning palette requires valid skin and mesh-node indices");
    }
    auto globals = evaluate_model_node_matrices(model, pose);
    if (!globals) {
        return core::Result<SkinningPalette>::failure(globals.error().code,
                                                      globals.error().message);
    }
    return build_model_space_skinning_palette(model, skin, mesh_node, globals.value());
}

core::Result<SkinningPalette>
build_model_space_skinning_palette(const assets::ModelAsset& model, std::uint32_t skin,
                                   std::uint32_t mesh_node,
                                   std::span<const math::Mat4f> model_node_matrices) {
    if (skin >= model.skins.size() || mesh_node >= model.nodes.size() ||
        model_node_matrices.size() != model.nodes.size() ||
        !std::ranges::all_of(model_node_matrices,
                             [](const math::Mat4f& matrix) { return matrix.is_finite(); })) {
        return core::Result<SkinningPalette>::failure(
            "skeletal_animation.invalid_skin_binding",
            "skinning palette requires valid skin, mesh-node, and evaluated node matrices");
    }
    SkinningPalette result;
    result.skin = skin;
    result.mesh_node = mesh_node;
    const auto& source_skin = model.skins[skin];
    result.joint_matrices.reserve(source_skin.joints.size());
    for (std::size_t joint = 0; joint < source_skin.joints.size(); ++joint) {
        const auto node = source_skin.joints[joint];
        if (node >= model_node_matrices.size() ||
            joint >= source_skin.inverse_bind_matrices.size()) {
            return core::Result<SkinningPalette>::failure(
                "skeletal_animation.invalid_skin",
                "skin joint or inverse-bind matrix is outside the model");
        }
        result.joint_matrices.push_back(model_node_matrices[node] *
                                        source_skin.inverse_bind_matrices[joint]);
    }
    return core::Result<SkinningPalette>::success(std::move(result));
}

core::Result<CpuSkinnedVertex> skin_model_vertex(const assets::ModelVertex& vertex,
                                                 std::span<const math::Mat4f> joint_matrices) {
    if (joint_matrices.empty()) {
        return core::Result<CpuSkinnedVertex>::failure(
            "skeletal_animation.empty_palette", "CPU skinning requires at least one joint matrix");
    }
    math::Vec3f position{};
    math::Vec3f normal{};
    float weight_sum = 0.0F;
    for (std::size_t influence = 0; influence < vertex.weights.size(); ++influence) {
        const auto weight = vertex.weights[influence];
        if (!std::isfinite(weight) || weight < 0.0F || weight > 1.0F) {
            return core::Result<CpuSkinnedVertex>::failure(
                "skeletal_animation.invalid_vertex_influence",
                "skinned vertex influence is invalid for the supplied joint palette");
        }
        if (weight == 0.0F) {
            continue;
        }
        if (vertex.joints[influence] >= joint_matrices.size()) {
            return core::Result<CpuSkinnedVertex>::failure(
                "skeletal_animation.invalid_vertex_influence",
                "skinned vertex influence is invalid for the supplied joint palette");
        }
        weight_sum += weight;
        const auto& matrix = joint_matrices[vertex.joints[influence]];
        const auto transformed_position =
            matrix * math::Vec4f{vertex.position.x, vertex.position.y, vertex.position.z, 1.0F};
        const auto transformed_normal =
            matrix * math::Vec4f{vertex.normal.x, vertex.normal.y, vertex.normal.z, 0.0F};
        position +=
            math::Vec3f{transformed_position.x, transformed_position.y, transformed_position.z} *
            weight;
        normal +=
            math::Vec3f{transformed_normal.x, transformed_normal.y, transformed_normal.z} * weight;
    }
    const auto normal_length = static_cast<float>(math::length(normal));
    if (!position.is_finite() || std::abs(weight_sum - 1.0F) > 0.01F ||
        !std::isfinite(normal_length) || normal_length <= std::numeric_limits<float>::epsilon()) {
        return core::Result<CpuSkinnedVertex>::failure(
            "skeletal_animation.invalid_skinned_vertex",
            "CPU skinning produced a non-finite position or degenerate normal");
    }
    return core::Result<CpuSkinnedVertex>::success({position, normal / normal_length});
}

} // namespace heartstead::animation
