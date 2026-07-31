#include "engine/assets/model_asset.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <ranges>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace heartstead::assets {

namespace {

constexpr std::string_view model_magic_v2 = "heartstead.model.v2";
constexpr std::string_view model_magic_v3 = "heartstead.model.v3";
constexpr std::string_view model_magic_v4 = "heartstead.model.v4";
constexpr std::string_view model_magic_v5 = "heartstead.model.v5";
constexpr std::uint8_t model_material_double_sided = 1U << 0U;
constexpr std::uint8_t model_material_unlit = 1U << 1U;
constexpr std::uint8_t known_model_material_flags =
    model_material_double_sided | model_material_unlit;
constexpr float quaternion_tolerance = 0.01F;
constexpr float weight_tolerance = 0.01F;

[[nodiscard]] bool valid_name(std::string_view name, const ModelAssetLimits& limits) noexcept {
    return !name.empty() && name.size() <= limits.maximum_name_bytes &&
           name.find('\0') == std::string_view::npos;
}

[[nodiscard]] bool finite_vec4(math::Vec4f value) noexcept {
    return value.is_finite();
}

[[nodiscard]] bool valid_vertex(const ModelVertex& vertex) noexcept {
    if (!vertex.position.is_finite() || !vertex.normal.is_finite() || !vertex.tangent.is_finite() ||
        !vertex.uv0.is_finite() || !vertex.uv1.is_finite() ||
        !std::ranges::all_of(vertex.color, [](float value) {
            return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
        })) {
        return false;
    }
    float weight_sum = 0.0F;
    for (const auto weight : vertex.weights) {
        if (!std::isfinite(weight) || weight < 0.0F || weight > 1.0F) {
            return false;
        }
        weight_sum += weight;
    }
    return std::abs(weight_sum - 1.0F) <= weight_tolerance;
}

[[nodiscard]] bool valid_model_sampler(const ModelSampler& sampler) noexcept {
    switch (sampler.mag_filter) {
    case ModelTextureMagFilter::nearest:
    case ModelTextureMagFilter::linear:
        break;
    default:
        return false;
    }
    switch (sampler.min_filter) {
    case ModelTextureMinFilter::nearest:
    case ModelTextureMinFilter::linear:
    case ModelTextureMinFilter::nearest_mipmap_nearest:
    case ModelTextureMinFilter::linear_mipmap_nearest:
    case ModelTextureMinFilter::nearest_mipmap_linear:
    case ModelTextureMinFilter::linear_mipmap_linear:
        break;
    default:
        return false;
    }
    const auto valid_wrap = [](ModelTextureWrap wrap) {
        switch (wrap) {
        case ModelTextureWrap::repeat:
        case ModelTextureWrap::clamp_to_edge:
        case ModelTextureWrap::mirrored_repeat:
            return true;
        }
        return false;
    };
    return valid_wrap(sampler.wrap_s) && valid_wrap(sampler.wrap_t);
}

[[nodiscard]] bool valid_texture_binding(const ModelTextureBinding& binding,
                                         const ModelAsset& asset) noexcept {
    return (binding.image == no_model_index || binding.image < asset.images.size()) &&
           (binding.sampler == no_model_index || binding.sampler < asset.samplers.size()) &&
           binding.texcoord <= 1U && binding.offset.is_finite() && binding.scale.is_finite() &&
           std::isfinite(binding.rotation);
}

[[nodiscard]] bool valid_model_image(const ModelImage& image, const ModelAssetLimits& limits,
                                     std::size_t& total_bytes) noexcept {
    if (!valid_name(image.name, limits) || image.width == 0 || image.height == 0 ||
        image.width > limits.maximum_image_dimension ||
        image.height > limits.maximum_image_dimension) {
        return false;
    }
    const auto expected = static_cast<std::uint64_t>(image.width) * image.height * 4U;
    if (expected != image.rgba8.size() ||
        expected > limits.maximum_decoded_image_bytes - total_bytes) {
        return false;
    }
    total_bytes += static_cast<std::size_t>(expected);
    return true;
}

[[nodiscard]] bool valid_model_material(const ModelMaterial& material, const ModelAsset& asset,
                                        const ModelAssetLimits& limits) noexcept {
    if (!valid_name(material.name, limits) ||
        !valid_texture_binding(material.base_color_texture, asset) ||
        !valid_texture_binding(material.metallic_roughness_texture, asset) ||
        !valid_texture_binding(material.normal_texture, asset) ||
        !valid_texture_binding(material.occlusion_texture, asset) ||
        !valid_texture_binding(material.emissive_texture, asset) ||
        !std::isfinite(material.alpha_cutoff) || material.alpha_cutoff < 0.0F) {
        return false;
    }
    switch (material.alpha_mode) {
    case ModelAlphaMode::opaque:
    case ModelAlphaMode::mask:
    case ModelAlphaMode::blend:
        break;
    default:
        return false;
    }
    const auto unit_value = [](float value) {
        return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
    };
    return std::ranges::all_of(material.base_color_factor, unit_value) &&
           std::ranges::all_of(material.emissive_factor, unit_value) &&
           unit_value(material.metallic_factor) && unit_value(material.roughness_factor) &&
           std::isfinite(material.normal_scale) && unit_value(material.occlusion_strength);
}

[[nodiscard]] bool node_hierarchy_is_acyclic(const ModelAsset& asset) {
    enum class VisitState : std::uint8_t {
        unvisited,
        visiting,
        complete,
    };
    std::vector<VisitState> states(asset.nodes.size(), VisitState::unvisited);
    for (std::size_t start = 0; start < asset.nodes.size(); ++start) {
        auto current = start;
        while (current != no_model_index) {
            if (current >= asset.nodes.size()) {
                return false;
            }
            if (states[current] == VisitState::complete) {
                break;
            }
            if (states[current] == VisitState::visiting) {
                return false;
            }
            states[current] = VisitState::visiting;
            current = asset.nodes[current].parent;
        }
        current = start;
        while (current != no_model_index && states[current] == VisitState::visiting) {
            states[current] = VisitState::complete;
            current = asset.nodes[current].parent;
        }
    }
    return true;
}

[[nodiscard]] bool valid_animation_channel(const ModelAnimationChannel& channel,
                                           const ModelAsset& asset,
                                           const ModelAssetLimits& limits) {
    switch (channel.path) {
    case ModelAnimationPath::translation:
    case ModelAnimationPath::rotation:
    case ModelAnimationPath::scale:
        break;
    case ModelAnimationPath::weights:
        if (channel.weight_count == 0 || channel.node >= asset.nodes.size() ||
            asset.nodes[channel.node].morph_weights.size() != channel.weight_count ||
            !channel.values.empty()) {
            return false;
        }
        break;
    default:
        return false;
    }
    switch (channel.interpolation) {
    case ModelAnimationInterpolation::step:
    case ModelAnimationInterpolation::linear:
    case ModelAnimationInterpolation::cubic_spline:
        break;
    default:
        return false;
    }
    if (channel.node >= asset.nodes.size() || channel.times.empty() ||
        channel.times.size() > limits.maximum_keyframes_per_channel) {
        return false;
    }
    const auto multiplier =
        channel.interpolation == ModelAnimationInterpolation::cubic_spline ? 3U : 1U;
    if (channel.path == ModelAnimationPath::weights) {
        const auto expected =
            channel.times.size() * multiplier * static_cast<std::size_t>(channel.weight_count);
        if (channel.weight_values.size() != expected ||
            !std::ranges::all_of(channel.weight_values,
                                 [](float value) { return std::isfinite(value); })) {
            return false;
        }
    } else if (channel.weight_count != 0 || !channel.weight_values.empty() ||
               channel.values.size() != channel.times.size() * multiplier) {
        return false;
    }
    float previous = -1.0F;
    for (const auto time : channel.times) {
        if (!std::isfinite(time) || time < 0.0F || time <= previous) {
            return false;
        }
        previous = time;
    }
    if (!std::ranges::all_of(channel.values, finite_vec4)) {
        return false;
    }
    if (channel.path == ModelAnimationPath::rotation) {
        for (std::size_t index = 0; index < channel.times.size(); ++index) {
            const auto value_index =
                channel.interpolation == ModelAnimationInterpolation::cubic_spline ? index * 3U + 1U
                                                                                   : index;
            const auto value = channel.values[value_index];
            const auto length_squared =
                value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
            if (std::abs(length_squared - 1.0F) > quaternion_tolerance) {
                return false;
            }
        }
    }
    return true;
}

class ByteWriter {
  public:
    void bytes(std::span<const std::uint8_t> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void string(std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(value.data()),
                                            value.size()});
    }

