#include "engine/assets/image_asset.hpp"
#include "engine/assets/model_asset.hpp"

#include <draco/compression/encode.h>
#include <draco/core/encoder_buffer.h>
#include <draco/mesh/triangle_soup_mesh_builder.h>
#include <ktx.h>
#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using heartstead::assets::ModelAsset;

constexpr std::array<float, 9> triangle_positions{
    -0.5F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
};
constexpr std::array<std::uint32_t, 3> triangle_indices{0, 1, 2};

void append_bytes(std::vector<std::uint8_t>& target, const void* source, std::size_t size) {
    const auto* first = static_cast<const std::uint8_t*>(source);
    target.insert(target.end(), first, first + size);
}

void write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    assert(output);
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::trunc);
    assert(output);
    output << text;
    assert(output);
}

std::filesystem::path make_test_directory(std::string_view label) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("heartstead_" + std::string(label) + "_" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    return root;
}

void assert_triangle(const ModelAsset& model) {
    assert(model.vertices.size() == 3);
    assert(model.indices.size() == 3);
    assert(model.primitives.size() == 1);
    for (const auto index : model.indices) {
        assert(index < model.vertices.size());
    }
    const auto minimum_x = std::min(
        {model.vertices[0].position.x, model.vertices[1].position.x, model.vertices[2].position.x});
    const auto maximum_x = std::max(
        {model.vertices[0].position.x, model.vertices[1].position.x, model.vertices[2].position.x});
    const auto maximum_y = std::max(
        {model.vertices[0].position.y, model.vertices[1].position.y, model.vertices[2].position.y});
    assert(std::abs(minimum_x + 0.5F) < 0.001F);
    assert(std::abs(maximum_x - 0.5F) < 0.001F);
    assert(std::abs(maximum_y - 1.0F) < 0.001F);
}

void test_meshopt_compressed_import() {
    constexpr std::size_t fallback_bytes = sizeof(triangle_positions) + sizeof(triangle_indices);
    std::vector<std::uint8_t> binary(fallback_bytes);

    std::vector<std::uint8_t> encoded_vertices(
        meshopt_encodeVertexBufferBound(3, sizeof(float) * 3));
    const auto encoded_vertex_size =
        meshopt_encodeVertexBuffer(encoded_vertices.data(), encoded_vertices.size(),
                                   triangle_positions.data(), 3, sizeof(float) * 3);
    assert(encoded_vertex_size > 0);
    encoded_vertices.resize(encoded_vertex_size);
    const auto vertex_source_offset = binary.size();
    binary.insert(binary.end(), encoded_vertices.begin(), encoded_vertices.end());

    std::vector<std::uint8_t> encoded_indices(
        meshopt_encodeIndexBufferBound(triangle_indices.size(), 3));
    const auto encoded_index_size =
        meshopt_encodeIndexBuffer(encoded_indices.data(), encoded_indices.size(),
                                  triangle_indices.data(), triangle_indices.size());
    assert(encoded_index_size > 0);
    encoded_indices.resize(encoded_index_size);
    const auto index_source_offset = binary.size();
    binary.insert(binary.end(), encoded_indices.begin(), encoded_indices.end());

    std::ostringstream json;
    json << R"({
  "asset":{"version":"2.0"},
  "extensionsUsed":["EXT_meshopt_compression"],
  "extensionsRequired":["EXT_meshopt_compression"],
  "scene":0,
  "scenes":[{"nodes":[0]}],
  "nodes":[{"mesh":0}],
  "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],
  "buffers":[{"uri":"meshopt.bin","byteLength":)"
         << binary.size() << R"(}],
  "bufferViews":[
    {"buffer":0,"byteOffset":0,"byteLength":36,"byteStride":12,
     "extensions":{"EXT_meshopt_compression":{"buffer":0,"byteOffset":)"
         << vertex_source_offset << R"(,"byteLength":)" << encoded_vertices.size() << R"(,
       "byteStride":12,"count":3,"mode":"ATTRIBUTES"}}},
    {"buffer":0,"byteOffset":36,"byteLength":12,
     "extensions":{"EXT_meshopt_compression":{"buffer":0,"byteOffset":)"
         << index_source_offset << R"(,"byteLength":)" << encoded_indices.size() << R"(,
       "byteStride":4,"count":3,"mode":"TRIANGLES"}}}
  ],
  "accessors":[
    {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",
     "min":[-0.5,0,0],"max":[0.5,1,0]},
    {"bufferView":1,"componentType":5125,"count":3,"type":"SCALAR"}
  ]
})";

    const auto root = make_test_directory("meshopt_import");
    write_file(root / "meshopt.bin", binary);
    write_file(root / "meshopt.gltf", json.str());
    const auto imported = heartstead::assets::import_gltf_model(root / "meshopt.gltf");
    assert(imported);
    assert_triangle(imported.value());
    std::error_code error;
    std::filesystem::remove_all(root, error);
    assert(!error);
}

