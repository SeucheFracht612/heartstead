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

SkeletalPose bind_pose(const assets::ModelAsset& model) {
    SkeletalPose result;
    result.local_transforms.reserve(model.nodes.size());
    for (const auto& node : model.nodes) {
        result.local_transforms.push_back(node.bind_transform);
    }
    return result;
}

core::Status validate_skeletal_pose(const assets::ModelAsset& model, const SkeletalPose& pose) {
    if (pose.local_transforms.size() != model.nodes.size() ||
        !std::ranges::all_of(pose.local_transforms,
                             [](const auto& transform) { return transform.is_valid(); })) {
        return core::Status::failure(
            "skeletal_animation.invalid_pose",
            "skeletal pose must provide one valid local TRS transform per model node");
    }
    return core::Status::ok();
}

core::Result<SkeletalPose> sample_animation_clip(const assets::ModelAsset& model,
                                                 const AnimationClipPlayback& playback) {
    if (playback.clip >= model.animations.size() || !std::isfinite(playback.time_seconds)) {
        return core::Result<SkeletalPose>::failure(
            "skeletal_animation.invalid_playback",
            "animation playback requires a valid clip and finite time");
    }
    auto pose = bind_pose(model);
    const auto& clip = model.animations[playback.clip];
    const auto time_seconds = sampled_time(clip, playback);
    for (const auto& channel : clip.channels) {
        const auto value_multiplier =
            channel.interpolation == assets::ModelAnimationInterpolation::cubic_spline ? 3U : 1U;
        if (channel.node >= pose.local_transforms.size() || channel.times.empty() ||
            channel.values.size() != channel.times.size() * value_multiplier) {
            return core::Result<SkeletalPose>::failure(
                "skeletal_animation.invalid_channel",
                "animation channel has invalid runtime bounds or references a missing model node");
        }
        const auto value = channel_value(channel, time_seconds);
        auto& transform = pose.local_transforms[channel.node];
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
        }
    }
    auto status = validate_skeletal_pose(model, pose);
    if (!status) {
        return core::Result<SkeletalPose>::failure(status.error().code, status.error().message);
    }
    return core::Result<SkeletalPose>::success(std::move(pose));
}

core::Result<SkeletalPose> blend_skeletal_poses(const assets::ModelAsset& model,
                                                const SkeletalPose& first,
                                                const SkeletalPose& second, float second_weight) {
    auto status = validate_skeletal_pose(model, first);
    if (!status) {
        return core::Result<SkeletalPose>::failure(status.error().code, status.error().message);
    }
    status = validate_skeletal_pose(model, second);
    if (!status) {
        return core::Result<SkeletalPose>::failure(status.error().code, status.error().message);
    }
    if (!std::isfinite(second_weight) || second_weight < 0.0F || second_weight > 1.0F) {
        return core::Result<SkeletalPose>::failure(
            "skeletal_animation.invalid_blend",
            "animation blend weight must be finite and in the inclusive [0, 1] range");
    }
    SkeletalPose result;
    result.local_transforms.resize(first.local_transforms.size());
    for (std::size_t index = 0; index < result.local_transforms.size(); ++index) {
        const auto& left = first.local_transforms[index];
        const auto& right = second.local_transforms[index];
        result.local_transforms[index] = {
            left.translation + (right.translation - left.translation) * second_weight,
            slerp(left.rotation, right.rotation, second_weight),
            left.scale + (right.scale - left.scale) * second_weight,
        };
    }
    return core::Result<SkeletalPose>::success(std::move(result));
}

core::Result<SkeletalPose> sample_blended_animation(const assets::ModelAsset& model,
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
    return blend_skeletal_poses(model, first_pose.value(), second_pose.value(), second_weight);
}

core::Result<std::vector<math::Mat4f>> evaluate_model_node_matrices(const assets::ModelAsset& model,
                                                                    const SkeletalPose& pose) {
    auto status = validate_skeletal_pose(model, pose);
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
                                                     const SkeletalPose& pose) {
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
