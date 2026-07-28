#pragma once

#include "engine/assets/model_asset.hpp"
#include "engine/core/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace heartstead::animation {

struct SkeletalPose {
    std::vector<assets::ModelNodeTransform> local_transforms;

    friend bool operator==(const SkeletalPose&, const SkeletalPose&) = default;
};

struct AnimationClipPlayback {
    std::uint32_t clip = assets::no_model_index;
    float time_seconds = 0.0F;
    bool looping = true;
};

struct SkinningPalette {
    std::uint32_t skin = assets::no_model_index;
    std::uint32_t mesh_node = assets::no_model_index;
    std::vector<math::Mat4f> joint_matrices;
};

struct CpuSkinnedVertex {
    math::Vec3f position{};
    math::Vec3f normal{};
};

[[nodiscard]] SkeletalPose bind_pose(const assets::ModelAsset& model);
[[nodiscard]] core::Status validate_skeletal_pose(const assets::ModelAsset& model,
                                                  const SkeletalPose& pose);
[[nodiscard]] core::Result<SkeletalPose>
sample_animation_clip(const assets::ModelAsset& model, const AnimationClipPlayback& playback);
[[nodiscard]] core::Result<SkeletalPose> blend_skeletal_poses(const assets::ModelAsset& model,
                                                              const SkeletalPose& first,
                                                              const SkeletalPose& second,
                                                              float second_weight);
[[nodiscard]] core::Result<SkeletalPose>
sample_blended_animation(const assets::ModelAsset& model, const AnimationClipPlayback& first,
                         const AnimationClipPlayback& second, float second_weight);
[[nodiscard]] core::Result<std::vector<math::Mat4f>>
evaluate_model_node_matrices(const assets::ModelAsset& model, const SkeletalPose& pose);
[[nodiscard]] core::Result<SkinningPalette> build_skinning_palette(const assets::ModelAsset& model,
                                                                   std::uint32_t skin,
                                                                   std::uint32_t mesh_node,
                                                                   const SkeletalPose& pose);
[[nodiscard]] core::Result<CpuSkinnedVertex>
skin_model_vertex(const assets::ModelVertex& vertex, std::span<const math::Mat4f> joint_matrices);

} // namespace heartstead::animation