    void u8(std::uint8_t value) {
        bytes_.push_back(value);
    }

    void u16(std::uint16_t value) {
        bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        bytes_.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    }

    void u32(std::uint32_t value) {
        for (std::uint32_t shift = 0; shift < 32; shift += 8) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }

    void f32(float value) {
        u32(std::bit_cast<std::uint32_t>(value));
    }

    [[nodiscard]] std::vector<std::uint8_t> take() {
        return std::move(bytes_);
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

class ByteReader {
  public:
    explicit ByteReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] core::Result<std::span<const std::uint8_t>> bytes(std::size_t count) {
        if (count > bytes_.size() - offset_) {
            return core::Result<std::span<const std::uint8_t>>::failure(
                "model_asset.truncated", "model asset payload is truncated");
        }
        const auto result = bytes_.subspan(offset_, count);
        offset_ += count;
        return core::Result<std::span<const std::uint8_t>>::success(result);
    }

    [[nodiscard]] core::Result<std::string> string(std::size_t maximum_bytes) {
        auto count = u32();
        if (!count) {
            return core::Result<std::string>::failure(count.error().code, count.error().message);
        }
        if (count.value() == 0 || count.value() > maximum_bytes) {
            return core::Result<std::string>::failure("model_asset.invalid_string",
                                                      "model asset string length is invalid");
        }
        auto value = bytes(count.value());
        if (!value) {
            return core::Result<std::string>::failure(value.error().code, value.error().message);
        }
        return core::Result<std::string>::success(
            std::string(reinterpret_cast<const char*>(value.value().data()), value.value().size()));
    }

    [[nodiscard]] core::Result<std::uint8_t> u8() {
        auto value = bytes(1);
        if (!value) {
            return core::Result<std::uint8_t>::failure(value.error().code, value.error().message);
        }
        return core::Result<std::uint8_t>::success(value.value()[0]);
    }

    [[nodiscard]] core::Result<std::uint16_t> u16() {
        auto value = bytes(2);
        if (!value) {
            return core::Result<std::uint16_t>::failure(value.error().code, value.error().message);
        }
        return core::Result<std::uint16_t>::success(
            static_cast<std::uint16_t>(value.value()[0]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(value.value()[1]) << 8U));
    }

    [[nodiscard]] core::Result<std::uint32_t> u32() {
        auto value = bytes(4);
        if (!value) {
            return core::Result<std::uint32_t>::failure(value.error().code, value.error().message);
        }
        return core::Result<std::uint32_t>::success(
            static_cast<std::uint32_t>(value.value()[0]) |
            (static_cast<std::uint32_t>(value.value()[1]) << 8U) |
            (static_cast<std::uint32_t>(value.value()[2]) << 16U) |
            (static_cast<std::uint32_t>(value.value()[3]) << 24U));
    }

    [[nodiscard]] core::Result<float> f32() {
        auto value = u32();
        if (!value) {
            return core::Result<float>::failure(value.error().code, value.error().message);
        }
        return core::Result<float>::success(std::bit_cast<float>(value.value()));
    }

    [[nodiscard]] bool at_end() const noexcept {
        return offset_ == bytes_.size();
    }

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

void write_vec2(ByteWriter& writer, math::Vec2f value) {
    writer.f32(value.x);
    writer.f32(value.y);
}

void write_vec3(ByteWriter& writer, math::Vec3f value) {
    writer.f32(value.x);
    writer.f32(value.y);
    writer.f32(value.z);
}

void write_vec4(ByteWriter& writer, math::Vec4f value) {
    writer.f32(value.x);
    writer.f32(value.y);
    writer.f32(value.z);
    writer.f32(value.w);
}

[[nodiscard]] core::Result<math::Vec2f> read_vec2(ByteReader& reader) {
    auto x = reader.f32();
    auto y = reader.f32();
    if (!x || !y) {
        const auto& error = !x ? x.error() : y.error();
        return core::Result<math::Vec2f>::failure(error.code, error.message);
    }
    return core::Result<math::Vec2f>::success({x.value(), y.value()});
}

[[nodiscard]] core::Result<math::Vec3f> read_vec3(ByteReader& reader) {
    auto x = reader.f32();
    auto y = reader.f32();
    auto z = reader.f32();
    if (!x || !y || !z) {
        const auto& error = !x ? x.error() : !y ? y.error() : z.error();
        return core::Result<math::Vec3f>::failure(error.code, error.message);
    }
    return core::Result<math::Vec3f>::success({x.value(), y.value(), z.value()});
}

[[nodiscard]] core::Result<math::Vec4f> read_vec4(ByteReader& reader) {
    auto x = reader.f32();
    auto y = reader.f32();
    auto z = reader.f32();
    auto w = reader.f32();
    if (!x || !y || !z || !w) {
        const auto& error = !x ? x.error() : !y ? y.error() : !z ? z.error() : w.error();
        return core::Result<math::Vec4f>::failure(error.code, error.message);
    }
    return core::Result<math::Vec4f>::success({x.value(), y.value(), z.value(), w.value()});
}

template <typename T> [[nodiscard]] core::Result<T> decode_failure(const core::Error& error) {
    return core::Result<T>::failure(error.code, error.message);
}

} // namespace

bool ModelQuaternion::is_finite() const noexcept {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w);
}

float ModelQuaternion::length_squared() const noexcept {
    return x * x + y * y + z * z + w * w;
}

bool ModelNodeTransform::is_valid() const noexcept {
    return translation.is_finite() && rotation.is_finite() &&
           std::abs(rotation.length_squared() - 1.0F) <= quaternion_tolerance &&
           scale.is_finite() && !scale.any_zero();
}

core::Status ModelAssetLimits::validate() const {
    if (maximum_source_bytes == 0 || maximum_vertices == 0 || maximum_indices == 0 ||
        maximum_nodes == 0 || maximum_primitives == 0 || maximum_skins == 0 ||
        maximum_images == 0 || maximum_samplers == 0 || maximum_materials == 0 ||
        maximum_image_dimension == 0 || maximum_decoded_image_bytes < 4 ||
        maximum_joints_per_skin == 0 || maximum_joints_per_skin > UINT16_MAX ||
        maximum_animations == 0 || maximum_channels_per_animation == 0 ||
        maximum_keyframes_per_channel == 0 || maximum_morph_targets_per_primitive == 0 ||
        maximum_morph_delta_values == 0 || maximum_sockets == 0 || maximum_lods == 0 ||
        maximum_collision_shapes == 0 || maximum_cameras == 0 || maximum_lights == 0 ||
        maximum_name_bytes == 0 || maximum_name_bytes > UINT32_MAX) {
        return core::Status::failure("model_asset.invalid_limits",
                                     "model asset limits must be finite and non-zero");
    }
    return core::Status::ok();
}