std::vector<std::uint8_t> encode_draco_triangle(std::uint32_t& position_unique_id) {
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(1);
    const auto position_attribute =
        builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    assert(position_attribute >= 0);
    position_unique_id = 17;
    builder.SetAttributeUniqueId(position_attribute, position_unique_id);
    builder.SetAttributeValuesForFace(position_attribute, draco::FaceIndex(0),
                                      triangle_positions.data(), triangle_positions.data() + 3,
                                      triangle_positions.data() + 6);
    auto mesh = builder.Finalize();
    assert(mesh);

    draco::Encoder encoder;
    encoder.SetSpeedOptions(5, 5);
    draco::EncoderBuffer encoded;
    const auto status = encoder.EncodeMeshToBuffer(*mesh, &encoded);
    assert(status.ok());
    const auto* first = reinterpret_cast<const std::uint8_t*>(encoded.data());
    return {first, first + encoded.size()};
}

void test_draco_compressed_import() {
    std::uint32_t position_unique_id = 0;
    const auto binary = encode_draco_triangle(position_unique_id);
    assert(!binary.empty());

    std::ostringstream json;
    json << R"({
  "asset":{"version":"2.0"},
  "extensionsUsed":["KHR_draco_mesh_compression"],
  "extensionsRequired":["KHR_draco_mesh_compression"],
  "scene":0,
  "scenes":[{"nodes":[0]}],
  "nodes":[{"mesh":0}],
  "meshes":[{"primitives":[{
    "attributes":{"POSITION":0},
    "indices":1,
    "extensions":{"KHR_draco_mesh_compression":{
      "bufferView":0,"attributes":{"POSITION":)"
         << position_unique_id << R"(}
    }}
  }]}],
  "buffers":[{"uri":"draco.bin","byteLength":)"
         << binary.size() << R"(}],
  "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)"
         << binary.size() << R"(}],
  "accessors":[
    {"componentType":5126,"count":3,"type":"VEC3",
     "min":[-0.5,0,0],"max":[0.5,1,0]},
    {"componentType":5125,"count":3,"type":"SCALAR"}
  ]
})";

    const auto root = make_test_directory("draco_import");
    write_file(root / "draco.bin", binary);
    write_file(root / "draco.gltf", json.str());
    const auto imported = heartstead::assets::import_gltf_model(root / "draco.gltf");
    assert(imported);
    assert_triangle(imported.value());
    std::error_code error;
    std::filesystem::remove_all(root, error);
    assert(!error);
}

