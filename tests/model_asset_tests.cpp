#include "engine/animation/skeletal_animation.hpp"
#include "engine/assets/model_asset.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void append_i16(std::vector<std::uint8_t>& bytes, std::int16_t value) {
    append_u16(bytes, std::bit_cast<std::uint16_t>(value));
}

void append_f32(std::vector<std::uint8_t>& bytes, float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (std::uint32_t shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xFFU));
    }
}

void append_vec2(std::vector<std::uint8_t>& bytes, float x, float y) {
    append_f32(bytes, x);
    append_f32(bytes, y);
}

void append_vec3(std::vector<std::uint8_t>& bytes, float x, float y, float z) {
    append_f32(bytes, x);
    append_f32(bytes, y);
    append_f32(bytes, z);
}

void append_vec4(std::vector<std::uint8_t>& bytes, float x, float y, float z, float w) {
    append_f32(bytes, x);
    append_f32(bytes, y);
    append_f32(bytes, z);
    append_f32(bytes, w);
}

void append_identity(std::vector<std::uint8_t>& bytes, float translate_y) {
    const std::array values{
        1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F,        0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, translate_y, 0.0F, 1.0F,
    };
    for (const auto value : values) {
        append_f32(bytes, value);
    }
}

std::vector<std::uint8_t> animated_triangle_buffer() {
    std::vector<std::uint8_t> bytes;
    append_vec3(bytes, -0.5F, 0.0F, 0.0F);
    append_vec3(bytes, 0.5F, 0.0F, 0.0F);
    append_vec3(bytes, 0.0F, 1.0F, 0.0F);
    for (int index = 0; index < 3; ++index) {
        append_vec3(bytes, 0.0F, 0.0F, 1.0F);
    }
    append_vec2(bytes, 0.0F, 0.0F);
    append_vec2(bytes, 1.0F, 0.0F);
    append_vec2(bytes, 0.5F, 1.0F);
    for (int index = 0; index < 3; ++index) {
        append_u16(bytes, 0);
        append_u16(bytes, 1);
        append_u16(bytes, 0);
        append_u16(bytes, 0);
    }
    for (int index = 0; index < 3; ++index) {
        append_vec4(bytes, 0.5F, 0.5F, 0.0F, 0.0F);
    }
    append_u16(bytes, 0);
    append_u16(bytes, 1);
    append_u16(bytes, 2);
    append_u16(bytes, 0);
    append_identity(bytes, 0.0F);
    append_identity(bytes, -1.0F);
    append_f32(bytes, 0.0F);
    append_f32(bytes, 1.0F);
    append_vec4(bytes, 0.0F, 0.0F, 0.0F, 1.0F);
    append_vec4(bytes, 0.0F, 0.0F, 0.70710677F, 0.70710677F);
    append_vec2(bytes, 0.1F, 0.2F);
    append_vec2(bytes, 0.8F, 0.2F);
    append_vec2(bytes, 0.4F, 0.9F);
    for (const auto color : {std::array<std::uint8_t, 4>{255, 128, 64, 255},
                             std::array<std::uint8_t, 4>{64, 255, 128, 192},
                             std::array<std::uint8_t, 4>{128, 64, 255, 128}}) {
        bytes.insert(bytes.end(), color.begin(), color.end());
    }
    for (int index = 0; index < 3; ++index) {
        append_i16(bytes, 32'767);
        append_i16(bytes, 0);
        append_i16(bytes, 0);
        append_i16(bytes, 32'767);
    }
    for (int index = 0; index < 3; ++index) {
        append_u16(bytes, 1);
        append_u16(bytes, 0);
        append_u16(bytes, 0);
        append_u16(bytes, 0);
    }
    for (int index = 0; index < 3; ++index) {
        append_vec4(bytes, 0.75F, 0.0F, 0.0F, 0.0F);
    }
    append_vec3(bytes, 0.0F, 0.25F, 0.0F);
    append_vec3(bytes, 0.0F, 0.25F, 0.0F);
    append_vec3(bytes, 0.0F, 0.25F, 0.0F);
    append_f32(bytes, 0.0F);
    append_f32(bytes, 1.0F);
    assert(bytes.size() == 520);
    return bytes;
}

std::vector<std::uint8_t> one_pixel_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x04, 0x00, 0x00,
        0x00, 0xb5, 0x1c, 0x0c, 0x02, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54, 0x78,
        0xda, 0x63, 0x64, 0xf8, 0x0f, 0x00, 0x01, 0x05, 0x01, 0x01, 0x27, 0x18, 0xe3, 0x66,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

std::string animated_triangle_gltf() {
    return R"({
  "asset":{"version":"2.0"},
  "extensionsUsed":["KHR_materials_unlit","KHR_texture_transform","KHR_mesh_quantization"],
  "extensionsRequired":["KHR_materials_unlit","KHR_texture_transform","KHR_mesh_quantization"],
  "scene":0,
  "scenes":[{"nodes":[0,1]}],
  "nodes":[
    {"name":"body","mesh":0,"skin":0},
    {"name":"root","children":[2]},
    {"name":"hand","translation":[0,1,0]}
  ],
  "meshes":[{"name":"player","weights":[0],"primitives":[{
    "attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2,"TEXCOORD_1":9,
                  "COLOR_0":10,"TANGENT":11,
                  "JOINTS_0":3,"WEIGHTS_0":4,"JOINTS_1":12,"WEIGHTS_1":13},
    "indices":5,
    "material":0,
    "targets":[{"POSITION":14}]
  }]}],
  "materials":[{
    "name":"storybook_cloth",
    "pbrMetallicRoughness":{
      "baseColorFactor":[0.8,0.6,0.4,1.0],
      "baseColorTexture":{"index":0,"texCoord":1,"extensions":{"KHR_texture_transform":{
        "offset":[0.25,0.5],"scale":[2,3],"rotation":0.5,"texCoord":1
      }}},
      "metallicFactor":0.35,
      "roughnessFactor":0.65,
      "metallicRoughnessTexture":{"index":0}
    },
    "normalTexture":{"index":0,"scale":0.75},
    "occlusionTexture":{"index":0,"strength":0.6},
    "emissiveTexture":{"index":0},
    "emissiveFactor":[0.1,0.2,0.3],
    "alphaMode":"MASK",
    "alphaCutoff":0.25,
    "doubleSided":true,
    "extensions":{"KHR_materials_unlit":{}}
  },{
    "name":"glass",
    "pbrMetallicRoughness":{"baseColorFactor":[0.2,0.4,0.8,0.5]},
    "alphaMode":"BLEND"
  }],
  "textures":[{"source":0,"sampler":0}],
  "samplers":[{"magFilter":9728,"minFilter":9987,"wrapS":33071,"wrapT":33648}],
  "images":[{"name":"storybook_color","uri":"player.png"}],
  "skins":[{"name":"rig","inverseBindMatrices":6,"skeleton":1,"joints":[1,2]}],
  "animations":[{"name":"wave","samplers":[{
    "input":7,"output":8,"interpolation":"LINEAR"
  }],"channels":[{"sampler":0,"target":{"node":2,"path":"rotation"}}]},
  {"name":"morph","samplers":[{"input":7,"output":15,"interpolation":"LINEAR"}],
   "channels":[{"sampler":0,"target":{"node":0,"path":"weights"}}]}],
  "buffers":[{"uri":"player.bin","byteLength":520}],
  "bufferViews":[
    {"buffer":0,"byteOffset":0,"byteLength":36},
    {"buffer":0,"byteOffset":36,"byteLength":36},
    {"buffer":0,"byteOffset":72,"byteLength":24},
    {"buffer":0,"byteOffset":96,"byteLength":24},
    {"buffer":0,"byteOffset":120,"byteLength":48},
    {"buffer":0,"byteOffset":168,"byteLength":6},
    {"buffer":0,"byteOffset":176,"byteLength":128},
    {"buffer":0,"byteOffset":304,"byteLength":8},
    {"buffer":0,"byteOffset":312,"byteLength":32},
    {"buffer":0,"byteOffset":344,"byteLength":24},
    {"buffer":0,"byteOffset":368,"byteLength":12},
    {"buffer":0,"byteOffset":380,"byteLength":24},
    {"buffer":0,"byteOffset":404,"byteLength":24},
    {"buffer":0,"byteOffset":428,"byteLength":48},
    {"buffer":0,"byteOffset":476,"byteLength":36},
    {"buffer":0,"byteOffset":512,"byteLength":8}
  ],
  "accessors":[
    {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",
     "min":[-0.5,0,0],"max":[0.5,1,0]},
    {"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
    {"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},
    {"bufferView":3,"componentType":5123,"count":3,"type":"VEC4"},
    {"bufferView":4,"componentType":5126,"count":3,"type":"VEC4"},
    {"bufferView":5,"componentType":5123,"count":3,"type":"SCALAR"},
    {"bufferView":6,"componentType":5126,"count":2,"type":"MAT4"},
    {"bufferView":7,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
    {"bufferView":8,"componentType":5126,"count":2,"type":"VEC4"},
    {"bufferView":9,"componentType":5126,"count":3,"type":"VEC2"},
    {"bufferView":10,"componentType":5121,"normalized":true,"count":3,"type":"VEC4"},
    {"bufferView":11,"componentType":5122,"normalized":true,"count":3,"type":"VEC4"},
    {"bufferView":12,"componentType":5123,"count":3,"type":"VEC4"},
    {"bufferView":13,"componentType":5126,"count":3,"type":"VEC4"},
    {"bufferView":14,"componentType":5126,"count":3,"type":"VEC3",
     "min":[0,0.25,0],"max":[0,0.25,0]},
    {"bufferView":15,"componentType":5126,"count":2,"type":"SCALAR"}
  ]
})";
}

void write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    assert(output);
}

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::trunc);
    assert(output);
    output << text;
    assert(output);
}

