#include "engine/assets/model_asset.hpp"

#include "engine/assets/image_asset.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <draco/compression/decode.h>
#include <draco/core/decoder_buffer.h>
#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace heartstead::assets {

namespace {

constexpr auto supported_gltf_extensions =
    fastgltf::Extensions::KHR_materials_unlit | fastgltf::Extensions::KHR_texture_transform |
    fastgltf::Extensions::KHR_texture_basisu | fastgltf::Extensions::KHR_mesh_quantization |
    fastgltf::Extensions::KHR_lights_punctual | fastgltf::Extensions::EXT_meshopt_compression |
    fastgltf::Extensions::KHR_draco_mesh_compression;

[[nodiscard]] core::Result<ModelAsset> importer_failure(std::string code, std::string message) {
    return core::Result<ModelAsset>::failure(std::move(code), std::move(message));
}

[[nodiscard]] core::Result<ModelAsset> fastgltf_failure(fastgltf::Error error,
                                                        std::string_view operation) {
    return importer_failure("gltf_import.parse_failed",
                            std::string(operation) + ": " +
                                std::string(fastgltf::getErrorName(error)) + " (" +
                                std::string(fastgltf::getErrorMessage(error)) + ")");
}

[[nodiscard]] core::Result<std::string> model_name(std::string_view source, std::string_view prefix,
                                                   std::size_t index,
                                                   const ModelAssetLimits& limits) {
    auto result =
        source.empty() ? std::string(prefix) + "_" + std::to_string(index) : std::string(source);
    if (result.size() > limits.maximum_name_bytes || result.find('\0') != std::string::npos) {
        return core::Result<std::string>::failure(
            "gltf_import.name_limit", "glTF object name exceeds the configured model name limit");
    }
    return core::Result<std::string>::success(std::move(result));
}

[[nodiscard]] ModelQuaternion normalized_quaternion(const fastgltf::math::fquat& value) {
    ModelQuaternion result{value[0], value[1], value[2], value[3]};
    const auto magnitude = std::sqrt(result.length_squared());
    if (std::isfinite(magnitude) && magnitude > 0.0F) {
        result.x /= magnitude;
        result.y /= magnitude;
        result.z /= magnitude;
        result.w /= magnitude;
    }
    return result;
}

[[nodiscard]] math::Mat4f model_matrix(const fastgltf::math::fmat4x4& source) {
    math::Mat4f result;
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            result.at(row, column) = source[column][row];
        }
    }
    return result;
}

[[nodiscard]] core::Status check_append_count(std::size_t current, std::size_t additional,
                                              std::size_t maximum, std::string_view label) {
    if (additional > maximum || current > maximum - additional) {
        return core::Status::failure("gltf_import.count_limit",
                                     "glTF " + std::string(label) +
                                         " count exceeds its configured limit");
    }
    return core::Status::ok();
}

[[nodiscard]] std::string lowercase_ascii(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return result;
}