core::Status validate_model_asset(const ModelAsset& asset, const ModelAssetLimits& limits) {
    auto status = limits.validate();
    if (!status) {
        return status;
    }
    if (asset.vertices.empty() || asset.indices.empty() || asset.primitives.empty() ||
        asset.vertices.size() > limits.maximum_vertices ||
        asset.indices.size() > limits.maximum_indices ||
        asset.nodes.size() > limits.maximum_nodes ||
        asset.primitives.size() > limits.maximum_primitives ||
        asset.images.size() > limits.maximum_images ||
        asset.samplers.size() > limits.maximum_samplers ||
        asset.materials.size() > limits.maximum_materials ||
        asset.skins.size() > limits.maximum_skins ||
        asset.animations.size() > limits.maximum_animations ||
        asset.sockets.size() > limits.maximum_sockets || asset.lods.size() > limits.maximum_lods ||
        asset.collision_shapes.size() > limits.maximum_collision_shapes ||
        asset.cameras.size() > limits.maximum_cameras ||
        asset.lights.size() > limits.maximum_lights || !asset.bounds.is_valid()) {
        return core::Status::failure("model_asset.invalid_counts",
                                     "model asset geometry or bounded counts are invalid");
    }
    if (!std::ranges::all_of(asset.vertices, valid_vertex)) {
        return core::Status::failure("model_asset.invalid_vertex",
                                     "model asset vertex data is invalid");
    }
    if (!std::ranges::all_of(asset.indices,
                             [&](std::uint32_t index) { return index < asset.vertices.size(); })) {
        return core::Status::failure("model_asset.index_out_of_bounds",
                                     "model asset index references a missing vertex");
    }
    if (!node_hierarchy_is_acyclic(asset)) {
        return core::Status::failure("model_asset.invalid_hierarchy",
                                     "model asset node hierarchy is invalid or cyclic");
    }
    std::size_t total_image_bytes = 0;
    for (const auto& image : asset.images) {
        if (!valid_model_image(image, limits, total_image_bytes)) {
            return core::Status::failure(
                "model_asset.invalid_image",
                "model asset image name, dimensions, or RGBA8 payload is invalid");
        }
    }
    if (!std::ranges::all_of(asset.samplers, valid_model_sampler)) {
        return core::Status::failure("model_asset.invalid_sampler",
                                     "model asset sampler state is invalid");
    }
    if (!std::ranges::all_of(asset.materials, [&](const ModelMaterial& material) {
            return valid_model_material(material, asset, limits);
        })) {
        return core::Status::failure(
            "model_asset.invalid_material",
            "model asset material parameters or base-color image binding are invalid");
    }
    for (const auto& node : asset.nodes) {
        if (!valid_name(node.name, limits) || !node.bind_transform.is_valid() ||
            !std::ranges::all_of(node.morph_weights,
                                 [](float value) { return std::isfinite(value); })) {
            return core::Status::failure("model_asset.invalid_node",
                                         "model asset node name or bind transform is invalid");
        }
    }
    std::size_t morph_delta_values = 0;
    for (const auto& primitive : asset.primitives) {
        if (!valid_name(primitive.name, limits) || primitive.node >= asset.nodes.size() ||
            (primitive.skin != no_model_index && primitive.skin >= asset.skins.size()) ||
            (primitive.material != no_model_index &&
             primitive.material >= asset.materials.size()) ||
            primitive.vertex_count == 0 || primitive.index_count == 0 ||
            primitive.first_vertex > asset.vertices.size() ||
            primitive.vertex_count > asset.vertices.size() - primitive.first_vertex ||
            primitive.first_index > asset.indices.size() ||
            primitive.index_count > asset.indices.size() - primitive.first_index ||
            primitive.index_count % 3U != 0 || !primitive.bounds.is_valid() ||
            primitive.lod_level >= limits.maximum_lods) {
            return core::Status::failure("model_asset.invalid_primitive",
                                         "model asset primitive range or binding is invalid");
        }
        if (primitive.morph_targets.size() > limits.maximum_morph_targets_per_primitive ||
            asset.nodes[primitive.node].morph_weights.size() != primitive.morph_targets.size()) {
            return core::Status::failure(
                "model_asset.invalid_morph_binding",
                "model primitive morph targets must match its node's default weights");
        }
        for (const auto& target : primitive.morph_targets) {
            const auto valid_deltas = [&](const std::vector<math::Vec3f>& deltas) {
                return (deltas.empty() || deltas.size() == primitive.vertex_count) &&
                       std::ranges::all_of(deltas,
                                           [](math::Vec3f value) { return value.is_finite(); });
            };
            if (!valid_deltas(target.position_deltas) || !valid_deltas(target.normal_deltas) ||
                !valid_deltas(target.tangent_deltas)) {
                return core::Status::failure(
                    "model_asset.invalid_morph_target",
                    "model morph target deltas must be finite and match the primitive");
            }
            const auto additional = target.position_deltas.size() + target.normal_deltas.size() +
                                    target.tangent_deltas.size();
            if (additional > limits.maximum_morph_delta_values - morph_delta_values) {
                return core::Status::failure(
                    "model_asset.morph_limit",
                    "model morph target data exceeds its configured limit");
            }
            morph_delta_values += additional;
        }
        if (primitive.skin != no_model_index) {
            const auto& skin = asset.skins[primitive.skin];
            for (std::size_t vertex_index = primitive.first_vertex;
                 vertex_index < primitive.first_vertex + primitive.vertex_count; ++vertex_index) {
                const auto& vertex = asset.vertices[vertex_index];
                for (std::size_t influence = 0; influence < vertex.joints.size(); ++influence) {
                    if (vertex.weights[influence] > 0.0F &&
                        vertex.joints[influence] >= skin.joints.size()) {
                        return core::Status::failure(
                            "model_asset.joint_out_of_bounds",
                            "model asset vertex references a missing skin joint");
                    }
                }
            }
        }
    }
    for (const auto& skin : asset.skins) {
        if (!valid_name(skin.name, limits) || skin.joints.empty() ||
            skin.joints.size() > limits.maximum_joints_per_skin ||
            skin.joints.size() != skin.inverse_bind_matrices.size() ||
            (skin.skeleton_root != no_model_index && skin.skeleton_root >= asset.nodes.size()) ||
            !std::ranges::all_of(skin.joints,
                                 [&](std::uint32_t joint) { return joint < asset.nodes.size(); }) ||
            !std::ranges::all_of(skin.inverse_bind_matrices,
                                 [](const math::Mat4f& matrix) { return matrix.is_finite(); })) {
            return core::Status::failure("model_asset.invalid_skin",
                                         "model asset skin hierarchy or bind matrices are invalid");
        }
    }
    for (const auto& clip : asset.animations) {
        if (!valid_name(clip.name, limits) || !std::isfinite(clip.duration_seconds) ||
            clip.duration_seconds < 0.0F || clip.channels.empty() ||
            clip.channels.size() > limits.maximum_channels_per_animation) {
            return core::Status::failure("model_asset.invalid_animation",
                                         "model asset animation metadata is invalid");
        }
        float maximum_time = 0.0F;
        for (const auto& channel : clip.channels) {
            if (!valid_animation_channel(channel, asset, limits)) {
                return core::Status::failure("model_asset.invalid_channel",
                                             "model asset animation channel is invalid");
            }
            maximum_time = std::max(maximum_time, channel.times.back());
        }
        if (std::abs(maximum_time - clip.duration_seconds) > 0.0001F) {
            return core::Status::failure("model_asset.invalid_duration",
                                         "model asset clip duration does not match its channels");
        }
    }
    std::unordered_set<std::string> socket_names;
    for (const auto& socket : asset.sockets) {
        if (!valid_name(socket.name, limits) || socket.node >= asset.nodes.size() ||
            !socket_names.insert(socket.name).second) {
            return core::Status::failure(
                "model_asset.invalid_socket",
                "model sockets must have unique bounded names and reference valid nodes");
        }
    }
    std::uint32_t expected_lod = 0;
    for (const auto& lod : asset.lods) {
        if (lod.level != expected_lod++ || !std::isfinite(lod.screen_coverage) ||
            lod.screen_coverage <= 0.0F || lod.screen_coverage > 1.0F ||
            !std::isfinite(lod.geometric_error) || lod.geometric_error < 0.0F ||
            lod.primitives.empty() ||
            !std::ranges::all_of(lod.primitives, [&](std::uint32_t primitive) {
                return primitive < asset.primitives.size() &&
                       asset.primitives[primitive].lod_level == lod.level &&
                       asset.primitives[primitive].renderable;
            })) {
            return core::Status::failure(
                "model_asset.invalid_lod",
                "model LODs must be contiguous, bounded, and reference matching primitives");
        }
    }
    for (const auto& collision : asset.collision_shapes) {
        if (!valid_name(collision.name, limits) || collision.node >= asset.nodes.size() ||
            !collision.bounds.is_valid()) {
            return core::Status::failure(
                "model_asset.invalid_collision",
                "model collision metadata must name a valid node and finite bounds");
        }
    }
    for (const auto& camera : asset.cameras) {
        const auto finite_projection =
            std::isfinite(camera.aspect_ratio) && camera.aspect_ratio >= 0.0F &&
            std::isfinite(camera.vertical_fov_radians) && camera.vertical_fov_radians > 0.0F &&
            std::isfinite(camera.x_magnification) && camera.x_magnification > 0.0F &&
            std::isfinite(camera.y_magnification) && camera.y_magnification > 0.0F &&
            std::isfinite(camera.near_plane) && camera.near_plane > 0.0F &&
            std::isfinite(camera.far_plane) && camera.far_plane >= 0.0F &&
            (camera.far_plane == 0.0F || camera.far_plane > camera.near_plane);
        if (!valid_name(camera.name, limits) || camera.node >= asset.nodes.size() ||
            !finite_projection ||
            (camera.kind != ModelCameraKind::perspective &&
             camera.kind != ModelCameraKind::orthographic)) {
            return core::Status::failure("model_asset.invalid_camera",
                                         "model camera metadata is invalid");
        }
    }
    for (const auto& light : asset.lights) {
        const auto valid_kind = light.kind == ModelLightKind::directional ||
                                light.kind == ModelLightKind::point ||
                                light.kind == ModelLightKind::spot;
        if (!valid_name(light.name, limits) || light.node >= asset.nodes.size() || !valid_kind ||
            !light.color.is_finite() || light.color.x < 0.0F || light.color.y < 0.0F ||
            light.color.z < 0.0F || !std::isfinite(light.intensity) || light.intensity < 0.0F ||
            !std::isfinite(light.range) || light.range < 0.0F ||
            !std::isfinite(light.inner_cone_radians) || light.inner_cone_radians < 0.0F ||
            !std::isfinite(light.outer_cone_radians) ||
            light.outer_cone_radians < light.inner_cone_radians) {
            return core::Status::failure("model_asset.invalid_light",
                                         "model punctual-light metadata is invalid");
        }
    }
    return core::Status::ok();
}