void test_typed_gltf_import_and_codec() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("heartstead_model_asset_tests_" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    const auto model_path = root / "player.gltf";
    write_file(root / "player.bin", animated_triangle_buffer());
    write_file(root / "player.png", one_pixel_png());
    write_file(model_path, animated_triangle_gltf());

    auto dependencies = heartstead::assets::discover_gltf_external_dependencies(model_path);
    assert(dependencies);
    assert(dependencies.value() ==
           std::vector<std::filesystem::path>({"player.bin", "player.png"}));
    auto imported = heartstead::assets::import_gltf_model(model_path);
    assert(imported);
    assert(imported.value().vertices.size() == 3);
    assert(imported.value().vertices[0].uv1.x == 0.1F);
    assert(imported.value().vertices[0].color[0] == 1.0F);
    assert(imported.value().vertices[0].color[1] > 0.49F);
    assert(imported.value().vertices[0].tangent.x > 0.99F);
    assert(imported.value().vertices[0].joints[0] == 1);
    assert(imported.value().indices == std::vector<std::uint32_t>({0, 1, 2}));
    assert(imported.value().nodes.size() == 3);
    assert(imported.value().nodes[2].parent == 1);
    assert(imported.value().skins.size() == 1);
    assert(imported.value().skins[0].joints == std::vector<std::uint32_t>({1, 2}));
    assert(imported.value().primitives.size() == 1);
    assert(imported.value().primitives[0].skin == 0);
    assert(imported.value().primitives[0].material == 0);
    assert(imported.value().primitives[0].morph_targets.size() == 1);
    assert(imported.value().primitives[0].morph_targets[0].position_deltas.size() == 3);
    assert(imported.value().images.size() == 1);
    assert(imported.value().images[0].width == 1);
    assert(imported.value().images[0].height == 1);
    assert(imported.value().images[0].rgba8.size() == 4);
    assert(imported.value().samplers.size() == 1);
    assert(imported.value().samplers[0].mag_filter ==
           heartstead::assets::ModelTextureMagFilter::nearest);
    assert(imported.value().samplers[0].min_filter ==
           heartstead::assets::ModelTextureMinFilter::linear_mipmap_linear);
    assert(imported.value().samplers[0].wrap_s ==
           heartstead::assets::ModelTextureWrap::clamp_to_edge);
    assert(imported.value().samplers[0].wrap_t ==
           heartstead::assets::ModelTextureWrap::mirrored_repeat);
    assert(imported.value().materials.size() == 2);
    assert(imported.value().materials[0].name == "storybook_cloth");
    assert(imported.value().materials[0].base_color_texture.image == 0);
    assert(imported.value().materials[0].base_color_texture.sampler == 0);
    assert(imported.value().materials[0].base_color_texture.texcoord == 1);
    assert(imported.value().materials[0].base_color_texture.offset.x == 0.25F);
    assert(imported.value().materials[0].base_color_texture.scale.y == 3.0F);
    assert(imported.value().materials[0].metallic_factor == 0.35F);
    assert(imported.value().materials[0].roughness_factor == 0.65F);
    assert(imported.value().materials[0].normal_scale == 0.75F);
    assert(imported.value().materials[0].occlusion_strength == 0.6F);
    assert(imported.value().materials[0].emissive_factor[2] > 0.29F);
    assert(imported.value().materials[0].alpha_mode == heartstead::assets::ModelAlphaMode::mask);
    assert(imported.value().materials[0].alpha_cutoff == 0.25F);
    assert(imported.value().materials[0].double_sided);
    assert(imported.value().materials[0].unlit);
    assert(imported.value().materials[1].alpha_mode == heartstead::assets::ModelAlphaMode::blend);
    assert(imported.value().animations.size() == 2);
    assert(imported.value().animations[0].name == "wave");
    assert(imported.value().animations[0].duration_seconds == 1.0F);
    assert(imported.value().animations[0].channels[0].values.size() == 2);
    assert(imported.value().animations[1].channels[0].path ==
           heartstead::assets::ModelAnimationPath::weights);
    assert(imported.value().animations[1].channels[0].weight_count == 1);
    assert(imported.value().animations[1].channels[0].weight_values ==
           std::vector<float>({0.0F, 1.0F}));
    assert(heartstead::assets::resolve_model_animation_clip(imported.value(), "wave").value() == 0);
    auto missing_clip =
        heartstead::assets::resolve_model_animation_clip(imported.value(), "missing");
    assert(!missing_clip);
    assert(missing_clip.error().code == "model_asset.missing_animation_name");
    auto ambiguous_clips = imported.value();
    ambiguous_clips.animations.push_back(ambiguous_clips.animations.front());
    auto ambiguous = heartstead::assets::resolve_model_animation_clip(ambiguous_clips, "wave");
    assert(!ambiguous);
    assert(ambiguous.error().code == "model_asset.ambiguous_animation_name");

    auto encoded = heartstead::assets::encode_model_asset(imported.value());
    assert(encoded);
    auto decoded = heartstead::assets::decode_model_asset(encoded.value());
    assert(decoded);
    assert(decoded.value() == imported.value());

    auto legacy_model = imported.value();
    for (auto& vertex : legacy_model.vertices) {
        vertex.tangent = {};
        vertex.tangent.x = 1.0F;
        vertex.tangent.w = 1.0F;
        vertex.uv1 = {};
        vertex.color = {1.0F, 1.0F, 1.0F, 1.0F};
    }
    legacy_model.samplers.clear();
    for (auto& material : legacy_model.materials) {
        const auto base_image = material.base_color_texture.image;
        const auto base_factor = material.base_color_factor;
        const auto name = material.name;
        const auto alpha_mode = material.alpha_mode;
        const auto alpha_cutoff = material.alpha_cutoff;
        const auto double_sided = material.double_sided;
        material = {};
        material.name = name;
        material.base_color_factor = base_factor;
        material.base_color_texture.image = base_image;
        material.alpha_mode = alpha_mode;
        material.alpha_cutoff = alpha_cutoff;
        material.double_sided = double_sided;
    }
    legacy_model.nodes[0].morph_weights.clear();
    legacy_model.primitives[0].morph_targets.clear();
    legacy_model.animations.resize(1);
    auto legacy_encoded = heartstead::assets::encode_model_asset(legacy_model);
    assert(legacy_encoded);
    std::size_t v4_extension_bytes = legacy_model.vertices.size() * 40U + 4U +
                                     legacy_model.samplers.size() * 4U +
                                     legacy_model.materials.size() * 173U;
    for (const auto& node : legacy_model.nodes) {
        v4_extension_bytes += 4U + node.morph_weights.size() * sizeof(float);
    }
    for (const auto& primitive : legacy_model.primitives) {
        v4_extension_bytes += 4U;
        for (const auto& target : primitive.morph_targets) {
            v4_extension_bytes +=
                1U + (target.position_deltas.size() + target.normal_deltas.size() +
                      target.tangent_deltas.size()) *
                         sizeof(float) * 3U;
        }
    }
    for (const auto& clip : legacy_model.animations) {
        for (const auto& channel : clip.channels) {
            v4_extension_bytes += 8U + channel.weight_values.size() * sizeof(float);
        }
    }
    assert(v4_extension_bytes < legacy_encoded.value().size());
    legacy_encoded.value().resize(legacy_encoded.value().size() - v4_extension_bytes);
    constexpr std::string_view current_magic = "heartstead.model.v4";
    const auto magic = std::search(legacy_encoded.value().begin(), legacy_encoded.value().end(),
                                   current_magic.begin(), current_magic.end());
    assert(magic != legacy_encoded.value().end());
    *(magic + static_cast<std::ptrdiff_t>(current_magic.size() - 1U)) = '2';
    auto legacy_decoded = heartstead::assets::decode_model_asset(legacy_encoded.value());
    assert(legacy_decoded);
    assert(legacy_decoded.value() == legacy_model);

    for (std::size_t size = 0; size < encoded.value().size(); ++size) {
        assert(!heartstead::assets::decode_model_asset(
            std::span<const std::uint8_t>{encoded.value().data(), size}));
    }

    auto bad_joint = imported.value();
    bad_joint.vertices[0].joints[0] = 2;
    assert(!heartstead::assets::validate_model_asset(bad_joint));

    heartstead::assets::ModelAssetLimits restrictive;
    restrictive.maximum_joints_per_skin = 1;
    assert(!heartstead::assets::import_gltf_model(model_path, restrictive));

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    assert(!cleanup_error);
}

