#include "engine/assets/model_asset.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace heartstead::assets {

namespace {

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
    if (accessor.type != fastgltf::AccessorType::Vec3 ||
        accessor.componentType != fastgltf::ComponentType::Float || accessor.count == 0) {
        return core::Status::failure("gltf_import.invalid_positions",
                                     "glTF POSITION must be a non-empty float VEC3 accessor");
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
        [&](const auto& value, std::size_t index) { vertices[index].uv = {value[0], value[1]}; });
    if (!status) {
        return status;
    }

    const auto joints = primitive.findAttribute("JOINTS_0");
    const auto weights = primitive.findAttribute("WEIGHTS_0");
    const auto joints_1 = primitive.findAttribute("JOINTS_1");
    const auto weights_1 = primitive.findAttribute("WEIGHTS_1");
    if (joints_1 != primitive.attributes.end() || weights_1 != primitive.attributes.end()) {
        return core::Status::failure(
            "gltf_import.too_many_joint_sets",
            "glTF runtime format currently supports four joint influences per vertex");
    }
    if ((joints == primitive.attributes.end()) != (weights == primitive.attributes.end()) ||
        (skin != nullptr && joints == primitive.attributes.end())) {
        return core::Status::failure(
            "gltf_import.missing_skin_attributes",
            "skinned glTF primitive requires matching JOINTS_0 and WEIGHTS_0 attributes");
    }
    if (joints == primitive.attributes.end()) {
        return core::Status::ok();
    }
    if (skin == nullptr || joints->accessorIndex >= source.accessors.size() ||
        weights->accessorIndex >= source.accessors.size()) {
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
    fastgltf::iterateAccessorWithIndex<fastgltf::math::u16vec4>(
        source, joint_accessor, [&](const auto& value, std::size_t vertex_index) {
            for (std::size_t component = 0; component < 4; ++component) {
                vertices[vertex_index].joints[component] = value[component];
            }
        });
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
        source, weight_accessor, [&](const auto& value, std::size_t vertex_index) {
            float sum = 0.0F;
            for (std::size_t component = 0; component < 4; ++component) {
                vertices[vertex_index].weights[component] = value[component];
                sum += value[component];
            }
            if (std::isfinite(sum) && sum > 0.0F) {
                for (auto& weight : vertices[vertex_index].weights) {
                    weight /= sum;
                }
            }
        });
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

[[nodiscard]] core::Status import_geometry(const fastgltf::Asset& source, ModelAsset& target,
                                           const ModelAssetLimits& limits) {
    bool has_bounds = false;
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
            bool bad_index = false;
            fastgltf::iterateAccessorWithIndex<std::uint32_t>(
                source, index_accessor, [&](std::uint32_t value, std::size_t index) {
                    if (value >= vertices.size() ||
                        value > std::numeric_limits<std::uint32_t>::max() - first_vertex) {
                        bad_index = true;
                    } else {
                        target.indices[first_index + index] = first_vertex + value;
                    }
                });
            if (bad_index) {
                return core::Status::failure("gltf_import.index_out_of_bounds",
                                             "glTF index references a missing primitive vertex");
            }
            const auto primitive_label =
                mesh.name.empty() ? std::string{}
                                  : std::string(mesh.name) + "_" + std::to_string(primitive_index);
            auto name = model_name(primitive_label, "primitive", target.primitives.size(), limits);
            if (!name) {
                return core::Status::failure(name.error().code, name.error().message);
            }
            target.primitives.push_back({std::move(name).value(), first_vertex,
                                         static_cast<std::uint32_t>(vertices.size()), first_index,
                                         static_cast<std::uint32_t>(index_accessor.count),
                                         static_cast<std::uint32_t>(node_index), skin_index});
            for (const auto& vertex : vertices) {
                if (!has_bounds) {
                    target.bounds = {vertex.position, vertex.position};
                    has_bounds = true;
                } else {
                    target.bounds.min = math::component_min(target.bounds.min, vertex.position);
                    target.bounds.max = math::component_max(target.bounds.max, vertex.position);
                }
            }
        }
    }
    if (target.primitives.empty()) {
        return core::Status::failure("gltf_import.no_geometry",
                                     "glTF runtime model contains no triangle geometry");
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
        return core::Result<ModelAnimationPath>::failure(
            "gltf_import.unsupported_animation_path",
            "runtime skeletal clips do not currently support morph-weight animation");
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
            const auto expected_type = path.value() == ModelAnimationPath::rotation
                                           ? fastgltf::AccessorType::Vec4
                                           : fastgltf::AccessorType::Vec3;
            if (input.type != fastgltf::AccessorType::Scalar ||
                input.componentType != fastgltf::ComponentType::Float || input.count == 0 ||
                input.count > limits.maximum_keyframes_per_channel ||
                output.type != expected_type || output.count != input.count * output_multiplier) {
                return core::Status::failure(
                    "gltf_import.invalid_animation_accessor",
                    "glTF animation accessors have invalid type or keyframe counts");
            }
            ModelAnimationChannel channel;
            channel.node = static_cast<std::uint32_t>(*source_channel.nodeIndex);
            channel.path = path.value();
            channel.interpolation = interpolation;
            channel.times.resize(input.count);
            channel.values.resize(output.count);
            fastgltf::iterateAccessorWithIndex<float>(
                source, input,
                [&](float value, std::size_t index) { channel.times[index] = value; });
            if (channel.path == ModelAnimationPath::rotation) {
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
    fastgltf::Parser parser;
    const auto options = fastgltf::Options::LoadExternalBuffers |
                         fastgltf::Options::DecomposeNodeMatrices |
                         fastgltf::Options::GenerateMeshIndices;
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

    ModelAsset result;
    auto status = import_nodes(parsed.get(), result, limits);
    if (!status) {
        return importer_failure(status.error().code, status.error().message);
    }
    status = import_skins(parsed.get(), result, limits);
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