core::Result<std::vector<std::uint8_t>> encode_model_asset(const ModelAsset& asset,
                                                           const ModelAssetLimits& limits) {
    auto status = validate_model_asset(asset, limits);
    if (!status) {
        return core::Result<std::vector<std::uint8_t>>::failure(status.error().code,
                                                                status.error().message);
    }
    ByteWriter writer;
    writer.string(model_magic_v5);
    writer.u32(static_cast<std::uint32_t>(asset.vertices.size()));
    writer.u32(static_cast<std::uint32_t>(asset.indices.size()));
    writer.u32(static_cast<std::uint32_t>(asset.nodes.size()));
    writer.u32(static_cast<std::uint32_t>(asset.primitives.size()));
    writer.u32(static_cast<std::uint32_t>(asset.images.size()));
    writer.u32(static_cast<std::uint32_t>(asset.materials.size()));
    writer.u32(static_cast<std::uint32_t>(asset.skins.size()));
    writer.u32(static_cast<std::uint32_t>(asset.animations.size()));
    write_vec3(writer, asset.bounds.min);
    write_vec3(writer, asset.bounds.max);

    for (const auto& vertex : asset.vertices) {
        write_vec3(writer, vertex.position);
        write_vec3(writer, vertex.normal);
        write_vec2(writer, vertex.uv0);
        for (const auto joint : vertex.joints) {
            writer.u16(joint);
        }
        for (const auto weight : vertex.weights) {
            writer.f32(weight);
        }
    }
    for (const auto index : asset.indices) {
        writer.u32(index);
    }
    for (const auto& node : asset.nodes) {
        writer.string(node.name);
        writer.u32(node.parent);
        write_vec3(writer, node.bind_transform.translation);
        write_vec4(writer, {node.bind_transform.rotation.x, node.bind_transform.rotation.y,
                            node.bind_transform.rotation.z, node.bind_transform.rotation.w});
        write_vec3(writer, node.bind_transform.scale);
    }
    for (const auto& primitive : asset.primitives) {
        writer.string(primitive.name);
        writer.u32(primitive.first_vertex);
        writer.u32(primitive.vertex_count);
        writer.u32(primitive.first_index);
        writer.u32(primitive.index_count);
        writer.u32(primitive.node);
        writer.u32(primitive.skin);
        writer.u32(primitive.material);
    }
    for (const auto& image : asset.images) {
        writer.string(image.name);
        writer.u32(image.width);
        writer.u32(image.height);
        writer.u32(static_cast<std::uint32_t>(image.rgba8.size()));
        writer.bytes(image.rgba8);
    }
    for (const auto& material : asset.materials) {
        writer.string(material.name);
        for (const auto value : material.base_color_factor) {
            writer.f32(value);
        }
        writer.u32(material.base_color_texture.image);
        writer.u8(static_cast<std::uint8_t>(material.alpha_mode));
        writer.f32(material.alpha_cutoff);
        std::uint8_t flags = 0;
        if (material.double_sided) {
            flags |= model_material_double_sided;
        }
        if (material.unlit) {
            flags |= model_material_unlit;
        }
        writer.u8(flags);
    }
    for (const auto& skin : asset.skins) {
        writer.string(skin.name);
        writer.u32(skin.skeleton_root);
        writer.u32(static_cast<std::uint32_t>(skin.joints.size()));
        for (const auto joint : skin.joints) {
            writer.u32(joint);
        }
        for (const auto& matrix : skin.inverse_bind_matrices) {
            for (const auto value : matrix.elements) {
                writer.f32(value);
            }
        }
    }
    for (const auto& clip : asset.animations) {
        writer.string(clip.name);
        writer.f32(clip.duration_seconds);
        writer.u32(static_cast<std::uint32_t>(clip.channels.size()));
        for (const auto& channel : clip.channels) {
            writer.u32(channel.node);
            writer.u8(static_cast<std::uint8_t>(channel.path));
            writer.u8(static_cast<std::uint8_t>(channel.interpolation));
            writer.u32(static_cast<std::uint32_t>(channel.times.size()));
            writer.u32(static_cast<std::uint32_t>(channel.values.size()));
            for (const auto time : channel.times) {
                writer.f32(time);
            }
            for (const auto value : channel.values) {
                write_vec4(writer, value);
            }
        }
    }
    // V4 appends the new vertex, material, sampler, and morph state after the
    // V2/V3-compatible core. This keeps legacy decoding simple and makes the
    // version boundary explicit without duplicating the mature core codec.
    for (const auto& vertex : asset.vertices) {
        write_vec4(writer, vertex.tangent);
        write_vec2(writer, vertex.uv1);
        for (const auto component : vertex.color) {
            writer.f32(component);
        }
    }
    writer.u32(static_cast<std::uint32_t>(asset.samplers.size()));
    for (const auto& sampler : asset.samplers) {
        writer.u8(static_cast<std::uint8_t>(sampler.mag_filter));
        writer.u8(static_cast<std::uint8_t>(sampler.min_filter));
        writer.u8(static_cast<std::uint8_t>(sampler.wrap_s));
        writer.u8(static_cast<std::uint8_t>(sampler.wrap_t));
    }
    const auto write_texture_binding = [&](const ModelTextureBinding& binding) {
        writer.u32(binding.image);
        writer.u32(binding.sampler);
        writer.u8(binding.texcoord);
        write_vec2(writer, binding.offset);
        write_vec2(writer, binding.scale);
        writer.f32(binding.rotation);
    };
    for (const auto& material : asset.materials) {
        for (const auto component : material.emissive_factor) {
            writer.f32(component);
        }
        writer.f32(material.metallic_factor);
        writer.f32(material.roughness_factor);
        writer.f32(material.normal_scale);
        writer.f32(material.occlusion_strength);
        write_texture_binding(material.base_color_texture);
        write_texture_binding(material.metallic_roughness_texture);
        write_texture_binding(material.normal_texture);
        write_texture_binding(material.occlusion_texture);
        write_texture_binding(material.emissive_texture);
    }
    for (const auto& node : asset.nodes) {
        writer.u32(static_cast<std::uint32_t>(node.morph_weights.size()));
        for (const auto weight : node.morph_weights) {
            writer.f32(weight);
        }
    }
    for (const auto& primitive : asset.primitives) {
        writer.u32(static_cast<std::uint32_t>(primitive.morph_targets.size()));
        for (const auto& target : primitive.morph_targets) {
            std::uint8_t attributes = 0;
            if (!target.position_deltas.empty()) {
                attributes |= 1U << 0U;
            }
            if (!target.normal_deltas.empty()) {
                attributes |= 1U << 1U;
            }
            if (!target.tangent_deltas.empty()) {
                attributes |= 1U << 2U;
            }
            writer.u8(attributes);
            for (const auto value : target.position_deltas) {
                write_vec3(writer, value);
            }
            for (const auto value : target.normal_deltas) {
                write_vec3(writer, value);
            }
            for (const auto value : target.tangent_deltas) {
                write_vec3(writer, value);
            }
        }
    }
    for (const auto& clip : asset.animations) {
        for (const auto& channel : clip.channels) {
            writer.u32(channel.weight_count);
            writer.u32(static_cast<std::uint32_t>(channel.weight_values.size()));
            for (const auto value : channel.weight_values) {
                writer.f32(value);
            }
        }
    }

    // V5 appends production presentation metadata. V2-V4 remain readable for cooked-store
    // migration, while new output has an explicit ABI boundary.
    for (const auto& primitive : asset.primitives) {
        write_vec3(writer, primitive.bounds.min);
        write_vec3(writer, primitive.bounds.max);
        writer.u32(primitive.lod_level);
        std::uint8_t flags = primitive.renderable ? 1U : 0U;
        flags |= primitive.collision_source ? 2U : 0U;
        writer.u8(flags);
    }
    writer.u32(static_cast<std::uint32_t>(asset.sockets.size()));
    for (const auto& socket : asset.sockets) {
        writer.string(socket.name);
        writer.u32(socket.node);
    }
    writer.u32(static_cast<std::uint32_t>(asset.lods.size()));
    for (const auto& lod : asset.lods) {
        writer.u32(lod.level);
        writer.f32(lod.screen_coverage);
        writer.f32(lod.geometric_error);
        writer.u32(static_cast<std::uint32_t>(lod.primitives.size()));
        for (const auto primitive : lod.primitives) {
            writer.u32(primitive);
        }
    }
    writer.u32(static_cast<std::uint32_t>(asset.collision_shapes.size()));
    for (const auto& collision : asset.collision_shapes) {
        writer.string(collision.name);
        writer.u32(collision.node);
        write_vec3(writer, collision.bounds.min);
        write_vec3(writer, collision.bounds.max);
    }
    writer.u32(static_cast<std::uint32_t>(asset.cameras.size()));
    for (const auto& camera : asset.cameras) {
        writer.string(camera.name);
        writer.u32(camera.node);
        writer.u8(static_cast<std::uint8_t>(camera.kind));
        writer.f32(camera.aspect_ratio);
        writer.f32(camera.vertical_fov_radians);
        writer.f32(camera.x_magnification);
        writer.f32(camera.y_magnification);
        writer.f32(camera.near_plane);
        writer.f32(camera.far_plane);
    }
    writer.u32(static_cast<std::uint32_t>(asset.lights.size()));
    for (const auto& light : asset.lights) {
        writer.string(light.name);
        writer.u32(light.node);
        writer.u8(static_cast<std::uint8_t>(light.kind));
        write_vec3(writer, light.color);
        writer.f32(light.intensity);
        writer.f32(light.range);
        writer.f32(light.inner_cone_radians);
        writer.f32(light.outer_cone_radians);
    }
    auto encoded = writer.take();
    if (encoded.size() > limits.maximum_source_bytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            "model_asset.encoded_size_limit",
            "encoded model asset exceeds its configured payload limit");
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(encoded));
}