void test_base_storybook_player_asset() {
    const auto model_path = std::filesystem::path{HEARTSTEAD_TEST_SOURCE_DIR} /
                            "mods/base/assets/models/entities/storybook_player.gltf";
    auto imported = heartstead::assets::import_gltf_model(model_path);
    assert(imported);
    assert(imported.value().vertices.size() == 4);
    assert(imported.value().indices.size() == 6);
    assert(imported.value().skins.size() == 1);
    assert(imported.value().skins.front().joints.size() == 2);
    assert(imported.value().images.size() == 1);
    assert(imported.value().images.front().width == 2);
    assert(imported.value().images.front().height == 2);
    assert(imported.value().materials.size() == 1);
    assert(imported.value().primitives.front().material == 0);
    assert(imported.value().materials.front().base_color_texture.image == 0);
    assert(imported.value().animations.size() == 6);
    assert(imported.value().animations[0].name == "idle");
    assert(imported.value().animations[1].name == "walk");
    assert(imported.value().animations[2].name == "swim");
    assert(imported.value().animations[3].name == "run");
    assert(imported.value().animations[4].name == "jump");
    assert(imported.value().animations[5].name == "fall");
    auto encoded = heartstead::assets::encode_model_asset(imported.value());
    assert(encoded);
    auto runtime_model = heartstead::assets::decode_model_asset(encoded.value());
    assert(runtime_model);
    assert(runtime_model.value() == imported.value());
}

