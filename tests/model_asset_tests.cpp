#include "engine/assets/model_asset.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <chrono>
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
    assert(bytes.size() == 344);
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
  "scene":0,
  "scenes":[{"nodes":[0,1]}],
  "nodes":[
    {"name":"body","mesh":0,"skin":0},
    {"name":"root","children":[2]},
    {"name":"hand","translation":[0,1,0]}
  ],
  "meshes":[{"name":"player","primitives":[{
    "attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2,"JOINTS_0":3,"WEIGHTS_0":4},
    "indices":5,
    "material":0
  }]}],
  "materials":[{
    "name":"storybook_cloth",
    "pbrMetallicRoughness":{
      "baseColorFactor":[0.8,0.6,0.4,1.0],
      "baseColorTexture":{"index":0}
    },
    "alphaMode":"MASK",
    "alphaCutoff":0.25,
    "doubleSided":true
  }],
  "textures":[{"source":0}],
  "images":[{"name":"storybook_color","uri":"player.png"}],
  "skins":[{"name":"rig","inverseBindMatrices":6,"skeleton":1,"joints":[1,2]}],
  "animations":[{"name":"wave","samplers":[{
    "input":7,"output":8,"interpolation":"LINEAR"
  }],"channels":[{"sampler":0,"target":{"node":2,"path":"rotation"}}]}],
  "buffers":[{"uri":"player.bin","byteLength":344}],
  "bufferViews":[
    {"buffer":0,"byteOffset":0,"byteLength":36},
    {"buffer":0,"byteOffset":36,"byteLength":36},
    {"buffer":0,"byteOffset":72,"byteLength":24},
    {"buffer":0,"byteOffset":96,"byteLength":24},
    {"buffer":0,"byteOffset":120,"byteLength":48},
    {"buffer":0,"byteOffset":168,"byteLength":6},
    {"buffer":0,"byteOffset":176,"byteLength":128},
    {"buffer":0,"byteOffset":304,"byteLength":8},
    {"buffer":0,"byteOffset":312,"byteLength":32}
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
    {"bufferView":8,"componentType":5126,"count":2,"type":"VEC4"}
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
    assert(imported.value().indices == std::vector<std::uint32_t>({0, 1, 2}));
    assert(imported.value().nodes.size() == 3);
    assert(imported.value().nodes[2].parent == 1);
    assert(imported.value().skins.size() == 1);
    assert(imported.value().skins[0].joints == std::vector<std::uint32_t>({1, 2}));
    assert(imported.value().primitives.size() == 1);
    assert(imported.value().primitives[0].skin == 0);
    assert(imported.value().primitives[0].material == 0);
    assert(imported.value().images.size() == 1);
    assert(imported.value().images[0].width == 1);
    assert(imported.value().images[0].height == 1);
    assert(imported.value().images[0].rgba8.size() == 4);
    assert(imported.value().materials.size() == 1);
    assert(imported.value().materials[0].name == "storybook_cloth");
    assert(imported.value().materials[0].base_color_image == 0);
    assert(imported.value().materials[0].alpha_mode == heartstead::assets::ModelAlphaMode::mask);
    assert(imported.value().materials[0].alpha_cutoff == 0.25F);
    assert(imported.value().materials[0].double_sided);
    assert(imported.value().animations.size() == 1);
    assert(imported.value().animations[0].name == "wave");
    assert(imported.value().animations[0].duration_seconds == 1.0F);
    assert(imported.value().animations[0].channels[0].values.size() == 2);

    auto encoded = heartstead::assets::encode_model_asset(imported.value());
    assert(encoded);
    auto decoded = heartstead::assets::decode_model_asset(encoded.value());
    assert(decoded);
    assert(decoded.value() == imported.value());

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
    assert(imported.value().materials.front().base_color_image == 0);
    assert(imported.value().animations.size() == 3);
    assert(imported.value().animations[0].name == "idle");
    assert(imported.value().animations[1].name == "walk");
    assert(imported.value().animations[2].name == "swim");
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
    return 0;
}
