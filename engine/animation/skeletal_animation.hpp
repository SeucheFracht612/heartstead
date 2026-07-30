#pragma once

#include "engine/assets/model_asset.hpp"
#include "engine/core/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace heartstead::animation {

struct NodePose {
    std::vector<assets::ModelNodeTransform> local_transforms;
    std::vector<std::vector<float>> morph_weights;

    friend bool operator==(const NodePose&, const NodePose&) = default;
};

// Compatibility name for callers that still describe the shared node pose by its
// skinning consumer. Rigid-node and skinned animation use the same pose type.
using SkeletalPose = NodePose;

struct AnimationClipPlayback {
    std::uint32_t clip = assets::no_model_index;
    float time_seconds = 0.0F;
    bool looping = true;
};

enum class RootMotionPolicy : std::uint8_t {
    preserve,
    ignore_horizontal_translation,
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

[[nodiscard]] NodePose bind_node_pose(const assets::ModelAsset& model);
[[nodiscard]] core::Status validate_node_pose(const assets::ModelAsset& model,
                                              const NodePose& pose);
[[nodiscard]] core::Result<NodePose> blend_node_poses(const assets::ModelAsset& model,
                                                      const NodePose& first, const NodePose& second,
                                                      float second_weight);
[[nodiscard]] core::Status apply_root_motion_policy(const assets::ModelAsset& model, NodePose& pose,
                                                    RootMotionPolicy policy);

[[nodiscard]] NodePose bind_pose(const assets::ModelAsset& model);
[[nodiscard]] core::Status validate_skeletal_pose(const assets::ModelAsset& model,
                                                  const SkeletalPose& pose);
[[nodiscard]] core::Result<NodePose> sample_animation_clip(const assets::ModelAsset& model,
                                                           const AnimationClipPlayback& playback);
[[nodiscard]] core::Result<NodePose> blend_skeletal_poses(const assets::ModelAsset& model,
                                                          const SkeletalPose& first,
                                                          const SkeletalPose& second,
                                                          float second_weight);
[[nodiscard]] core::Result<NodePose> sample_blended_animation(const assets::ModelAsset& model,
                                                              const AnimationClipPlayback& first,
                                                              const AnimationClipPlayback& second,
                                                              float second_weight);
[[nodiscard]] core::Result<std::vector<math::Mat4f>>
evaluate_model_node_matrices(const assets::ModelAsset& model, const NodePose& pose);
[[nodiscard]] core::Result<SkinningPalette> build_skinning_palette(const assets::ModelAsset& model,
                                                                   std::uint32_t skin,
                                                                   std::uint32_t mesh_node,
                                                                   const NodePose& pose);
[[nodiscard]] core::Result<SkinningPalette>
build_model_space_skinning_palette(const assets::ModelAsset& model, std::uint32_t skin,
                                   std::uint32_t mesh_node, const NodePose& pose);
[[nodiscard]] core::Result<SkinningPalette>
build_model_space_skinning_palette(const assets::ModelAsset& model, std::uint32_t skin,
                                   std::uint32_t mesh_node,
                                   std::span<const math::Mat4f> model_node_matrices);
[[nodiscard]] core::Result<CpuSkinnedVertex>
skin_model_vertex(const assets::ModelVertex& vertex, std::span<const math::Mat4f> joint_matrices);

} // namespace heartstead::animation