void test_base_rigid_node_player_asset() {
    const auto model_path = std::filesystem::path{HEARTSTEAD_TEST_SOURCE_DIR} /
                            "mods/base/assets/models/entities/test_player.glb";
    auto imported = heartstead::assets::import_gltf_model(model_path);
    assert(imported);
    assert(imported.value().nodes.size() == 8);
    assert(imported.value().primitives.size() == 6);
    assert(imported.value().skins.empty());
    assert(imported.value().animations.size() == 27);
    assert(imported.value().images.size() == 1);
    assert(imported.value().materials.size() == 1);
    assert(imported.value().materials[0].base_color_texture.image == 0);
    const auto capabilities = heartstead::assets::model_capabilities(imported.value());
    assert(capabilities.has_animation_clips);
    assert(capabilities.has_animated_nodes);
    assert(!capabilities.has_skins);
    assert(!capabilities.has_morph_targets);
    for (const auto& primitive : imported.value().primitives) {
        assert(primitive.node < imported.value().nodes.size());
        assert(primitive.skin == heartstead::assets::no_model_index);
        assert(primitive.material == 0);
    }
    const auto idle = heartstead::assets::resolve_model_animation_clip(imported.value(), "idle");
    assert(idle);
    assert(heartstead::assets::resolve_model_animation_clip(imported.value(), "walk"));
    assert(heartstead::assets::resolve_model_animation_clip(imported.value(), "sprint"));
    const auto head_node =
        std::ranges::find(imported.value().nodes, "head", &heartstead::assets::ModelNode::name);
    assert(head_node != imported.value().nodes.end());
    const auto head_index =
        static_cast<std::size_t>(std::distance(imported.value().nodes.begin(), head_node));
    assert(std::abs(head_node->bind_transform.scale.x - 0.1F) < 0.0001F);
    assert(std::abs(head_node->bind_transform.scale.y - 0.1F) < 0.0001F);
    assert(std::abs(head_node->bind_transform.scale.z - 0.1F) < 0.0001F);
    auto idle_pose =
        heartstead::animation::sample_animation_clip(imported.value(), {idle.value(), 0.5F, true});
    assert(idle_pose);
    assert(std::abs(idle_pose.value().local_transforms[head_index].scale.x - 0.1F) < 0.0001F);
    assert(std::abs(idle_pose.value().local_transforms[head_index].scale.y - 0.1F) < 0.0001F);
    assert(std::abs(idle_pose.value().local_transforms[head_index].scale.z - 0.1F) < 0.0001F);
    auto idle_matrices =
        heartstead::animation::evaluate_model_node_matrices(imported.value(), idle_pose.value());
    assert(idle_matrices);
    const auto& head_matrix = idle_matrices.value()[head_index];
    const auto head_x_scale = std::sqrt(head_matrix.at(0, 0) * head_matrix.at(0, 0) +
                                        head_matrix.at(1, 0) * head_matrix.at(1, 0) +
                                        head_matrix.at(2, 0) * head_matrix.at(2, 0));
    assert(std::abs(head_x_scale - 0.1F) < 0.0001F);
    const auto head_primitive =
        std::ranges::find(imported.value().primitives, static_cast<std::uint32_t>(head_index),
                          &heartstead::assets::ModelPrimitive::node);
    assert(head_primitive != imported.value().primitives.end());
    assert(head_primitive->material == 0);
    bool head_has_nonzero_uv = false;
    for (std::size_t offset = 0; offset < head_primitive->vertex_count; ++offset) {
        const auto& uv = imported.value().vertices[head_primitive->first_vertex + offset].uv0;
        head_has_nonzero_uv = head_has_nonzero_uv || uv.x != 0.0F || uv.y != 0.0F;
    }
    assert(head_has_nonzero_uv);
    auto encoded = heartstead::assets::encode_model_asset(imported.value());
    assert(encoded);
    auto runtime_model = heartstead::assets::decode_model_asset(encoded.value());
    assert(runtime_model);
    assert(runtime_model.value() == imported.value());
}

} // namespace

int main() {
    test_typed_gltf_import_and_codec();
    test_base_storybook_player_asset();
    test_base_rigid_node_player_asset();
    return 0;
}
