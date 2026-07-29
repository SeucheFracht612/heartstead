#pragma once

#include "engine/core/result.hpp"
#include "engine/math/matrix.hpp"
#include "engine/math/vector.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace heartstead::assets {

inline constexpr std::uint32_t no_model_index = UINT32_MAX;

struct ModelQuaternion {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 1.0F;

    [[nodiscard]] bool is_finite() const noexcept;
    [[nodiscard]] float length_squared() const noexcept;
    friend bool operator==(const ModelQuaternion&, const ModelQuaternion&) = default;
};

struct ModelNodeTransform {
    math::Vec3f translation{};
    ModelQuaternion rotation{};
    math::Vec3f scale{1.0F, 1.0F, 1.0F};

    [[nodiscard]] bool is_valid() const noexcept;
    friend bool operator==(const ModelNodeTransform&, const ModelNodeTransform&) = default;
};

struct ModelVertex {
    math::Vec3f position{};
    math::Vec3f normal{0.0F, 1.0F, 0.0F};
    math::Vec2f uv{};
    std::array<std::uint16_t, 4> joints{};
    std::array<float, 4> weights{1.0F, 0.0F, 0.0F, 0.0F};

    friend bool operator==(const ModelVertex&, const ModelVertex&) = default;
};

struct ModelNode {
    std::string name;
    std::uint32_t parent = no_model_index;
    ModelNodeTransform bind_transform;

    friend bool operator==(const ModelNode&, const ModelNode&) = default;
};

struct ModelPrimitive {
    std::string name;
    std::uint32_t first_vertex = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t node = no_model_index;
    std::uint32_t skin = no_model_index;
    std::uint32_t material = no_model_index;

    friend bool operator==(const ModelPrimitive&, const ModelPrimitive&) = default;
};

struct ModelImage {
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8;

    friend bool operator==(const ModelImage&, const ModelImage&) = default;
};

enum class ModelAlphaMode : std::uint8_t {
    opaque,
    mask,
};

struct ModelMaterial {
    std::string name;
    std::array<float, 4> base_color_factor{1.0F, 1.0F, 1.0F, 1.0F};
    std::uint32_t base_color_image = no_model_index;
    ModelAlphaMode alpha_mode = ModelAlphaMode::opaque;
    float alpha_cutoff = 0.5F;
    bool double_sided = false;

    friend bool operator==(const ModelMaterial&, const ModelMaterial&) = default;
};

struct ModelSkin {
    std::string name;
    std::uint32_t skeleton_root = no_model_index;
    std::vector<std::uint32_t> joints;
    std::vector<math::Mat4f> inverse_bind_matrices;

    friend bool operator==(const ModelSkin&, const ModelSkin&) = default;
};

enum class ModelAnimationPath : std::uint8_t {
    translation,
    rotation,
    scale,
};

enum class ModelAnimationInterpolation : std::uint8_t {
    step,
    linear,
    cubic_spline,
};

struct ModelAnimationChannel {
    std::uint32_t node = no_model_index;
    ModelAnimationPath path = ModelAnimationPath::translation;
    ModelAnimationInterpolation interpolation = ModelAnimationInterpolation::linear;
    std::vector<float> times;
    // Translation/scale use xyz. Rotation uses xyzw. Cubic-spline channels store
    // in-tangent, value, out-tangent for each time in that order.
    std::vector<math::Vec4f> values;

    friend bool operator==(const ModelAnimationChannel&, const ModelAnimationChannel&) = default;
};

struct ModelAnimationClip {
    std::string name;
    float duration_seconds = 0.0F;
    std::vector<ModelAnimationChannel> channels;

    friend bool operator==(const ModelAnimationClip&, const ModelAnimationClip&) = default;
};

struct ModelAsset {
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<ModelNode> nodes;
    std::vector<ModelPrimitive> primitives;
    std::vector<ModelImage> images;
    std::vector<ModelMaterial> materials;
    std::vector<ModelSkin> skins;
    std::vector<ModelAnimationClip> animations;
    math::Bounds3f bounds{};

    friend bool operator==(const ModelAsset& left, const ModelAsset& right) {
        return left.vertices == right.vertices && left.indices == right.indices &&
               left.nodes == right.nodes && left.primitives == right.primitives &&
               left.images == right.images && left.materials == right.materials &&
               left.skins == right.skins && left.animations == right.animations &&
               left.bounds.min == right.bounds.min && left.bounds.max == right.bounds.max;
    }
};

struct ModelAssetLimits {
    std::size_t maximum_source_bytes = 256U * 1024U * 1024U;
    std::uint32_t maximum_vertices = 1'000'000;
    std::uint32_t maximum_indices = 3'000'000;
    std::uint32_t maximum_nodes = 16'384;
    std::uint32_t maximum_primitives = 16'384;
    std::uint32_t maximum_images = 1'024;
    std::uint32_t maximum_materials = 4'096;
    std::uint32_t maximum_image_dimension = 8'192;
    std::size_t maximum_decoded_image_bytes = 128U * 1024U * 1024U;
    std::uint32_t maximum_skins = 256;
    std::uint32_t maximum_joints_per_skin = 256;
    std::uint32_t maximum_animations = 256;
    std::uint32_t maximum_channels_per_animation = 4'096;
    std::uint32_t maximum_keyframes_per_channel = 1'000'000;
    std::size_t maximum_name_bytes = 1'024;

    [[nodiscard]] core::Status validate() const;
};

[[nodiscard]] core::Status validate_model_asset(const ModelAsset& asset,
                                                const ModelAssetLimits& limits = {});
[[nodiscard]] core::Result<std::vector<std::filesystem::path>>
discover_gltf_external_dependencies(const std::filesystem::path& path,
                                    const ModelAssetLimits& limits = {});
[[nodiscard]] core::Result<ModelAsset> import_gltf_model(const std::filesystem::path& path,
                                                         const ModelAssetLimits& limits = {});
[[nodiscard]] core::Result<std::vector<std::uint8_t>>
encode_model_asset(const ModelAsset& asset, const ModelAssetLimits& limits = {});
[[nodiscard]] core::Result<ModelAsset> decode_model_asset(std::span<const std::uint8_t> bytes,
                                                          const ModelAssetLimits& limits = {});
[[nodiscard]] core::Result<std::uint32_t> resolve_model_animation_clip(const ModelAsset& model,
                                                                       std::string_view clip_name);

[[nodiscard]] std::string_view model_animation_path_name(ModelAnimationPath path) noexcept;
[[nodiscard]] std::string_view
model_animation_interpolation_name(ModelAnimationInterpolation interpolation) noexcept;
[[nodiscard]] std::string_view model_alpha_mode_name(ModelAlphaMode mode) noexcept;

} // namespace heartstead::assets