std::vector<std::uint8_t> make_basis_ktx2() {
    // VkFormat 37 is VK_FORMAT_R8G8B8A8_UNORM; libktx accepts the numeric
    // format without introducing the Vulkan SDK as a test dependency.
    ktxTextureCreateInfo info{};
    info.vkFormat = 37;
    info.baseWidth = 4;
    info.baseHeight = 4;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = 1;
    info.numLayers = 1;
    info.numFaces = 1;
    ktxTexture2* raw_texture = nullptr;
    assert(ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &raw_texture) ==
           KTX_SUCCESS);
    assert(raw_texture != nullptr);
    const auto destroy = [](ktxTexture2* texture) { ktxTexture2_Destroy(texture); };
    std::unique_ptr<ktxTexture2, decltype(destroy)> texture(raw_texture, destroy);

    constexpr std::array<std::uint8_t, 64> pixels{
        255, 0,   0,   255, 0,   255, 0,   255, 0,   0,   255, 255, 255, 255, 255, 255,
        0,   255, 0,   255, 0,   0,   255, 255, 255, 255, 255, 255, 255, 0,   0,   255,
        0,   0,   255, 255, 255, 255, 255, 255, 255, 0,   0,   255, 0,   255, 0,   255,
        255, 255, 255, 255, 255, 0,   0,   255, 0,   255, 0,   255, 0,   0,   255, 255,
    };
    assert(ktxTexture_SetImageFromMemory(ktxTexture(texture.get()), 0, 0, 0, pixels.data(),
                                         pixels.size()) == KTX_SUCCESS);
    assert(ktxTexture2_CompressBasis(texture.get(), 128) == KTX_SUCCESS);
    assert(ktxTexture2_NeedsTranscoding(texture.get()));

    ktx_uint8_t* encoded = nullptr;
    ktx_size_t encoded_size = 0;
    assert(ktxTexture_WriteToMemory(ktxTexture(texture.get()), &encoded, &encoded_size) ==
           KTX_SUCCESS);
    assert(encoded != nullptr);
    std::vector<std::uint8_t> result(encoded, encoded + encoded_size);
    std::free(encoded);
    return result;
}

void test_basisu_ktx2_import() {
    const auto image = make_basis_ktx2();
    auto directly_decoded = heartstead::assets::decode_ktx2(image);
    assert(directly_decoded);
    assert(directly_decoded.value().width == 4);
    assert(directly_decoded.value().height == 4);
    assert(directly_decoded.value().rgba8.size() == 64);

    std::vector<std::uint8_t> geometry;
    append_bytes(geometry, triangle_positions.data(), sizeof(triangle_positions));
    append_bytes(geometry, triangle_indices.data(), sizeof(triangle_indices));
    const auto root = make_test_directory("basisu_import");
    write_file(root / "geometry.bin", geometry);
    write_file(root / "color.ktx2", image);
    write_file(root / "basisu.gltf", R"({
  "asset":{"version":"2.0"},
  "extensionsUsed":["KHR_texture_basisu"],
  "extensionsRequired":["KHR_texture_basisu"],
  "scene":0,
  "scenes":[{"nodes":[0]}],
  "nodes":[{"mesh":0}],
  "meshes":[{"primitives":[{
    "attributes":{"POSITION":0},"indices":1,"material":0
  }]}],
  "materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],
  "textures":[{"extensions":{"KHR_texture_basisu":{"source":0}}}],
  "images":[{"uri":"color.ktx2","mimeType":"image/ktx2"}],
  "buffers":[{"uri":"geometry.bin","byteLength":48}],
  "bufferViews":[
    {"buffer":0,"byteOffset":0,"byteLength":36},
    {"buffer":0,"byteOffset":36,"byteLength":12}
  ],
  "accessors":[
    {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",
     "min":[-0.5,0,0],"max":[0.5,1,0]},
    {"bufferView":1,"componentType":5125,"count":3,"type":"SCALAR"}
  ]
})");
    const auto imported = heartstead::assets::import_gltf_model(root / "basisu.gltf");
    assert(imported);
    assert_triangle(imported.value());
    assert(imported.value().images.size() == 1);
    assert(imported.value().images[0].width == 4);
    assert(imported.value().images[0].height == 4);
    assert(imported.value().materials[0].base_color_texture.image == 0);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    assert(!error);
}

} // namespace

int main() {
    test_meshopt_compressed_import();
    test_draco_compressed_import();
    test_basisu_ktx2_import();
    return 0;
}
