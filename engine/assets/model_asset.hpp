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
#include <utility>
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
    math::Vec4f tangent{1.0F, 0.0F, 0.0F, 1.0F};
    math::Vec2f uv0{};
    math::Vec2f uv1{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<std::uint16_t, 4> joints{};
    std::array<float, 4> weights{1.0F, 0.0F, 0.0F, 0.0F};

    ModelVertex() = default;
    ModelVertex(math::Vec3f source_position, math::Vec3f source_normal, math::Vec2f source_uv,
                std::array<std::uint16_t, 4> source_joints, std::array<float, 4> source_weights)
        : position(source_position), normal(source_normal), uv0(source_uv), joints(source_joints),
          weights(source_weights) {}

    friend bool operator==(const ModelVertex&, const ModelVertex&) = default;
};

struct ModelMorphTarget {
    std::vector<math::Vec3f> position_deltas;
    std::vector<math::Vec3f> normal_deltas;
    std::vector<math::Vec3f> tangent_deltas;

    friend bool operator==(const ModelMorphTarget&, const ModelMorphTarget&) = default;
};

struct ModelNode {
    std::string name;
    std::uint32_t parent = no_model_index;
    ModelNodeTransform bind_transform;
    std::vector<float> morph_weights;

    ModelNode() = default;
    ModelNode(std::string source_name, std::uint32_t source_parent,
              ModelNodeTransform source_transform, std::vector<float> source_morph_weights = {})
        : name(std::move(source_name)), parent(source_parent), bind_transform(source_transform),
          morph_weights(std::move(source_morph_weights)) {}

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
    std::vector<ModelMorphTarget> morph_targets;
    math::Bounds3f bounds{};
    std::uint32_t lod_level = 0;
    bool renderable = true;
    bool collision_source = false;

    ModelPrimitive() = default;
    ModelPrimitive(std::string source_name, std::uint32_t source_first_vertex,
                   std::uint32_t source_vertex_count, std::uint32_t source_first_index,
                   std::uint32_t source_index_count, std::uint32_t source_node,
                   std::uint32_t source_skin = no_model_index,
                   std::uint32_t source_material = no_model_index,
                   std::vector<ModelMorphTarget> source_morph_targets = {},
                   math::Bounds3f source_bounds = {}, std::uint32_t source_lod_level = 0,
                   bool source_renderable = true, bool source_collision_source = false)
        : name(std::move(source_name)), first_vertex(source_first_vertex),
          vertex_count(source_vertex_count), first_index(source_first_index),
          index_count(source_index_count), node(source_node), skin(source_skin),
          material(source_material), morph_targets(std::move(source_morph_targets)),
          bounds(source_bounds), lod_level(source_lod_level), renderable(source_renderable),
          collision_source(source_collision_source) {}

    friend bool operator==(const ModelPrimitive&, const ModelPrimitive&) = default;
};

struct ModelSocket {
    std::string name;
    std::uint32_t node = no_model_index;

    friend bool operator==(const ModelSocket&, const ModelSocket&) = default;
};

struct ModelLod {
    std::uint32_t level = 0;
    float screen_coverage = 1.0F;
    float geometric_error = 0.0F;
    std::vector<std::uint32_t> primitives;

    friend bool operator==(const ModelLod&, const ModelLod&) = default;
};

struct ModelCollisionShape {
    std::string name;
    std::uint32_t node = no_model_index;
    math::Bounds3f bounds{};

    friend bool operator==(const ModelCollisionShape&, const ModelCollisionShape&) = default;
};

enum class ModelCameraKind : std::uint8_t {
    perspective,
    orthographic,
};

struct ModelCamera {
    std::string name;
    std::uint32_t node = no_model_index;
    ModelCameraKind kind = ModelCameraKind::perspective;
    float aspect_ratio = 0.0F;
    float vertical_fov_radians = 0.785398F;
    float x_magnification = 1.0F;
    float y_magnification = 1.0F;
    float near_plane = 0.1F;
    float far_plane = 0.0F;

    [[nodiscard]] bool has_infinite_far_plane() const noexcept {
        return far_plane == 0.0F;
    }

    friend bool operator==(const ModelCamera&, const ModelCamera&) = default;
};

enum class ModelLightKind : std::uint8_t {
    directional,
    point,
    spot,
};

struct ModelLight {
    std::string name;
    std::uint32_t node = no_model_index;
    ModelLightKind kind = ModelLightKind::point;
    math::Vec3f color{1.0F, 1.0F, 1.0F};
    float intensity = 1.0F;
    float range = 0.0F;
    float inner_cone_radians = 0.0F;
    float outer_cone_radians = 0.785398F;

    friend bool operator==(const ModelLight&, const ModelLight&) = default;
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
    blend,
};

enum class ModelTextureWrap : std::uint8_t {
    repeat,
    clamp_to_edge,
    mirrored_repeat,
};

enum class ModelTextureMagFilter : std::uint8_t {
    nearest,
    linear,
};

enum class ModelTextureMinFilter : std::uint8_t {
    nearest,
    linear,
    nearest_mipmap_nearest,
    linear_mipmap_nearest,
    nearest_mipmap_linear,
    linear_mipmap_linear,
};

struct ModelSampler {
    ModelTextureMagFilter mag_filter = ModelTextureMagFilter::linear;
    ModelTextureMinFilter min_filter = ModelTextureMinFilter::linear;
    ModelTextureWrap wrap_s = ModelTextureWrap::repeat;
    ModelTextureWrap wrap_t = ModelTextureWrap::repeat;