[[nodiscard]] std::optional<std::string>
node_marker_payload(std::string_view source_name, std::span<const std::string_view> markers) {
    const auto lowered = lowercase_ascii(source_name);
    for (const auto marker : markers) {
        if (lowered.starts_with(marker) && source_name.size() > marker.size()) {
            return std::string(source_name.substr(marker.size()));
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool is_collision_node(std::string_view source_name) {
    constexpr std::array collision_markers{
        std::string_view{"collision_"}, std::string_view{"collision."},
        std::string_view{"collision:"}, std::string_view{"ucx_"},
        std::string_view{"ubx_"},       std::string_view{"ucp_"},
        std::string_view{"usp_"},
    };
    return node_marker_payload(source_name, collision_markers).has_value();
}

[[nodiscard]] core::Result<std::uint32_t> node_lod_level(std::string_view source_name,
                                                         const ModelAssetLimits& limits) {
    const auto lowered = lowercase_ascii(source_name);
    for (std::size_t offset = 0; offset + 3U < lowered.size(); ++offset) {
        const auto at_token_start = offset == 0 || lowered[offset - 1U] == '_' ||
                                    lowered[offset - 1U] == '.' || lowered[offset - 1U] == '-' ||
                                    lowered[offset - 1U] == ':';
        if (!at_token_start || lowered.substr(offset, 3U) != "lod") {
            continue;
        }
        std::size_t cursor = offset + 3U;
        std::uint32_t level = 0;
        bool has_digit = false;
        while (cursor < lowered.size() && lowered[cursor] >= '0' && lowered[cursor] <= '9') {
            has_digit = true;
            const auto digit = static_cast<std::uint32_t>(lowered[cursor] - '0');
            if (level > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U) {
                return core::Result<std::uint32_t>::failure(
                    "gltf_import.lod_limit", "glTF node LOD suffix overflows its level");
            }
            level = level * 10U + digit;
            ++cursor;
        }
        if (has_digit) {
            if (level >= limits.maximum_lods) {
                return core::Result<std::uint32_t>::failure(
                    "gltf_import.lod_limit",
                    "glTF node LOD suffix exceeds the configured LOD count");
            }
            return core::Result<std::uint32_t>::success(level);
        }
    }
    return core::Result<std::uint32_t>::success(0U);
}

[[nodiscard]] core::Result<std::span<const std::byte>>
buffer_source_bytes(const fastgltf::Asset& source, std::size_t buffer_index) {
    if (buffer_index >= source.buffers.size()) {
        return core::Result<std::span<const std::byte>>::failure(
            "gltf_import.buffer_out_of_bounds", "glTF compressed data references a missing buffer");
    }
    return std::visit(
        [](const auto& data) -> core::Result<std::span<const std::byte>> {
            using Source = std::remove_cvref_t<decltype(data)>;
            if constexpr (std::is_same_v<Source, fastgltf::sources::Array> ||
                          std::is_same_v<Source, fastgltf::sources::Vector>) {
                return core::Result<std::span<const std::byte>>::success(
                    {data.bytes.data(), data.bytes.size()});
            } else if constexpr (std::is_same_v<Source, fastgltf::sources::ByteView>) {
                return core::Result<std::span<const std::byte>>::success(
                    {data.bytes.data(), data.bytes.size()});
            } else {
                return core::Result<std::span<const std::byte>>::failure(
                    "gltf_import.buffer_not_loaded",
                    "glTF compressed buffer source was not loaded into the model cooker");
            }
        },
        source.buffers[buffer_index].data);
}

[[nodiscard]] core::Result<std::span<const std::byte>>
buffer_view_bytes(const fastgltf::Asset& source, std::size_t buffer_view_index) {
    if (buffer_view_index >= source.bufferViews.size()) {
        return core::Result<std::span<const std::byte>>::failure(
            "gltf_import.buffer_view_out_of_bounds",
            "glTF compressed data references a missing buffer view");
    }
    const auto& view = source.bufferViews[buffer_view_index];
    auto buffer = buffer_source_bytes(source, view.bufferIndex);
    if (!buffer) {
        return buffer;
    }
    if (view.byteOffset > buffer.value().size() ||
        view.byteLength > buffer.value().size() - view.byteOffset) {
        return core::Result<std::span<const std::byte>>::failure(
            "gltf_import.buffer_view_out_of_bounds", "glTF buffer view exceeds its loaded buffer");
    }
    return core::Result<std::span<const std::byte>>::success(
        buffer.value().subspan(view.byteOffset, view.byteLength));
}

[[nodiscard]] std::size_t append_buffer(fastgltf::Asset& asset, std::span<const std::byte> bytes,
                                        std::string_view name) {
    fastgltf::sources::Array storage{fastgltf::StaticVector<std::byte>(bytes.size()),
                                     fastgltf::MimeType::None};
    std::ranges::copy(bytes, storage.bytes.begin());
    fastgltf::Buffer buffer;
    buffer.byteLength = bytes.size();
    buffer.data = std::move(storage);
    buffer.name = name;
    asset.buffers.push_back(std::move(buffer));
    return asset.buffers.size() - 1U;
}

[[nodiscard]] core::Status decompress_meshopt_buffer_views(fastgltf::Asset& asset,
                                                           const ModelAssetLimits& limits) {
    std::size_t decoded_total = 0;
    for (auto& view : asset.bufferViews) {
        if (view.meshoptCompression == nullptr) {
            continue;
        }
        const auto& compressed = *view.meshoptCompression;
        auto source = buffer_source_bytes(asset, compressed.bufferIndex);
        if (!source) {
            return core::Status::failure(source.error().code, source.error().message);
        }
        if (compressed.byteOffset > source.value().size() ||
            compressed.byteLength > source.value().size() - compressed.byteOffset ||
            compressed.byteStride == 0 ||
            compressed.count > std::numeric_limits<std::size_t>::max() / compressed.byteStride) {
            return core::Status::failure(
                "gltf_import.invalid_meshopt_buffer",
                "EXT_meshopt_compression source range or decoded layout is invalid");
        }
        const auto decoded_size = compressed.count * compressed.byteStride;
        if (decoded_size > limits.maximum_source_bytes - decoded_total) {
            return core::Status::failure(
                "gltf_import.buffer_limit",
                "decoded EXT_meshopt_compression data exceeds the source byte limit");
        }
        decoded_total += decoded_size;
        std::vector<std::byte> decoded(decoded_size);
        const auto encoded = source.value().subspan(compressed.byteOffset, compressed.byteLength);
        const auto* encoded_data = reinterpret_cast<const unsigned char*>(encoded.data());
        int decode_result = -1;
        switch (compressed.mode) {
        case fastgltf::MeshoptCompressionMode::Attributes:
            decode_result =
                meshopt_decodeVertexBuffer(decoded.data(), compressed.count, compressed.byteStride,
                                           encoded_data, encoded.size());
            break;
        case fastgltf::MeshoptCompressionMode::Triangles:
            decode_result =
                meshopt_decodeIndexBuffer(decoded.data(), compressed.count, compressed.byteStride,
                                          encoded_data, encoded.size());
            break;
        case fastgltf::MeshoptCompressionMode::Indices:
            decode_result =
                meshopt_decodeIndexSequence(decoded.data(), compressed.count, compressed.byteStride,
                                            encoded_data, encoded.size());
            break;
        }
        if (decode_result != 0) {
            return core::Status::failure(
                "gltf_import.meshopt_decode_failed",
                "meshoptimizer rejected an EXT_meshopt_compression payload");
        }
        switch (compressed.filter) {
        case fastgltf::MeshoptCompressionFilter::None:
            break;
        case fastgltf::MeshoptCompressionFilter::Octahedral:
            meshopt_decodeFilterOct(decoded.data(), compressed.count, compressed.byteStride);
            break;
        case fastgltf::MeshoptCompressionFilter::Quaternion:
            meshopt_decodeFilterQuat(decoded.data(), compressed.count, compressed.byteStride);
            break;
        case fastgltf::MeshoptCompressionFilter::Exponential:
            meshopt_decodeFilterExp(decoded.data(), compressed.count, compressed.byteStride);
            break;
        }
        const auto buffer_index = append_buffer(asset, decoded, "meshopt_decoded");
        view.bufferIndex = buffer_index;
        view.byteOffset = 0;
        view.byteLength = decoded.size();
        if (compressed.mode == fastgltf::MeshoptCompressionMode::Attributes) {
            view.byteStride = compressed.byteStride;
        }
        view.meshoptCompression.reset();
    }
    return core::Status::ok();
}

[[nodiscard]] std::size_t accessor_components(fastgltf::AccessorType type) {
    switch (type) {
    case fastgltf::AccessorType::Scalar:
        return 1;
    case fastgltf::AccessorType::Vec2:
        return 2;
    case fastgltf::AccessorType::Vec3:
        return 3;
    case fastgltf::AccessorType::Vec4:
        return 4;
    default:
        return 0;
    }
}

[[nodiscard]] core::Status decompress_draco_primitives(fastgltf::Asset& asset,
                                                       const ModelAssetLimits& limits) {
    std::size_t decoded_total = 0;
    for (auto& mesh : asset.meshes) {
        for (auto& primitive : mesh.primitives) {
            if (primitive.dracoCompression == nullptr) {
                continue;
            }
            auto encoded = buffer_view_bytes(asset, primitive.dracoCompression->bufferView);
            if (!encoded) {
                return core::Status::failure(encoded.error().code, encoded.error().message);
            }
            draco::DecoderBuffer decoder_buffer;
            decoder_buffer.Init(reinterpret_cast<const char*>(encoded.value().data()),
                                encoded.value().size());
            draco::Decoder decoder;
            auto decoded = decoder.DecodeMeshFromBuffer(&decoder_buffer);
            if (!decoded.ok() || decoded.value() == nullptr) {
                return core::Status::failure(
                    "gltf_import.draco_decode_failed",
                    "Draco rejected a KHR_draco_mesh_compression payload: " +
                        decoded.status().error_msg_string());
            }
            auto draco_mesh = std::move(decoded).value();
            const auto point_count = static_cast<std::size_t>(draco_mesh->num_points());
            if (point_count == 0 || point_count > limits.maximum_vertices) {
                return core::Status::failure(
                    "gltf_import.vertex_limit",
                    "decoded Draco vertex count is empty or exceeds its configured limit");
            }
            for (const auto& primitive_attribute : primitive.attributes) {
                const auto draco_mapping =
                    primitive.dracoCompression->findAttribute(primitive_attribute.name);
                if (draco_mapping == primitive.dracoCompression->attributes.end() ||
                    primitive_attribute.accessorIndex >= asset.accessors.size()) {
                    return core::Status::failure("gltf_import.invalid_draco_attribute",
                                                 "Draco primitive attribute mapping is incomplete");
                }
                const auto* attribute = draco_mesh->GetAttributeByUniqueId(
                    static_cast<std::uint32_t>(draco_mapping->accessorIndex));
                auto& accessor = asset.accessors[primitive_attribute.accessorIndex];
                const auto components = accessor_components(accessor.type);
                if (attribute == nullptr || components == 0 || accessor.count != point_count) {
                    return core::Status::failure(
                        "gltf_import.invalid_draco_attribute",
                        "decoded Draco attribute does not match its glTF accessor");
                }
                const auto joints = primitive_attribute.name.starts_with("JOINTS_");
                const auto component_bytes = joints ? sizeof(std::uint16_t) : sizeof(float);
                const auto decoded_size = point_count * components * component_bytes;
                if (decoded_size > limits.maximum_source_bytes - decoded_total) {
                    return core::Status::failure(
                        "gltf_import.buffer_limit",
                        "decoded Draco data exceeds the source byte limit");
                }
                decoded_total += decoded_size;
                bool conversion_failed = false;
                std::vector<std::uint16_t> joint_values;
                std::vector<float> attribute_values;
                if (joints) {
                    joint_values.resize(point_count * components);
                } else {
                    attribute_values.resize(point_count * components);
                }
                for (std::size_t point = 0; point < point_count; ++point) {
                    const auto mapped = attribute->mapped_index(
                        draco::PointIndex(static_cast<std::uint32_t>(point)));
                    if (joints) {
                        auto* destination = joint_values.data() + point * components;
                        conversion_failed |= !attribute->ConvertValue(
                            mapped, static_cast<std::int8_t>(components), destination);
                    } else {
                        auto* destination = attribute_values.data() + point * components;
                        conversion_failed |= !attribute->ConvertValue(
                            mapped, static_cast<std::int8_t>(components), destination);
                    }
                }
                if (conversion_failed) {
                    return core::Status::failure(
                        "gltf_import.draco_attribute_conversion_failed",
                        "decoded Draco attribute could not be converted to runtime values");
                }
                const auto attribute_bytes = joints ? std::as_bytes(std::span{joint_values})
                                                    : std::as_bytes(std::span{attribute_values});
                const auto buffer = append_buffer(asset, attribute_bytes, "draco_attribute");
                fastgltf::BufferView view;
                view.bufferIndex = buffer;
                view.byteLength = attribute_bytes.size();
                asset.bufferViews.push_back(std::move(view));
                accessor.bufferViewIndex = asset.bufferViews.size() - 1U;
                accessor.byteOffset = 0;
                accessor.componentType = joints ? fastgltf::ComponentType::UnsignedShort
                                                : fastgltf::ComponentType::Float;
                accessor.normalized = false;
                accessor.sparse.reset();
            }
            if (!primitive.indicesAccessor.has_value() ||
                *primitive.indicesAccessor >= asset.accessors.size()) {
                return core::Status::failure(
                    "gltf_import.invalid_draco_indices",
                    "Draco triangle primitive is missing its index accessor");
            }
            const auto index_count = static_cast<std::size_t>(draco_mesh->num_faces()) * 3U;
            if (index_count == 0 || index_count > limits.maximum_indices) {
                return core::Status::failure(
                    "gltf_import.index_limit",
                    "decoded Draco index count is empty or exceeds its configured limit");
            }
            if (index_count >
                (limits.maximum_source_bytes - decoded_total) / sizeof(std::uint32_t)) {
                return core::Status::failure("gltf_import.buffer_limit",
                                             "decoded Draco indices exceed the source byte limit");
            }
            decoded_total += index_count * sizeof(std::uint32_t);
            std::vector<std::uint32_t> indices(index_count);
            for (std::uint32_t face = 0; face < draco_mesh->num_faces(); ++face) {
                const auto& source_face = draco_mesh->face(draco::FaceIndex(face));
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    indices[static_cast<std::size_t>(face) * 3U + corner] =
                        source_face[corner].value();
                }
            }
            const auto index_bytes = std::as_bytes(std::span{indices});
            const auto buffer = append_buffer(asset, index_bytes, "draco_indices");
            fastgltf::BufferView view;
            view.bufferIndex = buffer;
            view.byteLength = index_bytes.size();
            asset.bufferViews.push_back(std::move(view));
            auto& accessor = asset.accessors[*primitive.indicesAccessor];
            accessor.bufferViewIndex = asset.bufferViews.size() - 1U;
            accessor.byteOffset = 0;
            accessor.count = indices.size();
            accessor.type = fastgltf::AccessorType::Scalar;
            accessor.componentType = fastgltf::ComponentType::UnsignedInt;
            accessor.normalized = false;
            accessor.sparse.reset();
            primitive.dracoCompression.reset();
        }
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status import_nodes(const fastgltf::Asset& source, ModelAsset& target,
                                        const ModelAssetLimits& limits) {
    if (source.nodes.empty() || source.nodes.size() > limits.maximum_nodes) {
        return core::Status::failure("gltf_import.node_limit",
                                     "glTF model must contain a bounded node hierarchy");
    }
    target.nodes.resize(source.nodes.size());
    for (std::size_t index = 0; index < source.nodes.size(); ++index) {
        const auto& source_node = source.nodes[index];
        auto name = model_name(source_node.name, "node", index, limits);
        if (!name) {
            return core::Status::failure(name.error().code, name.error().message);
        }
        const auto* transform = std::get_if<fastgltf::TRS>(&source_node.transform);
        if (transform == nullptr) {
            return core::Status::failure(
                "gltf_import.matrix_not_decomposed",
                "glTF node matrix could not be decomposed to a runtime TRS transform");
        }
        target.nodes[index].name = std::move(name).value();
        target.nodes[index].bind_transform = {
            {transform->translation[0], transform->translation[1], transform->translation[2]},
            normalized_quaternion(transform->rotation),
            {transform->scale[0], transform->scale[1], transform->scale[2]},
        };
        const auto& default_weights =
            !source_node.weights.empty() ? source_node.weights
            : source_node.meshIndex.has_value() && *source_node.meshIndex < source.meshes.size()
                ? source.meshes[*source_node.meshIndex].weights
                : source_node.weights;
        target.nodes[index].morph_weights.reserve(default_weights.size());
        for (const auto weight : default_weights) {
            target.nodes[index].morph_weights.push_back(static_cast<float>(weight));
        }
        for (const auto child : source_node.children) {
            if (child >= source.nodes.size()) {
                return core::Status::failure("gltf_import.child_out_of_bounds",
                                             "glTF node references a missing child");
            }
            if (target.nodes[child].parent != no_model_index) {
                return core::Status::failure("gltf_import.multiple_parents",
                                             "glTF node hierarchy gives a child multiple parents");
            }
            target.nodes[child].parent = static_cast<std::uint32_t>(index);
        }
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status import_node_metadata(const fastgltf::Asset& source, ModelAsset& target,
                                                const ModelAssetLimits& limits) {
    constexpr std::array socket_markers{
        std::string_view{"socket_"},
        std::string_view{"socket."},
        std::string_view{"socket:"},
    };
    for (std::size_t node_index = 0; node_index < source.nodes.size(); ++node_index) {
        const auto& source_node = source.nodes[node_index];
        if (auto socket_name = node_marker_payload(source_node.name, socket_markers);
            socket_name.has_value()) {
            if (target.sockets.size() >= limits.maximum_sockets ||
                socket_name->size() > limits.maximum_name_bytes) {
                return core::Status::failure(
                    "gltf_import.socket_limit",
                    "glTF socket name or socket count exceeds its configured limit");
            }
            target.sockets.push_back(
                {std::move(*socket_name), static_cast<std::uint32_t>(node_index)});
        }

        if (source_node.cameraIndex.has_value()) {
            if (*source_node.cameraIndex >= source.cameras.size() ||
                target.cameras.size() >= limits.maximum_cameras) {
                return core::Status::failure(
                    "gltf_import.camera_out_of_bounds",
                    "glTF node camera reference is missing or exceeds its configured limit");
            }
            const auto& source_camera = source.cameras[*source_node.cameraIndex];
            auto name = model_name(source_camera.name, "camera", target.cameras.size(), limits);
            if (!name) {
                return core::Status::failure(name.error().code, name.error().message);
            }
            ModelCamera camera;
            camera.name = std::move(name).value();
            camera.node = static_cast<std::uint32_t>(node_index);
            if (const auto* perspective =
                    std::get_if<fastgltf::Camera::Perspective>(&source_camera.camera)) {
                camera.kind = ModelCameraKind::perspective;
                camera.aspect_ratio = perspective->aspectRatio.has_value()
                                          ? static_cast<float>(*perspective->aspectRatio)
                                          : 0.0F;
                camera.vertical_fov_radians = static_cast<float>(perspective->yfov);
                camera.near_plane = static_cast<float>(perspective->znear);
                camera.far_plane =
                    perspective->zfar.has_value() ? static_cast<float>(*perspective->zfar) : 0.0F;
            } else {
                const auto& orthographic =
                    std::get<fastgltf::Camera::Orthographic>(source_camera.camera);
                camera.kind = ModelCameraKind::orthographic;
                camera.x_magnification = static_cast<float>(orthographic.xmag);
                camera.y_magnification = static_cast<float>(orthographic.ymag);
                camera.near_plane = static_cast<float>(orthographic.znear);
                camera.far_plane = static_cast<float>(orthographic.zfar);
            }
            target.cameras.push_back(std::move(camera));
        }

        if (source_node.lightIndex.has_value()) {
            if (*source_node.lightIndex >= source.lights.size() ||
                target.lights.size() >= limits.maximum_lights) {
                return core::Status::failure(
                    "gltf_import.light_out_of_bounds",
                    "glTF node light reference is missing or exceeds its configured limit");
            }
            const auto& source_light = source.lights[*source_node.lightIndex];
            auto name = model_name(source_light.name, "light", target.lights.size(), limits);
            if (!name) {
                return core::Status::failure(name.error().code, name.error().message);
            }
            ModelLight light;
            light.name = std::move(name).value();
            light.node = static_cast<std::uint32_t>(node_index);
            switch (source_light.type) {
            case fastgltf::LightType::Directional:
                light.kind = ModelLightKind::directional;
                break;
            case fastgltf::LightType::Point:
                light.kind = ModelLightKind::point;
                break;
            case fastgltf::LightType::Spot:
                light.kind = ModelLightKind::spot;
                break;
            }
            light.color = {
                static_cast<float>(source_light.color[0]),
                static_cast<float>(source_light.color[1]),
                static_cast<float>(source_light.color[2]),
            };
            light.intensity = static_cast<float>(source_light.intensity);
            light.range =
                source_light.range.has_value() ? static_cast<float>(*source_light.range) : 0.0F;
            light.inner_cone_radians = source_light.innerConeAngle.has_value()
                                           ? static_cast<float>(*source_light.innerConeAngle)
                                           : 0.0F;
            light.outer_cone_radians = source_light.outerConeAngle.has_value()
                                           ? static_cast<float>(*source_light.outerConeAngle)
                                           : 0.785398F;
            target.lights.push_back(std::move(light));
        }
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status import_skins(const fastgltf::Asset& source, ModelAsset& target,
                                        const ModelAssetLimits& limits) {
    if (source.skins.size() > limits.maximum_skins) {
        return core::Status::failure("gltf_import.skin_limit",
                                     "glTF skin count exceeds its configured limit");
    }
    target.skins.resize(source.skins.size());
    for (std::size_t index = 0; index < source.skins.size(); ++index) {
        const auto& source_skin = source.skins[index];
        auto& target_skin = target.skins[index];
        auto name = model_name(source_skin.name, "skin", index, limits);
        if (!name) {
            return core::Status::failure(name.error().code, name.error().message);
        }
        if (source_skin.joints.empty() ||
            source_skin.joints.size() > limits.maximum_joints_per_skin) {
            return core::Status::failure("gltf_import.joint_limit",
                                         "glTF skin joint count is empty or exceeds its limit");
        }
        target_skin.name = std::move(name).value();
        if (source_skin.skeleton.has_value()) {
            if (*source_skin.skeleton >= source.nodes.size()) {
                return core::Status::failure("gltf_import.skeleton_out_of_bounds",
                                             "glTF skin skeleton references a missing node");
            }
            target_skin.skeleton_root = static_cast<std::uint32_t>(*source_skin.skeleton);
        }
        target_skin.joints.reserve(source_skin.joints.size());
        for (const auto joint : source_skin.joints) {
            if (joint >= source.nodes.size()) {
                return core::Status::failure("gltf_import.joint_out_of_bounds",
                                             "glTF skin references a missing joint node");
            }
            target_skin.joints.push_back(static_cast<std::uint32_t>(joint));
        }
        target_skin.inverse_bind_matrices.assign(source_skin.joints.size(),
                                                 math::Mat4f::identity());
        if (source_skin.inverseBindMatrices.has_value()) {
            const auto accessor_index = *source_skin.inverseBindMatrices;
            if (accessor_index >= source.accessors.size()) {
                return core::Status::failure(
                    "gltf_import.bind_accessor_out_of_bounds",
                    "glTF skin references a missing inverse-bind accessor");
            }
            const auto& accessor = source.accessors[accessor_index];
            if (accessor.count != source_skin.joints.size() ||
                accessor.type != fastgltf::AccessorType::Mat4 ||
                accessor.componentType != fastgltf::ComponentType::Float) {
                return core::Status::failure(
                    "gltf_import.invalid_bind_accessor",
                    "glTF inverse-bind accessor must be one float MAT4 per joint");
            }
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fmat4x4>(
                source, accessor, [&](const auto& matrix, std::size_t matrix_index) {
                    target_skin.inverse_bind_matrices[matrix_index] = model_matrix(matrix);
                });
        }
    }
    return core::Status::ok();
}

[[nodiscard]] core::Result<std::span<const std::uint8_t>>
image_source_bytes(const fastgltf::Asset& source, std::size_t image_index) {
    if (image_index >= source.images.size()) {
        return core::Result<std::span<const std::uint8_t>>::failure(
            "gltf_import.image_out_of_bounds", "glTF texture references a missing image");
    }
    return std::visit(
        [&](const auto& data) -> core::Result<std::span<const std::uint8_t>> {
            using Source = std::remove_cvref_t<decltype(data)>;
            if constexpr (std::is_same_v<Source, fastgltf::sources::BufferView>) {
                if (data.bufferViewIndex >= source.bufferViews.size()) {
                    return core::Result<std::span<const std::uint8_t>>::failure(
                        "gltf_import.image_buffer_out_of_bounds",
                        "glTF image references a missing buffer view");
                }
                const auto bytes =
                    fastgltf::DefaultBufferDataAdapter{}(source, data.bufferViewIndex);
                return core::Result<std::span<const std::uint8_t>>::success(
                    {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
            } else if constexpr (std::is_same_v<Source, fastgltf::sources::Array>) {
                return core::Result<std::span<const std::uint8_t>>::success(
                    {reinterpret_cast<const std::uint8_t*>(data.bytes.data()), data.bytes.size()});
            } else if constexpr (std::is_same_v<Source, fastgltf::sources::Vector>) {
                return core::Result<std::span<const std::uint8_t>>::success(
                    {reinterpret_cast<const std::uint8_t*>(data.bytes.data()), data.bytes.size()});
            } else if constexpr (std::is_same_v<Source, fastgltf::sources::ByteView>) {
                return core::Result<std::span<const std::uint8_t>>::success(
                    {reinterpret_cast<const std::uint8_t*>(data.bytes.data()), data.bytes.size()});
            } else {
                return core::Result<std::span<const std::uint8_t>>::failure(
                    "gltf_import.image_not_loaded",
                    "glTF image source was not loaded into the model cooker");
            }
        },
        source.images[image_index].data);
}

[[nodiscard]] core::Result<std::uint32_t>
import_model_image(const fastgltf::Asset& source, std::size_t image_index, bool ktx2,
                   ModelAsset& target,
                   std::unordered_map<std::size_t, std::uint32_t>& imported_images,
                   const ModelAssetLimits& limits, std::size_t& total_image_bytes) {
    const auto existing = imported_images.find(image_index);
    if (existing != imported_images.end()) {
        return core::Result<std::uint32_t>::success(existing->second);
    }
    if (target.images.size() >= limits.maximum_images) {
        return core::Result<std::uint32_t>::failure(
            "gltf_import.image_limit", "glTF referenced image count exceeds its configured limit");
    }
    auto bytes = image_source_bytes(source, image_index);
    if (!bytes) {
        return core::Result<std::uint32_t>::failure(bytes.error().code, bytes.error().message);
    }
    ImageAssetLimits image_limits;
    image_limits.maximum_dimension = limits.maximum_image_dimension;
    image_limits.maximum_decoded_bytes = limits.maximum_decoded_image_bytes - total_image_bytes;
    auto decoded = ktx2 ? decode_ktx2(bytes.value(), image_limits, true)
                        : decode_png_or_jpeg(bytes.value(), image_limits);
    if (!decoded) {
        return core::Result<std::uint32_t>::failure(
            decoded.error().code, "glTF image could not be decoded: " + decoded.error().message);
    }
    auto name = model_name(source.images[image_index].name, "image", image_index, limits);
    if (!name) {
        return core::Result<std::uint32_t>::failure(name.error().code, name.error().message);
    }
    const auto runtime_index = static_cast<std::uint32_t>(target.images.size());
    total_image_bytes += decoded.value().rgba8.size();
    target.images.push_back({std::move(name).value(), decoded.value().width, decoded.value().height,
                             std::move(decoded).value().rgba8});
    imported_images.emplace(image_index, runtime_index);
    return core::Result<std::uint32_t>::success(runtime_index);
}

[[nodiscard]] ModelTextureMagFilter model_mag_filter(fastgltf::Filter filter) {
    return filter == fastgltf::Filter::Nearest ? ModelTextureMagFilter::nearest
                                               : ModelTextureMagFilter::linear;
}

[[nodiscard]] ModelTextureMinFilter model_min_filter(fastgltf::Filter filter) {
    switch (filter) {
    case fastgltf::Filter::Nearest:
        return ModelTextureMinFilter::nearest;
    case fastgltf::Filter::Linear:
        return ModelTextureMinFilter::linear;
    case fastgltf::Filter::NearestMipMapNearest:
        return ModelTextureMinFilter::nearest_mipmap_nearest;
    case fastgltf::Filter::LinearMipMapNearest:
        return ModelTextureMinFilter::linear_mipmap_nearest;
    case fastgltf::Filter::NearestMipMapLinear:
        return ModelTextureMinFilter::nearest_mipmap_linear;
    case fastgltf::Filter::LinearMipMapLinear:
        return ModelTextureMinFilter::linear_mipmap_linear;
    }
    return ModelTextureMinFilter::linear;
}

[[nodiscard]] ModelTextureWrap model_wrap(fastgltf::Wrap wrap) {
    switch (wrap) {
    case fastgltf::Wrap::ClampToEdge:
        return ModelTextureWrap::clamp_to_edge;
    case fastgltf::Wrap::MirroredRepeat:
        return ModelTextureWrap::mirrored_repeat;
    case fastgltf::Wrap::Repeat:
        return ModelTextureWrap::repeat;
    }
    return ModelTextureWrap::repeat;
}

[[nodiscard]] core::Status import_samplers(const fastgltf::Asset& source, ModelAsset& target,
                                           const ModelAssetLimits& limits) {
    if (source.samplers.size() > limits.maximum_samplers) {
        return core::Status::failure("gltf_import.sampler_limit",
                                     "glTF sampler count exceeds its configured limit");
    }
    target.samplers.reserve(source.samplers.size());
    for (const auto& source_sampler : source.samplers) {
        ModelSampler sampler;
        sampler.mag_filter =
            model_mag_filter(source_sampler.magFilter.value_or(fastgltf::Filter::Linear));
        sampler.min_filter =
            model_min_filter(source_sampler.minFilter.value_or(fastgltf::Filter::Linear));
        sampler.wrap_s = model_wrap(source_sampler.wrapS);
        sampler.wrap_t = model_wrap(source_sampler.wrapT);
        target.samplers.push_back(sampler);
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status
import_texture_binding(const fastgltf::Asset& source, const fastgltf::TextureInfo& texture_info,
                       ModelTextureBinding& binding, ModelAsset& target,
                       std::unordered_map<std::size_t, std::uint32_t>& imported_images,
                       const ModelAssetLimits& limits, std::size_t& total_image_bytes) {
    if (texture_info.textureIndex >= source.textures.size()) {
        return core::Status::failure("gltf_import.texture_out_of_bounds",
                                     "glTF material references a missing texture");
    }
    const auto& texture = source.textures[texture_info.textureIndex];
    const auto ktx2 = texture.basisuImageIndex.has_value();
    const auto image_index =
        ktx2 ? *texture.basisuImageIndex
             : texture.imageIndex.value_or(std::numeric_limits<std::size_t>::max());
    if (image_index >= source.images.size()) {
        return core::Status::failure(
            "gltf_import.texture_source_out_of_bounds",
            "glTF texture does not resolve to a loaded PNG, JPEG, or KTX2 image");
    }
    if (texture.samplerIndex.has_value()) {
        if (*texture.samplerIndex >= target.samplers.size()) {
            return core::Status::failure("gltf_import.sampler_out_of_bounds",
                                         "glTF texture references a missing sampler");
        }
        binding.sampler = static_cast<std::uint32_t>(*texture.samplerIndex);
    }
    auto texcoord = texture_info.texCoordIndex;
    if (texture_info.transform != nullptr) {
        const auto& transform = *texture_info.transform;
        if (transform.texCoordIndex.has_value()) {
            texcoord = *transform.texCoordIndex;
        }
        binding.offset = {static_cast<float>(transform.uvOffset[0]),
                          static_cast<float>(transform.uvOffset[1])};
        binding.scale = {static_cast<float>(transform.uvScale[0]),
                         static_cast<float>(transform.uvScale[1])};
        binding.rotation = static_cast<float>(transform.rotation);
    }
    if (texcoord > 1U) {
        return core::Status::failure("gltf_import.unsupported_texture_coordinates",
                                     "runtime model textures support TEXCOORD_0 and TEXCOORD_1");
    }
    binding.texcoord = static_cast<std::uint8_t>(texcoord);
    auto image = import_model_image(source, image_index, ktx2, target, imported_images, limits,
                                    total_image_bytes);
    if (!image) {
        return core::Status::failure(image.error().code, image.error().message);
    }
    binding.image = image.value();
    return core::Status::ok();
}

[[nodiscard]] core::Status import_materials(const fastgltf::Asset& source, ModelAsset& target,
                                            const ModelAssetLimits& limits) {
    const auto needs_default_material =
        std::ranges::any_of(source.meshes, [](const fastgltf::Mesh& mesh) {
            return std::ranges::any_of(mesh.primitives, [](const fastgltf::Primitive& primitive) {
                return !primitive.materialIndex.has_value();
            });
        });
    if (source.materials.size() > limits.maximum_materials ||
        (needs_default_material && source.materials.size() == limits.maximum_materials)) {
        return core::Status::failure("gltf_import.material_limit",
                                     "glTF material count exceeds its configured limit");
    }
    target.materials.reserve(source.materials.size());
    std::unordered_map<std::size_t, std::uint32_t> imported_images;
    std::size_t total_image_bytes = 0;
    for (std::size_t index = 0; index < source.materials.size(); ++index) {
        const auto& source_material = source.materials[index];
        auto name = model_name(source_material.name, "material", index, limits);
        if (!name) {
            return core::Status::failure(name.error().code, name.error().message);
        }
        ModelMaterial material;
        material.name = std::move(name).value();
        for (std::size_t component = 0; component < material.base_color_factor.size();
             ++component) {
            material.base_color_factor[component] =
                static_cast<float>(source_material.pbrData.baseColorFactor[component]);
        }
        material.alpha_mode =
            source_material.alphaMode == fastgltf::AlphaMode::Mask    ? ModelAlphaMode::mask
            : source_material.alphaMode == fastgltf::AlphaMode::Blend ? ModelAlphaMode::blend
                                                                      : ModelAlphaMode::opaque;
        material.alpha_cutoff = static_cast<float>(source_material.alphaCutoff);
        material.double_sided = source_material.doubleSided;
        material.unlit = source_material.unlit;
        material.metallic_factor = static_cast<float>(source_material.pbrData.metallicFactor);
        material.roughness_factor = static_cast<float>(source_material.pbrData.roughnessFactor);
        material.normal_scale = source_material.normalTexture.has_value()
                                    ? static_cast<float>(source_material.normalTexture->scale)
                                    : 1.0F;
        material.occlusion_strength =
            source_material.occlusionTexture.has_value()
                ? static_cast<float>(source_material.occlusionTexture->strength)
                : 1.0F;
        for (std::size_t component = 0; component < material.emissive_factor.size(); ++component) {
            material.emissive_factor[component] =
                static_cast<float>(source_material.emissiveFactor[component]);
        }

        const auto import_optional = [&](const auto& texture,
                                         ModelTextureBinding& binding) -> core::Status {
            if (!texture.has_value()) {
                return core::Status::ok();
            }
            return import_texture_binding(source, *texture, binding, target, imported_images,
                                          limits, total_image_bytes);
        };
        auto status =
            import_optional(source_material.pbrData.baseColorTexture, material.base_color_texture);
        if (!status) {
            return status;
        }
        status = import_optional(source_material.pbrData.metallicRoughnessTexture,
                                 material.metallic_roughness_texture);
        if (!status) {
            return status;
        }
        status = import_optional(source_material.normalTexture, material.normal_texture);
        if (!status) {
            return status;
        }
        status = import_optional(source_material.occlusionTexture, material.occlusion_texture);
        if (!status) {
            return status;
        }
        status = import_optional(source_material.emissiveTexture, material.emissive_texture);
        if (!status) {
            return status;
        }
        target.materials.push_back(std::move(material));
    }
    if (needs_default_material) {
        ModelMaterial material;
        material.name = "default_material";
        target.materials.push_back(std::move(material));
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status copy_positions(const fastgltf::Asset& source,
                                          const fastgltf::Primitive& primitive,
                                          std::vector<ModelVertex>& vertices) {
    const auto attribute = primitive.findAttribute("POSITION");
    if (attribute == primitive.attributes.end() ||
        attribute->accessorIndex >= source.accessors.size()) {
        return core::Status::failure("gltf_import.missing_positions",
                                     "glTF mesh primitive must provide POSITION");
    }
    const auto& accessor = source.accessors[attribute->accessorIndex];
    if (accessor.type != fastgltf::AccessorType::Vec3 || accessor.count == 0) {
        return core::Status::failure("gltf_import.invalid_positions",
                                     "glTF POSITION must be a non-empty VEC3 accessor");
    }
    vertices.resize(accessor.count);
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
        source, accessor, [&](const auto& value, std::size_t index) {
            vertices[index].position = {value[0], value[1], value[2]};
        });
    return core::Status::ok();
}

template <typename Element, typename Assign>
[[nodiscard]] core::Status
copy_optional_attribute(const fastgltf::Asset& source, const fastgltf::Primitive& primitive,
                        std::string_view name, fastgltf::AccessorType expected_type,
                        std::size_t expected_count, Assign assign) {
    const auto attribute = primitive.findAttribute(name);
    if (attribute == primitive.attributes.end()) {
        return core::Status::ok();
    }
    if (attribute->accessorIndex >= source.accessors.size()) {
        return core::Status::failure("gltf_import.attribute_out_of_bounds",
                                     "glTF mesh attribute references a missing accessor");
    }
    const auto& accessor = source.accessors[attribute->accessorIndex];
    if (accessor.type != expected_type || accessor.count != expected_count) {
        return core::Status::failure("gltf_import.attribute_shape_mismatch",
                                     "glTF mesh attribute type/count does not match POSITION");
    }
    fastgltf::iterateAccessorWithIndex<Element>(source, accessor, std::move(assign));
    return core::Status::ok();
}

[[nodiscard]] core::Status import_vertex_attributes(const fastgltf::Asset& source,
                                                    const fastgltf::Primitive& primitive,
                                                    const ModelSkin* skin,
                                                    std::vector<ModelVertex>& vertices) {
    auto status = copy_positions(source, primitive, vertices);
    if (!status) {
        return status;
    }
    status = copy_optional_attribute<fastgltf::math::fvec3>(
        source, primitive, "NORMAL", fastgltf::AccessorType::Vec3, vertices.size(),
        [&](const auto& value, std::size_t index) {
            vertices[index].normal = {value[0], value[1], value[2]};
        });
    if (!status) {
        return status;
    }
    status = copy_optional_attribute<fastgltf::math::fvec2>(
        source, primitive, "TEXCOORD_0", fastgltf::AccessorType::Vec2, vertices.size(),
        [&](const auto& value, std::size_t index) { vertices[index].uv0 = {value[0], value[1]}; });
    if (!status) {
        return status;
    }
    status = copy_optional_attribute<fastgltf::math::fvec2>(
        source, primitive, "TEXCOORD_1", fastgltf::AccessorType::Vec2, vertices.size(),
        [&](const auto& value, std::size_t index) { vertices[index].uv1 = {value[0], value[1]}; });
    if (!status) {
        return status;
    }
    status = copy_optional_attribute<fastgltf::math::fvec4>(
        source, primitive, "TANGENT", fastgltf::AccessorType::Vec4, vertices.size(),
        [&](const auto& value, std::size_t index) {
            vertices[index].tangent = {value[0], value[1], value[2], value[3]};
        });
    if (!status) {
        return status;
    }
    const auto color = primitive.findAttribute("COLOR_0");
    if (color != primitive.attributes.end()) {
        if (color->accessorIndex >= source.accessors.size()) {
            return core::Status::failure("gltf_import.attribute_out_of_bounds",
                                         "glTF COLOR_0 references a missing accessor");
        }
        const auto& accessor = source.accessors[color->accessorIndex];
        if (accessor.count != vertices.size() || (accessor.type != fastgltf::AccessorType::Vec3 &&
                                                  accessor.type != fastgltf::AccessorType::Vec4)) {
            return core::Status::failure("gltf_import.attribute_shape_mismatch",
                                         "glTF COLOR_0 must be a VEC3 or VEC4 matching POSITION");
        }
        if (accessor.type == fastgltf::AccessorType::Vec3) {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                source, accessor, [&](const auto& value, std::size_t index) {
                    vertices[index].color = {value[0], value[1], value[2], 1.0F};
                });
        } else {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
                source, accessor, [&](const auto& value, std::size_t index) {
                    vertices[index].color = {value[0], value[1], value[2], value[3]};
                });
        }
    }

    const auto joints = primitive.findAttribute("JOINTS_0");
    const auto weights = primitive.findAttribute("WEIGHTS_0");
    const auto joints_1 = primitive.findAttribute("JOINTS_1");
    const auto weights_1 = primitive.findAttribute("WEIGHTS_1");
    if ((joints == primitive.attributes.end()) != (weights == primitive.attributes.end()) ||
        (joints_1 == primitive.attributes.end()) != (weights_1 == primitive.attributes.end()) ||
        (joints_1 != primitive.attributes.end() && joints == primitive.attributes.end()) ||
        (skin != nullptr && joints == primitive.attributes.end())) {
        return core::Status::failure(
            "gltf_import.missing_skin_attributes",
            "skinned glTF primitive requires matching JOINTS_0 and WEIGHTS_0 attributes");
    }
    if (joints == primitive.attributes.end()) {
        return core::Status::ok();
    }
    if (skin == nullptr || joints->accessorIndex >= source.accessors.size() ||
        weights->accessorIndex >= source.accessors.size() ||
        (joints_1 != primitive.attributes.end() &&
         (joints_1->accessorIndex >= source.accessors.size() ||
          weights_1->accessorIndex >= source.accessors.size()))) {
        return core::Status::failure("gltf_import.invalid_skin_binding",
                                     "glTF joint attributes require a valid skin and accessors");
    }
    const auto& joint_accessor = source.accessors[joints->accessorIndex];
    const auto& weight_accessor = source.accessors[weights->accessorIndex];
    if (joint_accessor.type != fastgltf::AccessorType::Vec4 ||
        weight_accessor.type != fastgltf::AccessorType::Vec4 ||
        joint_accessor.count != vertices.size() || weight_accessor.count != vertices.size()) {
        return core::Status::failure("gltf_import.invalid_skin_attributes",
                                     "glTF skin attributes must be VEC4 per vertex");
    }
    std::vector<std::array<std::uint16_t, 8>> imported_joints(vertices.size());
    std::vector<std::array<float, 8>> imported_weights(vertices.size());
    const auto copy_joint_set = [&](const fastgltf::Accessor& accessor, std::size_t offset) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::u16vec4>(
            source, accessor, [&](const auto& value, std::size_t vertex_index) {
                for (std::size_t component = 0; component < 4; ++component) {
                    imported_joints[vertex_index][offset + component] = value[component];
                }
            });
    };
    const auto copy_weight_set = [&](const fastgltf::Accessor& accessor, std::size_t offset) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
            source, accessor, [&](const auto& value, std::size_t vertex_index) {
                for (std::size_t component = 0; component < 4; ++component) {
                    imported_weights[vertex_index][offset + component] = value[component];
                }
            });
    };
    copy_joint_set(joint_accessor, 0);
    copy_weight_set(weight_accessor, 0);
    if (joints_1 != primitive.attributes.end()) {
        const auto& joint_accessor_1 = source.accessors[joints_1->accessorIndex];
        const auto& weight_accessor_1 = source.accessors[weights_1->accessorIndex];
        if (joint_accessor_1.type != fastgltf::AccessorType::Vec4 ||
            weight_accessor_1.type != fastgltf::AccessorType::Vec4 ||
            joint_accessor_1.count != vertices.size() ||
            weight_accessor_1.count != vertices.size()) {
            return core::Status::failure("gltf_import.invalid_skin_attributes",
                                         "glTF JOINTS_1 and WEIGHTS_1 must be VEC4 per vertex");
        }
        copy_joint_set(joint_accessor_1, 4);
        copy_weight_set(weight_accessor_1, 4);
    }
    for (std::size_t vertex_index = 0; vertex_index < vertices.size(); ++vertex_index) {
        std::array<std::pair<float, std::uint16_t>, 8> influences;
        for (std::size_t influence = 0; influence < influences.size(); ++influence) {
            influences[influence] = {imported_weights[vertex_index][influence],
                                     imported_joints[vertex_index][influence]};
        }
        std::ranges::sort(influences, std::greater{}, &decltype(influences)::value_type::first);
        float sum = 0.0F;
        for (std::size_t influence = 0; influence < 4; ++influence) {
            vertices[vertex_index].weights[influence] = influences[influence].first;
            vertices[vertex_index].joints[influence] = influences[influence].second;
            sum += influences[influence].first;
        }
        if (!std::isfinite(sum) || sum <= 0.0F) {
            return core::Status::failure(
                "gltf_import.invalid_skin_weights",
                "glTF skin weights must contain a finite non-zero influence per vertex");
        }
        for (auto& weight : vertices[vertex_index].weights) {
            weight /= sum;
        }
    }
    for (const auto& vertex : vertices) {
        for (std::size_t component = 0; component < 4; ++component) {
            if (vertex.weights[component] > 0.0F &&
                vertex.joints[component] >= skin->joints.size()) {
                return core::Status::failure("gltf_import.joint_out_of_bounds",
                                             "glTF vertex references a missing skin joint");
            }
        }
    }
    return core::Status::ok();
}

[[nodiscard]] core::Result<std::vector<ModelMorphTarget>>
import_morph_targets(const fastgltf::Asset& source, const fastgltf::Primitive& primitive,
                     std::size_t vertex_count, const ModelAssetLimits& limits,
                     std::size_t& total_delta_values) {
    if (primitive.targets.size() > limits.maximum_morph_targets_per_primitive) {
        return core::Result<std::vector<ModelMorphTarget>>::failure(
            "gltf_import.morph_limit", "glTF morph target count exceeds its configured limit");
    }
    std::vector<ModelMorphTarget> targets(primitive.targets.size());
    for (std::size_t target_index = 0; target_index < primitive.targets.size(); ++target_index) {
        const auto& source_target = primitive.targets[target_index];
        auto& target = targets[target_index];
        const auto import_attribute = [&](std::string_view semantic,
                                          std::vector<math::Vec3f>& deltas) -> core::Status {
            const auto attribute =
                std::ranges::find(source_target, semantic, &fastgltf::Attribute::name);
            if (attribute == source_target.end()) {
                return core::Status::ok();
            }
            if (attribute->accessorIndex >= source.accessors.size()) {
                return core::Status::failure("gltf_import.morph_accessor_out_of_bounds",
                                             "glTF morph target references a missing accessor");
            }
            const auto& accessor = source.accessors[attribute->accessorIndex];
            if (accessor.type != fastgltf::AccessorType::Vec3 || accessor.count != vertex_count ||
                vertex_count > limits.maximum_morph_delta_values - total_delta_values) {
                return core::Status::failure("gltf_import.invalid_morph_accessor",
                                             "glTF morph attributes must be VEC3 arrays matching "
                                             "POSITION and within limits");
            }
            deltas.resize(vertex_count);
            total_delta_values += vertex_count;
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                source, accessor, [&](const auto& value, std::size_t vertex) {
                    deltas[vertex] = {value[0], value[1], value[2]};
                });
            return core::Status::ok();
        };
        auto status = import_attribute("POSITION", target.position_deltas);
        if (!status) {
            return core::Result<std::vector<ModelMorphTarget>>::failure(status.error().code,
                                                                        status.error().message);
        }
        status = import_attribute("NORMAL", target.normal_deltas);
        if (!status) {
            return core::Result<std::vector<ModelMorphTarget>>::failure(status.error().code,
                                                                        status.error().message);
        }
        status = import_attribute("TANGENT", target.tangent_deltas);
        if (!status) {
            return core::Result<std::vector<ModelMorphTarget>>::failure(status.error().code,
                                                                        status.error().message);
        }
    }
    return core::Result<std::vector<ModelMorphTarget>>::success(std::move(targets));
}

void generate_tangents(std::vector<ModelVertex>& vertices,
                       std::span<const std::uint32_t> global_indices, std::uint32_t first_vertex,
                       bool has_uvs) {
    std::vector<math::Vec3f> tangent_accumulator(vertices.size());
    std::vector<math::Vec3f> bitangent_accumulator(vertices.size());
    if (has_uvs) {
        for (std::size_t triangle = 0; triangle + 2U < global_indices.size(); triangle += 3U) {
            const auto i0 = global_indices[triangle] - first_vertex;
            const auto i1 = global_indices[triangle + 1U] - first_vertex;
            const auto i2 = global_indices[triangle + 2U] - first_vertex;
            const auto edge1 = vertices[i1].position - vertices[i0].position;
            const auto edge2 = vertices[i2].position - vertices[i0].position;
            const math::Vec2f uv1{vertices[i1].uv0.x - vertices[i0].uv0.x,
                                  vertices[i1].uv0.y - vertices[i0].uv0.y};
            const math::Vec2f uv2{vertices[i2].uv0.x - vertices[i0].uv0.x,
                                  vertices[i2].uv0.y - vertices[i0].uv0.y};
            const auto determinant = uv1.x * uv2.y - uv1.y * uv2.x;
            if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12F) {
                continue;
            }
            const auto inverse = 1.0F / determinant;
            const auto tangent = (edge1 * uv2.y - edge2 * uv1.y) * inverse;
            const auto bitangent = (edge2 * uv1.x - edge1 * uv2.x) * inverse;
            for (const auto vertex : {i0, i1, i2}) {
                tangent_accumulator[vertex] = tangent_accumulator[vertex] + tangent;
                bitangent_accumulator[vertex] = bitangent_accumulator[vertex] + bitangent;
            }
        }
    }
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const auto normal = vertices[index].normal;
        auto tangent =
            tangent_accumulator[index] - normal * math::dot(normal, tangent_accumulator[index]);
        auto tangent_length = std::sqrt(math::length_squared(tangent));
        if (!std::isfinite(tangent_length) || tangent_length <= 1.0e-8F) {
            const auto axis = std::abs(normal.y) < 0.999F ? math::Vec3f{0.0F, 1.0F, 0.0F}
                                                          : math::Vec3f{1.0F, 0.0F, 0.0F};
            tangent = math::cross(axis, normal);
            tangent_length = std::sqrt(math::length_squared(tangent));
        }
        tangent = tangent * (1.0F / tangent_length);
        const auto handedness =
            math::dot(math::cross(normal, tangent), bitangent_accumulator[index]) < 0.0F ? -1.0F
                                                                                         : 1.0F;
        vertices[index].tangent = {tangent.x, tangent.y, tangent.z, handedness};
    }
}

[[nodiscard]] core::Status import_geometry(const fastgltf::Asset& source, ModelAsset& target,
                                           const ModelAssetLimits& limits) {
    bool has_bounds = false;
    bool has_renderable_geometry = false;
    std::size_t morph_delta_values = 0;
    for (std::size_t node_index = 0; node_index < source.nodes.size(); ++node_index) {
        const auto& node = source.nodes[node_index];
        if (!node.meshIndex.has_value()) {
            continue;
        }
        if (*node.meshIndex >= source.meshes.size()) {
            return core::Status::failure("gltf_import.mesh_out_of_bounds",
                                         "glTF node references a missing mesh");
        }
        const ModelSkin* skin = nullptr;
        auto skin_index = no_model_index;
        if (node.skinIndex.has_value()) {
            if (*node.skinIndex >= target.skins.size()) {
                return core::Status::failure("gltf_import.skin_out_of_bounds",
                                             "glTF node references a missing skin");
            }
            skin_index = static_cast<std::uint32_t>(*node.skinIndex);
            skin = &target.skins[skin_index];
        }
        const auto& mesh = source.meshes[*node.meshIndex];
        const auto collision_source = is_collision_node(node.name);
        auto lod_level = node_lod_level(node.name, limits);
        if (!lod_level) {
            return core::Status::failure(lod_level.error().code, lod_level.error().message);
        }
        for (std::size_t primitive_index = 0; primitive_index < mesh.primitives.size();
             ++primitive_index) {
            const auto& primitive = mesh.primitives[primitive_index];
            if (primitive.type != fastgltf::PrimitiveType::Triangles) {
                return core::Status::failure(
                    "gltf_import.unsupported_topology",
                    "runtime model cooker accepts triangle-list glTF primitives only");
            }
            auto status = check_append_count(target.primitives.size(), 1, limits.maximum_primitives,
                                             "primitive");
            if (!status) {
                return status;
            }
            std::vector<ModelVertex> vertices;
            status = import_vertex_attributes(source, primitive, skin, vertices);
            if (!status) {
                return status;
            }
            status = check_append_count(target.vertices.size(), vertices.size(),
                                        limits.maximum_vertices, "vertex");
            if (!status) {
                return status;
            }
            auto morph_targets = import_morph_targets(source, primitive, vertices.size(), limits,
                                                      morph_delta_values);
            if (!morph_targets) {
                return core::Status::failure(morph_targets.error().code,
                                             morph_targets.error().message);
            }
            auto& node_weights = target.nodes[node_index].morph_weights;
            if (node_weights.empty() && !morph_targets.value().empty()) {
                node_weights.resize(morph_targets.value().size(), 0.0F);
            }
            if (node_weights.size() != morph_targets.value().size()) {
                return core::Status::failure(
                    "gltf_import.morph_weight_mismatch",
                    "glTF node or mesh morph weights must match every primitive target count");
            }
            if (!primitive.indicesAccessor.has_value() ||
                *primitive.indicesAccessor >= source.accessors.size()) {
                return core::Status::failure("gltf_import.missing_indices",
                                             "glTF primitive must resolve an index accessor");
            }
            const auto& index_accessor = source.accessors[*primitive.indicesAccessor];
            if (index_accessor.type != fastgltf::AccessorType::Scalar ||
                index_accessor.count == 0 || index_accessor.count % 3U != 0) {
                return core::Status::failure(
                    "gltf_import.invalid_indices",
                    "glTF triangle primitive indices must be a non-empty SCALAR multiple of three");
            }
            status = check_append_count(target.indices.size(), index_accessor.count,
                                        limits.maximum_indices, "index");
            if (!status) {
                return status;
            }
            const auto first_vertex = static_cast<std::uint32_t>(target.vertices.size());
            const auto first_index = static_cast<std::uint32_t>(target.indices.size());
            target.vertices.insert(target.vertices.end(), vertices.begin(), vertices.end());
            target.indices.resize(target.indices.size() + index_accessor.count);
            std::vector<std::uint32_t> local_indices(index_accessor.count);
            bool bad_index = false;
            fastgltf::iterateAccessorWithIndex<std::uint32_t>(
                source, index_accessor, [&](std::uint32_t value, std::size_t index) {
                    if (value >= vertices.size() ||
                        value > std::numeric_limits<std::uint32_t>::max() - first_vertex) {
                        bad_index = true;
                    } else {
                        local_indices[index] = value;
                        target.indices[first_index + index] = first_vertex + value;
                    }
                });
            if (bad_index) {
                return core::Status::failure("gltf_import.index_out_of_bounds",
                                             "glTF index references a missing primitive vertex");
            }
            if (primitive.findAttribute("TANGENT") == primitive.attributes.end()) {
                generate_tangents(
                    vertices,
                    std::span<const std::uint32_t>{target.indices}.subspan(first_index,
                                                                           index_accessor.count),
                    first_vertex,
                    primitive.findAttribute("TEXCOORD_0") != primitive.attributes.end());
                std::ranges::copy(vertices, target.vertices.begin() + first_vertex);
            }
            std::vector<std::uint32_t> cache_optimized(local_indices.size());
            meshopt_optimizeVertexCache(cache_optimized.data(), local_indices.data(),
                                        local_indices.size(), vertices.size());
            std::vector<std::uint32_t> overdraw_optimized(local_indices.size());
            meshopt_optimizeOverdraw(overdraw_optimized.data(), cache_optimized.data(),
                                     cache_optimized.size(), &vertices.front().position.x,
                                     vertices.size(), sizeof(ModelVertex), 1.05F);
            for (std::size_t index = 0; index < overdraw_optimized.size(); ++index) {
                target.indices[first_index + index] = first_vertex + overdraw_optimized[index];
            }
            const auto primitive_label =
                mesh.name.empty() ? std::string{}
                                  : std::string(mesh.name) + "_" + std::to_string(primitive_index);
            auto name = model_name(primitive_label, "primitive", target.primitives.size(), limits);
            if (!name) {
                return core::Status::failure(name.error().code, name.error().message);
            }
            auto material_index = no_model_index;
            if (primitive.materialIndex.has_value()) {
                if (*primitive.materialIndex >= target.materials.size()) {
                    return core::Status::failure("gltf_import.material_out_of_bounds",
                                                 "glTF primitive references a missing material");
                }
                material_index = static_cast<std::uint32_t>(*primitive.materialIndex);
            } else if (target.materials.size() > source.materials.size()) {
                material_index = static_cast<std::uint32_t>(source.materials.size());
            }
            target.primitives.push_back({
                std::move(name).value(),
                first_vertex,
                static_cast<std::uint32_t>(vertices.size()),
                first_index,
                static_cast<std::uint32_t>(index_accessor.count),
                static_cast<std::uint32_t>(node_index),
                skin_index,
                material_index,
                std::move(morph_targets).value(),
                {},
                lod_level.value(),
                !collision_source,
                collision_source,
            });
            auto primitive_bounds =
                math::Bounds3f{vertices.front().position, vertices.front().position};
            for (const auto& vertex : vertices) {
                primitive_bounds.min = math::component_min(primitive_bounds.min, vertex.position);
                primitive_bounds.max = math::component_max(primitive_bounds.max, vertex.position);
            }
            auto& imported_primitive = target.primitives.back();
            for (std::size_t vertex_index = 0; vertex_index < vertices.size(); ++vertex_index) {
                auto maximum_delta = math::Vec3f{};
                for (const auto& morph_target : imported_primitive.morph_targets) {
                    if (!morph_target.position_deltas.empty()) {
                        const auto delta = morph_target.position_deltas[vertex_index];
                        maximum_delta.x += std::abs(delta.x);
                        maximum_delta.y += std::abs(delta.y);
                        maximum_delta.z += std::abs(delta.z);
                    }
                }
                primitive_bounds.min = math::component_min(
                    primitive_bounds.min, vertices[vertex_index].position - maximum_delta);
                primitive_bounds.max = math::component_max(
                    primitive_bounds.max, vertices[vertex_index].position + maximum_delta);
            }
            imported_primitive.bounds = primitive_bounds;
            if (collision_source) {
                if (target.collision_shapes.size() >= limits.maximum_collision_shapes) {
                    return core::Status::failure(
                        "gltf_import.collision_limit",
                        "glTF collision shape count exceeds its configured limit");
                }
                target.collision_shapes.push_back(
                    {target.nodes[node_index].name + "_" + std::to_string(primitive_index),
                     static_cast<std::uint32_t>(node_index), primitive_bounds});
            } else {
                has_renderable_geometry = true;
                if (!has_bounds) {
                    target.bounds = primitive_bounds;
                    has_bounds = true;
                } else {
                    target.bounds = target.bounds.merged_with(primitive_bounds);
                }
            }
        }
    }
    if (!has_renderable_geometry) {
        return core::Status::failure("gltf_import.no_geometry",
                                     "glTF runtime model contains no triangle geometry");
    }
    std::uint32_t maximum_lod = 0;
    for (const auto& primitive : target.primitives) {
        if (primitive.renderable) {
            maximum_lod = std::max(maximum_lod, primitive.lod_level);
        }
    }
    target.lods.resize(static_cast<std::size_t>(maximum_lod) + 1U);
    for (std::uint32_t level = 0; level <= maximum_lod; ++level) {
        auto& lod = target.lods[level];
        lod.level = level;
        lod.screen_coverage = std::exp2(-static_cast<float>(level));
    }
    for (std::uint32_t primitive_index = 0; primitive_index < target.primitives.size();
         ++primitive_index) {
        const auto& primitive = target.primitives[primitive_index];
        if (primitive.renderable) {
            target.lods[primitive.lod_level].primitives.push_back(primitive_index);
        }
    }
    if (std::ranges::any_of(target.lods,
                            [](const ModelLod& lod) { return lod.primitives.empty(); })) {
        return core::Status::failure(
            "gltf_import.non_contiguous_lods",
            "glTF LOD node suffixes must form a contiguous sequence beginning at LOD0");
    }
    return core::Status::ok();
}

[[nodiscard]] core::Result<ModelAnimationPath> animation_path(fastgltf::AnimationPath source) {
    switch (source) {
    case fastgltf::AnimationPath::Translation:
        return core::Result<ModelAnimationPath>::success(ModelAnimationPath::translation);
    case fastgltf::AnimationPath::Rotation:
        return core::Result<ModelAnimationPath>::success(ModelAnimationPath::rotation);
    case fastgltf::AnimationPath::Scale:
        return core::Result<ModelAnimationPath>::success(ModelAnimationPath::scale);
    case fastgltf::AnimationPath::Weights:
        return core::Result<ModelAnimationPath>::success(ModelAnimationPath::weights);
    }
    return core::Result<ModelAnimationPath>::failure("gltf_import.unknown_animation_path",
                                                     "glTF animation path is unknown");
}

[[nodiscard]] ModelAnimationInterpolation
animation_interpolation(fastgltf::AnimationInterpolation source) {
    switch (source) {
    case fastgltf::AnimationInterpolation::Step:
        return ModelAnimationInterpolation::step;
    case fastgltf::AnimationInterpolation::Linear:
        return ModelAnimationInterpolation::linear;
    case fastgltf::AnimationInterpolation::CubicSpline:
        return ModelAnimationInterpolation::cubic_spline;
    }
    return ModelAnimationInterpolation::linear;
}

[[nodiscard]] core::Status import_animations(const fastgltf::Asset& source, ModelAsset& target,
                                             const ModelAssetLimits& limits) {
    if (source.animations.size() > limits.maximum_animations) {
        return core::Status::failure("gltf_import.animation_limit",
                                     "glTF animation count exceeds its configured limit");
    }
    target.animations.reserve(source.animations.size());
    for (std::size_t animation_index = 0; animation_index < source.animations.size();
         ++animation_index) {
        const auto& source_animation = source.animations[animation_index];
        if (source_animation.channels.empty() ||
            source_animation.channels.size() > limits.maximum_channels_per_animation) {
            return core::Status::failure(
                "gltf_import.channel_limit",
                "glTF animation channel count is empty or exceeds its configured limit");
        }
        ModelAnimationClip clip;
        auto name = model_name(source_animation.name, "animation", animation_index, limits);
        if (!name) {
            return core::Status::failure(name.error().code, name.error().message);
        }
        clip.name = std::move(name).value();
        clip.channels.reserve(source_animation.channels.size());
        for (const auto& source_channel : source_animation.channels) {
            if (!source_channel.nodeIndex.has_value() ||
                *source_channel.nodeIndex >= source.nodes.size() ||
                source_channel.samplerIndex >= source_animation.samplers.size()) {
                return core::Status::failure(
                    "gltf_import.invalid_animation_channel",
                    "glTF animation channel references a missing node or sampler");
            }
            auto path = animation_path(source_channel.path);
            if (!path) {
                return core::Status::failure(path.error().code, path.error().message);
            }
            const auto& sampler = source_animation.samplers[source_channel.samplerIndex];
            if (sampler.inputAccessor >= source.accessors.size() ||
                sampler.outputAccessor >= source.accessors.size()) {
                return core::Status::failure(
                    "gltf_import.animation_accessor_out_of_bounds",
                    "glTF animation sampler references a missing accessor");
            }
            const auto& input = source.accessors[sampler.inputAccessor];
            const auto& output = source.accessors[sampler.outputAccessor];
            const auto interpolation = animation_interpolation(sampler.interpolation);
            const auto output_multiplier =
                interpolation == ModelAnimationInterpolation::cubic_spline ? 3U : 1U;
            const auto is_weights = path.value() == ModelAnimationPath::weights;
            const auto expected_type = path.value() == ModelAnimationPath::rotation
                                           ? fastgltf::AccessorType::Vec4
                                       : is_weights ? fastgltf::AccessorType::Scalar
                                                    : fastgltf::AccessorType::Vec3;
            const auto weight_count = target.nodes[*source_channel.nodeIndex].morph_weights.size();
            const auto expected_output_count =
                input.count * output_multiplier * (is_weights ? weight_count : 1U);
            if (input.type != fastgltf::AccessorType::Scalar ||
                input.componentType != fastgltf::ComponentType::Float || input.count == 0 ||
                input.count > limits.maximum_keyframes_per_channel ||
                (is_weights && weight_count == 0) || output.type != expected_type ||
                output.count != expected_output_count) {
                return core::Status::failure(
                    "gltf_import.invalid_animation_accessor",
                    "glTF animation accessors have invalid type or keyframe counts");
            }
            ModelAnimationChannel channel;
            channel.node = static_cast<std::uint32_t>(*source_channel.nodeIndex);
            channel.path = path.value();
            channel.interpolation = interpolation;
            channel.times.resize(input.count);
            fastgltf::iterateAccessorWithIndex<float>(
                source, input,
                [&](float value, std::size_t index) { channel.times[index] = value; });
            if (is_weights) {
                channel.weight_count = static_cast<std::uint32_t>(weight_count);
                channel.weight_values.resize(output.count);
                fastgltf::iterateAccessorWithIndex<float>(
                    source, output,
                    [&](float value, std::size_t index) { channel.weight_values[index] = value; });
            } else if (channel.path == ModelAnimationPath::rotation) {
                channel.values.resize(output.count);
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
                    source, output, [&](const auto& value, std::size_t index) {
                        math::Vec4f result{value[0], value[1], value[2], value[3]};
                        const auto is_cubic_value =
                            interpolation != ModelAnimationInterpolation::cubic_spline ||
                            index % 3U == 1U;
                        if (is_cubic_value) {
                            const auto magnitude =
                                std::sqrt(result.x * result.x + result.y * result.y +
                                          result.z * result.z + result.w * result.w);
                            if (std::isfinite(magnitude) && magnitude > 0.0F) {
                                result.x /= magnitude;
                                result.y /= magnitude;
                                result.z /= magnitude;
                                result.w /= magnitude;
                            }
                        }
                        channel.values[index] = result;
                    });
            } else {
                channel.values.resize(output.count);
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                    source, output, [&](const auto& value, std::size_t index) {
                        channel.values[index] = {value[0], value[1], value[2], 0.0F};
                    });
            }
            clip.duration_seconds = std::max(clip.duration_seconds, channel.times.back());
            clip.channels.push_back(std::move(channel));
        }
        target.animations.push_back(std::move(clip));
    }
    return core::Status::ok();
}

} // namespace

core::Result<std::vector<std::filesystem::path>>
discover_gltf_external_dependencies(const std::filesystem::path& path,
                                    const ModelAssetLimits& limits) {
    auto limit_status = limits.validate();
    if (!limit_status) {
        return core::Result<std::vector<std::filesystem::path>>::failure(
            limit_status.error().code, limit_status.error().message);
    }
    if (path.empty()) {
        return core::Result<std::vector<std::filesystem::path>>::failure(
            "gltf_import.missing_path", "glTF source path is required");
    }
    std::error_code file_error;
    const auto file_size = std::filesystem::file_size(path, file_error);
    if (file_error || file_size == 0 || file_size > limits.maximum_source_bytes) {
        return core::Result<std::vector<std::filesystem::path>>::failure(
            file_error ? "gltf_import.read_failed" : "gltf_import.source_limit",
            file_error ? file_error.message()
                       : "glTF source is empty or exceeds its configured byte limit");
    }

    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        auto failure = fastgltf_failure(data.error(), "failed to read glTF source");
        return core::Result<std::vector<std::filesystem::path>>::failure(failure.error().code,
                                                                         failure.error().message);
    }
    fastgltf::Parser parser(supported_gltf_extensions);
    auto parsed = parser.loadGltf(data.get(), path.parent_path(), fastgltf::Options::None);
    if (parsed.error() != fastgltf::Error::None) {
        auto failure = fastgltf_failure(parsed.error(), "failed to parse glTF dependencies");
        return core::Result<std::vector<std::filesystem::path>>::failure(failure.error().code,
                                                                         failure.error().message);
    }

    std::vector<std::filesystem::path> dependencies;
    const auto collect = [&](const fastgltf::DataSource& source) -> core::Status {
        const auto* uri_source = std::get_if<fastgltf::sources::URI>(&source);
        if (uri_source == nullptr || uri_source->uri.isDataUri()) {
            return core::Status::ok();
        }
        if (!uri_source->uri.valid() || !uri_source->uri.isLocalPath() ||
            !uri_source->uri.query().empty() || !uri_source->uri.fragment().empty()) {
            return core::Status::failure(
                "gltf_import.external_uri_unsupported",
                "glTF external dependencies must use plain relative local paths");
        }
        auto dependency = uri_source->uri.fspath();
        if (dependency.empty() || dependency.is_absolute()) {
            return core::Status::failure(
                "gltf_import.external_uri_unsupported",
                "glTF external dependencies must use non-empty relative paths");
        }
        dependencies.push_back(std::move(dependency));
        return core::Status::ok();
    };
    for (const auto& buffer : parsed->buffers) {
        auto status = collect(buffer.data);
        if (!status) {
            return core::Result<std::vector<std::filesystem::path>>::failure(
                status.error().code, status.error().message);
        }
    }
    for (const auto& image : parsed->images) {
        auto status = collect(image.data);
        if (!status) {
            return core::Result<std::vector<std::filesystem::path>>::failure(
                status.error().code, status.error().message);
        }
    }
    std::ranges::sort(dependencies);
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    return core::Result<std::vector<std::filesystem::path>>::success(std::move(dependencies));
}

core::Result<ModelAsset> import_gltf_model(const std::filesystem::path& path,
                                           const ModelAssetLimits& limits) {
    auto limit_status = limits.validate();
    if (!limit_status) {
        return importer_failure(limit_status.error().code, limit_status.error().message);
    }
    if (path.empty()) {
        return importer_failure("gltf_import.missing_path", "glTF source path is required");
    }
    std::error_code file_error;
    const auto file_size = std::filesystem::file_size(path, file_error);
    if (file_error) {
        return importer_failure("gltf_import.read_failed", file_error.message());
    }
    if (file_size == 0 || file_size > limits.maximum_source_bytes) {
        return importer_failure("gltf_import.source_limit",
                                "glTF source is empty or exceeds its configured byte limit");
    }

    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        return fastgltf_failure(data.error(), "failed to read glTF source");
    }
    fastgltf::Parser parser(supported_gltf_extensions);
    const auto options =
        fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages |
        fastgltf::Options::DecomposeNodeMatrices | fastgltf::Options::GenerateMeshIndices;
    auto parsed = parser.loadGltf(data.get(), path.parent_path(), options);
    if (parsed.error() != fastgltf::Error::None) {
        return fastgltf_failure(parsed.error(), "failed to parse glTF source");
    }
    const auto validation_error = fastgltf::validate(parsed.get());
    if (validation_error != fastgltf::Error::None) {
        return fastgltf_failure(validation_error, "failed strict glTF validation");
    }
    std::size_t declared_buffer_bytes = 0;
    for (const auto& buffer : parsed->buffers) {
        if (buffer.byteLength > limits.maximum_source_bytes ||
            declared_buffer_bytes > limits.maximum_source_bytes - buffer.byteLength) {
            return importer_failure(
                "gltf_import.buffer_limit",
                "glTF declared buffer bytes exceed the configured model source limit");
        }
        declared_buffer_bytes += buffer.byteLength;
    }
    auto status = decompress_meshopt_buffer_views(parsed.get(), limits);
    if (!status) {
        return importer_failure(status.error().code, status.error().message);
    }
    status = decompress_draco_primitives(parsed.get(), limits);
    if (!status) {
        return importer_failure(status.error().code, status.error().message);
    }

    ModelAsset result;
    status = import_nodes(parsed.get(), result, limits);
    if (!status) {
        return importer_failure(status.error().code, status.error().message);
    }
    status = import_node_metadata(parsed.get(), result, limits);
    if (!status) {
        return importer_failure(status.error().code, status.error().message);
    }
    status = import_skins(parsed.get(), result, limits);
    if (!status) {
        return importer_failure(status.error().code, status.error().message);
    }
    status = import_samplers(parsed.get(), result, limits);
    if (!status) {
        return importer_failure(status.error().code, status.error().message);
    }
    status = import_materials(parsed.get(), result, limits);
    if (!status) {
        return importer_failure(status.error().code, status.error().message);
    }
    status = import_geometry(parsed.get(), result, limits);
    if (!status) {
        return importer_failure(status.error().code, status.error().message);
    }
    status = import_animations(parsed.get(), result, limits);
    if (!status) {
        return importer_failure(status.error().code, status.error().message);
    }
    status = validate_model_asset(result, limits);
    if (!status) {
        return importer_failure(status.error().code, status.error().message);
    }
    return core::Result<ModelAsset>::success(std::move(result));
}

} // namespace heartstead::assets