core::Result<ModelAsset> decode_model_asset(std::span<const std::uint8_t> bytes,
                                            const ModelAssetLimits& limits) {
    auto limit_status = limits.validate();
    if (!limit_status) {
        return core::Result<ModelAsset>::failure(limit_status.error().code,
                                                 limit_status.error().message);
    }
    if (bytes.empty() || bytes.size() > limits.maximum_source_bytes) {
        return core::Result<ModelAsset>::failure("model_asset.invalid_payload_size",
                                                 "model asset payload size is invalid");
    }
    ByteReader reader(bytes);
    auto magic = reader.string(model_magic_v5.size());
    if (!magic) {
        return decode_failure<ModelAsset>(magic.error());
    }
    const auto model_version = magic.value() == model_magic_v5   ? 5U
                               : magic.value() == model_magic_v4 ? 4U
                               : magic.value() == model_magic_v3 ? 3U
                               : magic.value() == model_magic_v2 ? 2U
                                                                 : 0U;
    if (model_version == 0U) {
        return core::Result<ModelAsset>::failure("model_asset.invalid_magic",
                                                 "model asset payload has an unknown version");
    }
    auto vertex_count = reader.u32();
    auto index_count = reader.u32();
    auto node_count = reader.u32();
    auto primitive_count = reader.u32();
    auto image_count = reader.u32();
    auto material_count = reader.u32();
    auto skin_count = reader.u32();
    auto animation_count = reader.u32();
    if (!vertex_count || !index_count || !node_count || !primitive_count || !skin_count ||
        !image_count || !material_count || !animation_count) {
        const auto& error = !vertex_count      ? vertex_count.error()
                            : !index_count     ? index_count.error()
                            : !node_count      ? node_count.error()
                            : !primitive_count ? primitive_count.error()
                            : !image_count     ? image_count.error()
                            : !material_count  ? material_count.error()
                            : !skin_count      ? skin_count.error()
                                               : animation_count.error();
        return decode_failure<ModelAsset>(error);
    }
    if (vertex_count.value() > limits.maximum_vertices ||
        index_count.value() > limits.maximum_indices || node_count.value() > limits.maximum_nodes ||
        primitive_count.value() > limits.maximum_primitives ||
        image_count.value() > limits.maximum_images ||
        material_count.value() > limits.maximum_materials ||
        skin_count.value() > limits.maximum_skins ||
        animation_count.value() > limits.maximum_animations) {
        return core::Result<ModelAsset>::failure("model_asset.count_limit",
                                                 "model asset count exceeds a configured limit");
    }

    ModelAsset asset;
    auto bounds_min = read_vec3(reader);
    auto bounds_max = read_vec3(reader);
    if (!bounds_min || !bounds_max) {
        return decode_failure<ModelAsset>((!bounds_min ? bounds_min : bounds_max).error());
    }
    asset.bounds = {bounds_min.value(), bounds_max.value()};
    asset.vertices.resize(vertex_count.value());
    asset.indices.resize(index_count.value());
    asset.nodes.resize(node_count.value());
    asset.primitives.resize(primitive_count.value());
    asset.images.resize(image_count.value());
    asset.materials.resize(material_count.value());
    asset.skins.resize(skin_count.value());
    asset.animations.resize(animation_count.value());

    for (auto& vertex : asset.vertices) {
        auto position = read_vec3(reader);
        auto normal = read_vec3(reader);
        auto uv = read_vec2(reader);
        if (!position || !normal || !uv) {
            const auto& error = !position ? position.error()
                                : !normal ? normal.error()
                                          : uv.error();
            return decode_failure<ModelAsset>(error);
        }
        vertex.position = position.value();
        vertex.normal = normal.value();
        vertex.uv0 = uv.value();
        for (auto& joint : vertex.joints) {
            auto value = reader.u16();
            if (!value) {
                return decode_failure<ModelAsset>(value.error());
            }
            joint = value.value();
        }
        for (auto& weight : vertex.weights) {
            auto value = reader.f32();
            if (!value) {
                return decode_failure<ModelAsset>(value.error());
            }
            weight = value.value();
        }
    }
    for (auto& index : asset.indices) {
        auto value = reader.u32();
        if (!value) {
            return decode_failure<ModelAsset>(value.error());
        }
        index = value.value();
    }
    for (auto& node : asset.nodes) {
        auto name = reader.string(limits.maximum_name_bytes);
        auto parent = reader.u32();
        auto translation = read_vec3(reader);
        auto rotation = read_vec4(reader);
        auto scale = read_vec3(reader);
        if (!name || !parent || !translation || !rotation || !scale) {
            const auto& error = !name          ? name.error()
                                : !parent      ? parent.error()
                                : !translation ? translation.error()
                                : !rotation    ? rotation.error()
                                               : scale.error();
            return decode_failure<ModelAsset>(error);
        }
        node.name = std::move(name).value();
        node.parent = parent.value();
        node.bind_transform = {
            translation.value(),
            {rotation.value().x, rotation.value().y, rotation.value().z, rotation.value().w},
            scale.value()};
    }
    for (auto& primitive : asset.primitives) {
        auto name = reader.string(limits.maximum_name_bytes);
        auto first_vertex = reader.u32();
        auto vertices = reader.u32();
        auto first_index = reader.u32();
        auto indices = reader.u32();
        auto node = reader.u32();
        auto skin = reader.u32();
        auto material = reader.u32();
        if (!name || !first_vertex || !vertices || !first_index || !indices || !node || !skin ||
            !material) {
            const auto& error = !name           ? name.error()
                                : !first_vertex ? first_vertex.error()
                                : !vertices     ? vertices.error()
                                : !first_index  ? first_index.error()
                                : !indices      ? indices.error()
                                : !node         ? node.error()
                                : !skin         ? skin.error()
                                                : material.error();
            return decode_failure<ModelAsset>(error);
        }
        primitive = {std::move(name).value(),
                     first_vertex.value(),
                     vertices.value(),
                     first_index.value(),
                     indices.value(),
                     node.value(),
                     skin.value(),
                     material.value(),
                     {}};
    }
    std::size_t total_image_bytes = 0;
    for (auto& image : asset.images) {
        auto name = reader.string(limits.maximum_name_bytes);
        auto width = reader.u32();
        auto height = reader.u32();
        auto byte_count = reader.u32();
        if (!name || !width || !height || !byte_count) {
            const auto& error = !name     ? name.error()
                                : !width  ? width.error()
                                : !height ? height.error()
                                          : byte_count.error();
            return decode_failure<ModelAsset>(error);
        }
        const auto expected = static_cast<std::uint64_t>(width.value()) * height.value() * 4U;
        if (width.value() == 0 || height.value() == 0 ||
            width.value() > limits.maximum_image_dimension ||
            height.value() > limits.maximum_image_dimension || expected != byte_count.value() ||
            expected > limits.maximum_decoded_image_bytes - total_image_bytes) {
            return core::Result<ModelAsset>::failure(
                "model_asset.image_limit",
                "model asset image dimensions or decoded byte count exceed configured limits");
        }
        auto rgba8 = reader.bytes(byte_count.value());
        if (!rgba8) {
            return decode_failure<ModelAsset>(rgba8.error());
        }
        total_image_bytes += byte_count.value();
        image.name = std::move(name).value();
        image.width = width.value();
        image.height = height.value();
        image.rgba8.assign(rgba8.value().begin(), rgba8.value().end());
    }
    for (auto& material : asset.materials) {
        auto name = reader.string(limits.maximum_name_bytes);
        if (!name) {
            return decode_failure<ModelAsset>(name.error());
        }
        material.name = std::move(name).value();
        for (auto& value : material.base_color_factor) {
            auto decoded = reader.f32();
            if (!decoded) {
                return decode_failure<ModelAsset>(decoded.error());
            }
            value = decoded.value();
        }
        auto image = reader.u32();
        auto alpha_mode = reader.u8();
        auto alpha_cutoff = reader.f32();
        auto flags = reader.u8();
        if (!image || !alpha_mode || !alpha_cutoff || !flags) {
            const auto& error = !image          ? image.error()
                                : !alpha_mode   ? alpha_mode.error()
                                : !alpha_cutoff ? alpha_cutoff.error()
                                                : flags.error();
            return decode_failure<ModelAsset>(error);
        }
        const auto allowed_flags =
            model_version >= 3U ? known_model_material_flags : model_material_double_sided;
        if ((flags.value() & static_cast<std::uint8_t>(~allowed_flags)) != 0U) {
            return core::Result<ModelAsset>::failure(
                "model_asset.invalid_boolean", "model asset material contains unsupported flags");
        }
        material.base_color_texture.image = image.value();
        material.alpha_mode = static_cast<ModelAlphaMode>(alpha_mode.value());
        material.alpha_cutoff = alpha_cutoff.value();
        material.double_sided = (flags.value() & model_material_double_sided) != 0U;
        material.unlit = model_version >= 3U && (flags.value() & model_material_unlit) != 0U;
    }
    for (auto& skin : asset.skins) {
        auto name = reader.string(limits.maximum_name_bytes);
        auto root = reader.u32();
        auto joint_count = reader.u32();
        if (!name || !root || !joint_count) {
            const auto& error = !name ? name.error() : !root ? root.error() : joint_count.error();
            return decode_failure<ModelAsset>(error);
        }
        if (joint_count.value() == 0 || joint_count.value() > limits.maximum_joints_per_skin) {
            return core::Result<ModelAsset>::failure(
                "model_asset.joint_limit", "model asset joint count exceeds its configured limit");
        }
        skin.name = std::move(name).value();
        skin.skeleton_root = root.value();
        skin.joints.resize(joint_count.value());
        skin.inverse_bind_matrices.resize(joint_count.value());
        for (auto& joint : skin.joints) {
            auto value = reader.u32();
            if (!value) {
                return decode_failure<ModelAsset>(value.error());
            }
            joint = value.value();
        }
        for (auto& matrix : skin.inverse_bind_matrices) {
            for (auto& value : matrix.elements) {
                auto decoded = reader.f32();
                if (!decoded) {
                    return decode_failure<ModelAsset>(decoded.error());
                }
                value = decoded.value();
            }
        }
    }
    for (auto& clip : asset.animations) {
        auto name = reader.string(limits.maximum_name_bytes);
        auto duration = reader.f32();
        auto channel_count = reader.u32();
        if (!name || !duration || !channel_count) {
            const auto& error = !name       ? name.error()
                                : !duration ? duration.error()
                                            : channel_count.error();
            return decode_failure<ModelAsset>(error);
        }
        if (channel_count.value() == 0 ||
            channel_count.value() > limits.maximum_channels_per_animation) {
            return core::Result<ModelAsset>::failure(
                "model_asset.channel_limit",
                "model asset animation channel count exceeds its configured limit");
        }
        clip.name = std::move(name).value();
        clip.duration_seconds = duration.value();
        clip.channels.resize(channel_count.value());
        for (auto& channel : clip.channels) {
            auto node = reader.u32();
            auto path = reader.u8();
            auto interpolation = reader.u8();
            auto time_count = reader.u32();
            auto value_count = reader.u32();
            if (!node || !path || !interpolation || !time_count || !value_count) {
                const auto& error = !node            ? node.error()
                                    : !path          ? path.error()
                                    : !interpolation ? interpolation.error()
                                    : !time_count    ? time_count.error()
                                                     : value_count.error();
                return decode_failure<ModelAsset>(error);
            }
            if (time_count.value() == 0 ||
                time_count.value() > limits.maximum_keyframes_per_channel ||
                value_count.value() > limits.maximum_keyframes_per_channel * 3ULL) {
                return core::Result<ModelAsset>::failure(
                    "model_asset.keyframe_limit",
                    "model asset keyframe count exceeds its configured limit");
            }
            channel.node = node.value();
            channel.path = static_cast<ModelAnimationPath>(path.value());
            channel.interpolation = static_cast<ModelAnimationInterpolation>(interpolation.value());
            channel.times.resize(time_count.value());
            channel.values.resize(value_count.value());
            for (auto& time : channel.times) {
                auto value = reader.f32();
                if (!value) {
                    return decode_failure<ModelAsset>(value.error());
                }
                time = value.value();
            }
            for (auto& value : channel.values) {
                auto decoded = read_vec4(reader);
                if (!decoded) {
                    return decode_failure<ModelAsset>(decoded.error());
                }
                value = decoded.value();
            }
        }
    }
    if (model_version >= 4U) {
        for (auto& vertex : asset.vertices) {
            auto tangent = read_vec4(reader);
            auto uv1 = read_vec2(reader);
            if (!tangent || !uv1) {
                if (!tangent) {
                    return decode_failure<ModelAsset>(tangent.error());
                }
                return decode_failure<ModelAsset>(uv1.error());
            }
            vertex.tangent = tangent.value();
            vertex.uv1 = uv1.value();
            for (auto& component : vertex.color) {
                auto value = reader.f32();
                if (!value) {
                    return decode_failure<ModelAsset>(value.error());
                }
                component = value.value();
            }
        }

        auto sampler_count = reader.u32();
        if (!sampler_count) {
            return decode_failure<ModelAsset>(sampler_count.error());
        }
        if (sampler_count.value() > limits.maximum_samplers) {
            return core::Result<ModelAsset>::failure(
                "model_asset.sampler_limit",
                "model asset sampler count exceeds its configured limit");
        }
        asset.samplers.resize(sampler_count.value());
        for (auto& sampler : asset.samplers) {
            auto mag = reader.u8();
            auto min = reader.u8();
            auto wrap_s = reader.u8();
            auto wrap_t = reader.u8();
            if (!mag || !min || !wrap_s || !wrap_t) {
                const auto& error = !mag      ? mag.error()
                                    : !min    ? min.error()
                                    : !wrap_s ? wrap_s.error()
                                              : wrap_t.error();
                return decode_failure<ModelAsset>(error);
            }
            sampler.mag_filter = static_cast<ModelTextureMagFilter>(mag.value());
            sampler.min_filter = static_cast<ModelTextureMinFilter>(min.value());
            sampler.wrap_s = static_cast<ModelTextureWrap>(wrap_s.value());
            sampler.wrap_t = static_cast<ModelTextureWrap>(wrap_t.value());
        }

        const auto read_texture_binding = [&](ModelTextureBinding& binding) -> core::Status {
            auto image = reader.u32();
            auto sampler = reader.u32();
            auto texcoord = reader.u8();
            auto offset = read_vec2(reader);
            auto scale = read_vec2(reader);
            auto rotation = reader.f32();
            if (!image || !sampler || !texcoord || !offset || !scale || !rotation) {
                const auto& error = !image      ? image.error()
                                    : !sampler  ? sampler.error()
                                    : !texcoord ? texcoord.error()
                                    : !offset   ? offset.error()
                                    : !scale    ? scale.error()
                                                : rotation.error();
                return core::Status::failure(error.code, error.message);
            }
            binding.image = image.value();
            binding.sampler = sampler.value();
            binding.texcoord = texcoord.value();
            binding.offset = offset.value();
            binding.scale = scale.value();
            binding.rotation = rotation.value();
            return core::Status::ok();
        };
        for (auto& material : asset.materials) {
            for (auto& component : material.emissive_factor) {
                auto value = reader.f32();
                if (!value) {
                    return decode_failure<ModelAsset>(value.error());
                }
                component = value.value();
            }
            auto metallic = reader.f32();
            auto roughness = reader.f32();
            auto normal_scale = reader.f32();
            auto occlusion_strength = reader.f32();
            if (!metallic || !roughness || !normal_scale || !occlusion_strength) {
                const auto& error = !metallic       ? metallic.error()
                                    : !roughness    ? roughness.error()
                                    : !normal_scale ? normal_scale.error()
                                                    : occlusion_strength.error();
                return decode_failure<ModelAsset>(error);
            }
            material.metallic_factor = metallic.value();
            material.roughness_factor = roughness.value();
            material.normal_scale = normal_scale.value();
            material.occlusion_strength = occlusion_strength.value();
            for (auto* binding : {&material.base_color_texture,
                                  &material.metallic_roughness_texture, &material.normal_texture,
                                  &material.occlusion_texture, &material.emissive_texture}) {
                auto binding_status = read_texture_binding(*binding);
                if (!binding_status) {
                    return core::Result<ModelAsset>::failure(binding_status.error().code,
                                                             binding_status.error().message);
                }
            }
        }

        for (auto& node : asset.nodes) {
            auto weight_count = reader.u32();
            if (!weight_count) {
                return decode_failure<ModelAsset>(weight_count.error());
            }
            if (weight_count.value() > limits.maximum_morph_targets_per_primitive) {
                return core::Result<ModelAsset>::failure(
                    "model_asset.morph_limit",
                    "model node morph weight count exceeds its configured limit");
            }
            node.morph_weights.resize(weight_count.value());
            for (auto& weight : node.morph_weights) {
                auto value = reader.f32();
                if (!value) {
                    return decode_failure<ModelAsset>(value.error());
                }
                weight = value.value();
            }
        }

        std::size_t morph_delta_values = 0;
        for (auto& primitive : asset.primitives) {
            auto target_count = reader.u32();
            if (!target_count) {
                return decode_failure<ModelAsset>(target_count.error());
            }
            if (target_count.value() > limits.maximum_morph_targets_per_primitive) {
                return core::Result<ModelAsset>::failure(
                    "model_asset.morph_limit",
                    "model primitive morph target count exceeds its configured limit");
            }
            primitive.morph_targets.resize(target_count.value());
            for (auto& target : primitive.morph_targets) {
                auto attributes = reader.u8();
                if (!attributes) {
                    return decode_failure<ModelAsset>(attributes.error());
                }
                if ((attributes.value() & static_cast<std::uint8_t>(~0x07U)) != 0U) {
                    return core::Result<ModelAsset>::failure(
                        "model_asset.invalid_morph_target",
                        "model morph target contains unsupported attribute flags");
                }
                const auto read_deltas = [&](std::vector<math::Vec3f>& deltas,
                                             std::uint8_t bit) -> core::Status {
                    if ((attributes.value() & bit) == 0U) {
                        return core::Status::ok();
                    }
                    if (primitive.vertex_count >
                        limits.maximum_morph_delta_values - morph_delta_values) {
                        return core::Status::failure(
                            "model_asset.morph_limit",
                            "model morph target data exceeds its configured limit");
                    }
                    deltas.resize(primitive.vertex_count);
                    morph_delta_values += primitive.vertex_count;
                    for (auto& delta : deltas) {
                        auto value = read_vec3(reader);
                        if (!value) {
                            return core::Status::failure(value.error().code, value.error().message);
                        }
                        delta = value.value();
                    }
                    return core::Status::ok();
                };
                for (const auto& [deltas, bit] :
                     {std::pair{&target.position_deltas, static_cast<std::uint8_t>(1U << 0U)},
                      std::pair{&target.normal_deltas, static_cast<std::uint8_t>(1U << 1U)},
                      std::pair{&target.tangent_deltas, static_cast<std::uint8_t>(1U << 2U)}}) {
                    auto delta_status = read_deltas(*deltas, bit);
                    if (!delta_status) {
                        return core::Result<ModelAsset>::failure(delta_status.error().code,
                                                                 delta_status.error().message);
                    }
                }
            }
        }

        for (auto& clip : asset.animations) {
            for (auto& channel : clip.channels) {
                auto weight_count = reader.u32();
                auto value_count = reader.u32();
                if (!weight_count || !value_count) {
                    return decode_failure<ModelAsset>(
                        (!weight_count ? weight_count : value_count).error());
                }
                const auto maximum_values =
                    static_cast<std::uint64_t>(limits.maximum_keyframes_per_channel) *
                    limits.maximum_morph_targets_per_primitive * 3U;
                if (weight_count.value() > limits.maximum_morph_targets_per_primitive ||
                    value_count.value() > maximum_values) {
                    return core::Result<ModelAsset>::failure(
                        "model_asset.morph_limit",
                        "model morph animation data exceeds its configured limit");
                }
                channel.weight_count = weight_count.value();
                channel.weight_values.resize(value_count.value());
                for (auto& value : channel.weight_values) {
                    auto decoded = reader.f32();
                    if (!decoded) {
                        return decode_failure<ModelAsset>(decoded.error());
                    }
                    value = decoded.value();
                }
            }
        }
    }
    if (model_version >= 5U) {
        for (auto& primitive : asset.primitives) {
            auto minimum = read_vec3(reader);
            auto maximum = read_vec3(reader);
            auto lod_level = reader.u32();
            auto flags = reader.u8();
            if (!minimum || !maximum || !lod_level || !flags) {
                const auto& error = !minimum     ? minimum.error()
                                    : !maximum   ? maximum.error()
                                    : !lod_level ? lod_level.error()
                                                 : flags.error();
                return decode_failure<ModelAsset>(error);
            }
            if ((flags.value() & static_cast<std::uint8_t>(~0x03U)) != 0U) {
                return core::Result<ModelAsset>::failure(
                    "model_asset.invalid_primitive_flags",
                    "model primitive contains unsupported production flags");
            }
            primitive.bounds = {minimum.value(), maximum.value()};
            primitive.lod_level = lod_level.value();
            primitive.renderable = (flags.value() & 1U) != 0U;
            primitive.collision_source = (flags.value() & 2U) != 0U;
        }
        auto socket_count = reader.u32();
        if (!socket_count || socket_count.value() > limits.maximum_sockets) {
            return core::Result<ModelAsset>::failure(
                "model_asset.socket_limit", "model socket count exceeds its configured limit");
        }
        asset.sockets.resize(socket_count.value());
        for (auto& socket : asset.sockets) {
            auto name = reader.string(limits.maximum_name_bytes);
            auto node = reader.u32();
            if (!name || !node) {
                return decode_failure<ModelAsset>(!name ? name.error() : node.error());
            }
            socket.name = std::move(name).value();
            socket.node = node.value();
        }
        auto lod_count = reader.u32();
        if (!lod_count || lod_count.value() > limits.maximum_lods) {
            return core::Result<ModelAsset>::failure(
                "model_asset.lod_limit", "model LOD count exceeds its configured limit");
        }
        asset.lods.resize(lod_count.value());
        for (auto& lod : asset.lods) {
            auto level = reader.u32();
            auto coverage = reader.f32();
            auto error = reader.f32();
            auto lod_primitive_count = reader.u32();
            if (!level || !coverage || !error || !lod_primitive_count) {
                const auto& failure = !level      ? level.error()
                                      : !coverage ? coverage.error()
                                      : !error    ? error.error()
                                                  : lod_primitive_count.error();
                return decode_failure<ModelAsset>(failure);
            }
            if (lod_primitive_count.value() > limits.maximum_primitives) {
                return core::Result<ModelAsset>::failure(
                    "model_asset.lod_primitive_limit",
                    "model LOD primitive count exceeds its configured limit");
            }
            lod.level = level.value();
            lod.screen_coverage = coverage.value();
            lod.geometric_error = error.value();
            lod.primitives.resize(lod_primitive_count.value());
            for (auto& primitive : lod.primitives) {
                auto value = reader.u32();
                if (!value) {
                    return decode_failure<ModelAsset>(value.error());
                }
                primitive = value.value();
            }
        }
        auto collision_count = reader.u32();
        if (!collision_count || collision_count.value() > limits.maximum_collision_shapes) {
            return core::Result<ModelAsset>::failure(
                "model_asset.collision_limit",
                "model collision shape count exceeds its configured limit");
        }
        asset.collision_shapes.resize(collision_count.value());
        for (auto& collision : asset.collision_shapes) {
            auto name = reader.string(limits.maximum_name_bytes);
            auto node = reader.u32();
            auto minimum = read_vec3(reader);
            auto maximum = read_vec3(reader);
            if (!name || !node || !minimum || !maximum) {
                const auto& error = !name      ? name.error()
                                    : !node    ? node.error()
                                    : !minimum ? minimum.error()
                                               : maximum.error();
                return decode_failure<ModelAsset>(error);
            }
            collision = {std::move(name).value(), node.value(), {minimum.value(), maximum.value()}};
        }
        auto camera_count = reader.u32();
        if (!camera_count || camera_count.value() > limits.maximum_cameras) {
            return core::Result<ModelAsset>::failure(
                "model_asset.camera_limit", "model camera count exceeds its configured limit");
        }
        asset.cameras.resize(camera_count.value());
        for (auto& camera : asset.cameras) {
            auto name = reader.string(limits.maximum_name_bytes);
            auto node = reader.u32();
            auto kind = reader.u8();
            auto aspect = reader.f32();
            auto fov = reader.f32();
            auto xmag = reader.f32();
            auto ymag = reader.f32();
            auto near_plane = reader.f32();
            auto far_plane = reader.f32();
            if (!name || !node || !kind || !aspect || !fov || !xmag || !ymag || !near_plane ||
                !far_plane) {
                const auto& error = !name         ? name.error()
                                    : !node       ? node.error()
                                    : !kind       ? kind.error()
                                    : !aspect     ? aspect.error()
                                    : !fov        ? fov.error()
                                    : !xmag       ? xmag.error()
                                    : !ymag       ? ymag.error()
                                    : !near_plane ? near_plane.error()
                                                  : far_plane.error();
                return decode_failure<ModelAsset>(error);
            }
            camera = {std::move(name).value(),
                      node.value(),
                      static_cast<ModelCameraKind>(kind.value()),
                      aspect.value(),
                      fov.value(),
                      xmag.value(),
                      ymag.value(),
                      near_plane.value(),
                      far_plane.value()};
        }
        auto light_count = reader.u32();
        if (!light_count || light_count.value() > limits.maximum_lights) {
            return core::Result<ModelAsset>::failure(
                "model_asset.light_limit", "model light count exceeds its configured limit");
        }
        asset.lights.resize(light_count.value());
        for (auto& light : asset.lights) {
            auto name = reader.string(limits.maximum_name_bytes);
            auto node = reader.u32();
            auto kind = reader.u8();
            auto color = read_vec3(reader);
            auto intensity = reader.f32();
            auto range = reader.f32();
            auto inner = reader.f32();
            auto outer = reader.f32();
            if (!name || !node || !kind || !color || !intensity || !range || !inner || !outer) {
                const auto& error = !name        ? name.error()
                                    : !node      ? node.error()
                                    : !kind      ? kind.error()
                                    : !color     ? color.error()
                                    : !intensity ? intensity.error()
                                    : !range     ? range.error()
                                    : !inner     ? inner.error()
                                                 : outer.error();
                return decode_failure<ModelAsset>(error);
            }
            light = {std::move(name).value(),
                     node.value(),
                     static_cast<ModelLightKind>(kind.value()),
                     color.value(),
                     intensity.value(),
                     range.value(),
                     inner.value(),
                     outer.value()};
        }
    }
    if (!reader.at_end()) {
        return core::Result<ModelAsset>::failure("model_asset.trailing_data",
                                                 "model asset payload has trailing data");
    }
    auto status = validate_model_asset(asset, limits);
    if (!status) {
        return core::Result<ModelAsset>::failure(status.error().code, status.error().message);
    }
    return core::Result<ModelAsset>::success(std::move(asset));
}