    friend bool operator==(const ModelSampler&, const ModelSampler&) = default;
};

struct ModelTextureBinding {
    std::uint32_t image = no_model_index;
    std::uint32_t sampler = no_model_index;
    std::uint8_t texcoord = 0;
    math::Vec2f offset{};
    math::Vec2f scale{1.0F, 1.0F};
    float rotation = 0.0F;

    friend bool operator==(const ModelTextureBinding&, const ModelTextureBinding&) = default;
};

struct ModelMaterial {
    std::string name;
    std::array<float, 4> base_color_factor{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 3> emissive_factor{};
    float metallic_factor = 1.0F;
    float roughness_factor = 1.0F;
    float normal_scale = 1.0F;
    float occlusion_strength = 1.0F;
    ModelTextureBinding base_color_texture;
    ModelTextureBinding metallic_roughness_texture;
    ModelTextureBinding normal_texture;
    ModelTextureBinding occlusion_texture;
    ModelTextureBinding emissive_texture;
    ModelAlphaMode alpha_mode = ModelAlphaMode::opaque;
    float alpha_cutoff = 0.5F;
    bool double_sided = false;
    bool unlit = false;

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
    weights,
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
    // Weight channels have one scalar per morph target per keyframe (or three
    // scalars per target for cubic spline) and leave values empty.
    std::uint32_t weight_count = 0;
    std::vector<float> weight_values;

    ModelAnimationChannel() = default;
    ModelAnimationChannel(std::uint32_t source_node, ModelAnimationPath source_path,
                          ModelAnimationInterpolation source_interpolation,
                          std::vector<float> source_times, std::vector<math::Vec4f> source_values,
                          std::uint32_t source_weight_count = 0,
                          std::vector<float> source_weight_values = {})
        : node(source_node), path(source_path), interpolation(source_interpolation),
          times(std::move(source_times)), values(std::move(source_values)),
          weight_count(source_weight_count), weight_values(std::move(source_weight_values)) {}

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
    std::vector<ModelSampler> samplers;
    std::vector<ModelMaterial> materials;
    std::vector<ModelSkin> skins;
    std::vector<ModelAnimationClip> animations;
    std::vector<ModelSocket> sockets;
    std::vector<ModelLod> lods;
    std::vector<ModelCollisionShape> collision_shapes;
    std::vector<ModelCamera> cameras;
    std::vector<ModelLight> lights;
    math::Bounds3f bounds{};

    friend bool operator==(const ModelAsset& left, const ModelAsset& right) {
        return left.vertices == right.vertices && left.indices == right.indices &&
               left.nodes == right.nodes && left.primitives == right.primitives &&
               left.images == right.images && left.samplers == right.samplers &&
               left.materials == right.materials && left.skins == right.skins &&
               left.animations == right.animations && left.sockets == right.sockets &&
               left.lods == right.lods && left.collision_shapes == right.collision_shapes &&
               left.cameras == right.cameras && left.lights == right.lights &&
               left.bounds.min == right.bounds.min && left.bounds.max == right.bounds.max;
    }
};

struct ModelCapabilities {
    bool has_animation_clips = false;
    bool has_skins = false;
    bool has_morph_targets = false;
    bool has_animated_nodes = false;
    bool has_lods = false;
    bool has_sockets = false;
    bool has_collision_metadata = false;
    bool has_cameras = false;
    bool has_lights = false;

    [[nodiscard]] bool supports_animation() const noexcept {
        return has_animation_clips;
    }

    friend bool operator==(const ModelCapabilities&, const ModelCapabilities&) = default;
};

struct ModelAssetLimits {
    std::size_t maximum_source_bytes = 256U * 1024U * 1024U;
    std::uint32_t maximum_vertices = 1'000'000;
    std::uint32_t maximum_indices = 3'000'000;
    std::uint32_t maximum_nodes = 16'384;
    std::uint32_t maximum_primitives = 16'384;
    std::uint32_t maximum_images = 1'024;
    std::uint32_t maximum_samplers = 4'096;
    std::uint32_t maximum_materials = 4'096;
    std::uint32_t maximum_image_dimension = 8'192;
    std::size_t maximum_decoded_image_bytes = 128U * 1024U * 1024U;
    std::uint32_t maximum_skins = 256;
    std::uint32_t maximum_joints_per_skin = 256;
    std::uint32_t maximum_animations = 256;
    std::uint32_t maximum_channels_per_animation = 4'096;
    std::uint32_t maximum_keyframes_per_channel = 1'000'000;
    std::uint32_t maximum_morph_targets_per_primitive = 64;
    std::size_t maximum_morph_delta_values = 16U * 1024U * 1024U;
    std::uint32_t maximum_sockets = 4'096;
    std::uint32_t maximum_lods = 16;
    std::uint32_t maximum_collision_shapes = 4'096;
    std::uint32_t maximum_cameras = 256;
    std::uint32_t maximum_lights = 4'096;
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
[[nodiscard]] ModelCapabilities model_capabilities(const ModelAsset& model) noexcept;
[[nodiscard]] core::Result<std::uint32_t> resolve_model_animation_clip(const ModelAsset& model,
                                                                       std::string_view clip_name);

[[nodiscard]] std::string_view model_animation_path_name(ModelAnimationPath path) noexcept;
[[nodiscard]] std::string_view
model_animation_interpolation_name(ModelAnimationInterpolation interpolation) noexcept;
[[nodiscard]] std::string_view model_alpha_mode_name(ModelAlphaMode mode) noexcept;

} // namespace heartstead::assets
