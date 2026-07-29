#include "engine/assets/model_asset.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>

namespace heartstead::assets {

namespace {

constexpr std::string_view model_magic = "heartstead.model.v2";
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
    if (!vertex.position.is_finite() || !vertex.normal.is_finite() || !vertex.uv.is_finite()) {
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
        (material.base_color_image != no_model_index &&
         material.base_color_image >= asset.images.size()) ||
        !std::isfinite(material.alpha_cutoff) || material.alpha_cutoff < 0.0F) {
        return false;
    }
    switch (material.alpha_mode) {
    case ModelAlphaMode::opaque:
    case ModelAlphaMode::mask:
        break;
    default:
        return false;
    }
    return std::ranges::all_of(material.base_color_factor, [](float value) {
        return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
    });
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
    if (channel.values.size() != channel.times.size() * multiplier) {
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
        maximum_images == 0 || maximum_materials == 0 || maximum_image_dimension == 0 ||
        maximum_decoded_image_bytes < 4 || maximum_joints_per_skin == 0 ||
        maximum_joints_per_skin > UINT16_MAX || maximum_animations == 0 ||
        maximum_channels_per_animation == 0 || maximum_keyframes_per_channel == 0 ||
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
        asset.materials.size() > limits.maximum_materials ||
        asset.skins.size() > limits.maximum_skins ||
        asset.animations.size() > limits.maximum_animations || !asset.bounds.is_valid()) {
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
    if (!std::ranges::all_of(asset.materials, [&](const ModelMaterial& material) {
            return valid_model_material(material, asset, limits);
        })) {
        return core::Status::failure(
            "model_asset.invalid_material",
            "model asset material parameters or base-color image binding are invalid");
    }
    for (const auto& node : asset.nodes) {
        if (!valid_name(node.name, limits) || !node.bind_transform.is_valid()) {
            return core::Status::failure("model_asset.invalid_node",
                                         "model asset node name or bind transform is invalid");
        }
    }
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
            primitive.index_count % 3U != 0) {
            return core::Status::failure("model_asset.invalid_primitive",
                                         "model asset primitive range or binding is invalid");
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
    writer.string(model_magic);
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
        write_vec2(writer, vertex.uv);
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
        writer.u32(material.base_color_image);
        writer.u8(static_cast<std::uint8_t>(material.alpha_mode));
        writer.f32(material.alpha_cutoff);
        writer.u8(material.double_sided ? 1U : 0U);
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
    return core::Result<std::vector<std::uint8_t>>::success(writer.take());
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
    auto magic = reader.string(model_magic.size());
    if (!magic) {
        return decode_failure<ModelAsset>(magic.error());
    }
    if (magic.value() != model_magic) {
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
        vertex.uv = uv.value();
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
        primitive = {
            std::move(name).value(), first_vertex.value(), vertices.value(), first_index.value(),
            indices.value(),         node.value(),         skin.value(),     material.value()};
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
        auto double_sided = reader.u8();
        if (!image || !alpha_mode || !alpha_cutoff || !double_sided) {
            const auto& error = !image          ? image.error()
                                : !alpha_mode   ? alpha_mode.error()
                                : !alpha_cutoff ? alpha_cutoff.error()
                                                : double_sided.error();
            return decode_failure<ModelAsset>(error);
        }
        if (double_sided.value() > 1U) {
            return core::Result<ModelAsset>::failure(
                "model_asset.invalid_boolean",
                "model asset material contains an invalid double-sided flag");
        }
        material.base_color_image = image.value();
        material.alpha_mode = static_cast<ModelAlphaMode>(alpha_mode.value());
        material.alpha_cutoff = alpha_cutoff.value();
        material.double_sided = double_sided.value() != 0U;
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
    }
    return "unknown";
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