std::string_view model_animation_path_name(ModelAnimationPath path) noexcept {
    switch (path) {
    case ModelAnimationPath::translation:
        return "translation";
    case ModelAnimationPath::rotation:
        return "rotation";
    case ModelAnimationPath::scale:
        return "scale";
    case ModelAnimationPath::weights:
        return "weights";
    }
    return "unknown";
}

std::string_view
model_animation_interpolation_name(ModelAnimationInterpolation interpolation) noexcept {
    switch (interpolation) {
    case ModelAnimationInterpolation::step:
        return "step";
    case ModelAnimationInterpolation::linear:
        return "linear";
    case ModelAnimationInterpolation::cubic_spline:
        return "cubic_spline";
    }
    return "unknown";
}

std::string_view model_alpha_mode_name(ModelAlphaMode mode) noexcept {
    switch (mode) {
    case ModelAlphaMode::opaque:
        return "opaque";
    case ModelAlphaMode::mask:
        return "mask";
    case ModelAlphaMode::blend:
        return "blend";
    }
    return "unknown";
}

ModelCapabilities model_capabilities(const ModelAsset& model) noexcept {
    ModelCapabilities result;
    result.has_animation_clips = !model.animations.empty();
    result.has_skins = std::ranges::any_of(model.primitives, [](const ModelPrimitive& primitive) {
        return primitive.skin != no_model_index;
    });
    result.has_morph_targets =
        std::ranges::any_of(model.primitives, [](const ModelPrimitive& primitive) {
            return !primitive.morph_targets.empty();
        });
    result.has_animated_nodes =
        std::ranges::any_of(model.animations, [](const ModelAnimationClip& clip) {
            return std::ranges::any_of(clip.channels, [](const ModelAnimationChannel& channel) {
                return channel.path == ModelAnimationPath::translation ||
                       channel.path == ModelAnimationPath::rotation ||
                       channel.path == ModelAnimationPath::scale;
            });
        });
    result.has_lods = model.lods.size() > 1U;
    result.has_sockets = !model.sockets.empty();
    result.has_collision_metadata = !model.collision_shapes.empty();
    result.has_cameras = !model.cameras.empty();
    result.has_lights = !model.lights.empty();
    return result;
}

core::Result<std::uint32_t> resolve_model_animation_clip(const ModelAsset& model,
                                                         std::string_view clip_name) {
    if (clip_name.empty()) {
        return core::Result<std::uint32_t>::failure(
            "model_asset.empty_animation_name",
            "animation clip lookup requires a non-empty exported clip name");
    }
    auto resolved = no_model_index;
    for (std::uint32_t index = 0; index < model.animations.size(); ++index) {
        if (model.animations[index].name != clip_name) {
            continue;
        }
        if (resolved != no_model_index) {
            return core::Result<std::uint32_t>::failure(
                "model_asset.ambiguous_animation_name",
                "animation clip name is duplicated in the model: " + std::string(clip_name));
        }
        resolved = index;
    }
    if (resolved == no_model_index) {
        return core::Result<std::uint32_t>::failure("model_asset.missing_animation_name",
                                                    "animation clip does not exist in the model: " +
                                                        std::string(clip_name));
    }
    return core::Result<std::uint32_t>::success(resolved);
}

} // namespace heartstead::assets
