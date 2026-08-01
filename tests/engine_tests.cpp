#include "engine/assemblies/assembly.hpp"
#include "engine/assemblies/assembly_prototype.hpp"
#include "engine/assets/asset_catalog.hpp"
#include "engine/assets/asset_cooker.hpp"
#include "engine/assets/cooked_asset_manifest.hpp"
#include "engine/assets/cooked_asset_store.hpp"
#include "engine/assets/model_asset.hpp"
#include "engine/assets/resource_pack.hpp"
#include "engine/assets/texture_asset.hpp"
#include "engine/assets/virtual_file_system.hpp"
#include "engine/build/build_piece.hpp"
#include "engine/build/build_piece_prototype.hpp"
#include "engine/cargo/cargo.hpp"
#include "engine/cargo/cargo_prototype.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/core/hash.hpp"
#include "engine/core/ids.hpp"
#include "engine/debug/inspection.hpp"
#include "engine/dirty/dirty_region.hpp"
#include "engine/entities/entity.hpp"
#include "engine/entities/entity_prototype.hpp"
#include "engine/entities/physical_resource.hpp"
#include "engine/items/item_prototype.hpp"
#include "engine/items/item_stack.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/math/vector.hpp"
#include "engine/modding/flat_manifest.hpp"
#include "engine/modding/generic_prototype_loader.hpp"
#include "engine/modding/mod_discovery.hpp"
#include "engine/modding/mod_fingerprint.hpp"
#include "engine/modding/mod_validation.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/net/client_session.hpp"
#include "engine/net/command_payload.hpp"
#include "engine/net/host_session.hpp"
#include "engine/net/replication.hpp"
#include "engine/net/server_command.hpp"
#include "engine/net/transport.hpp"
#include "engine/net/transport_control.hpp"
#include "engine/net/transport_packet.hpp"
#include "engine/net/transport_reliability.hpp"
#include "engine/networks/network_derivation.hpp"
#include "engine/networks/spatial_network.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/platform/platform.hpp"
#include "engine/processes/process.hpp"
#include "engine/processes/process_environment.hpp"
#include "engine/processes/process_prototype.hpp"
#include "engine/renderer/materials/material_asset_validation.hpp"
#include "engine/renderer/materials/material_definition.hpp"
#include "engine/renderer/materials/material_pipeline_layout.hpp"
#include "engine/renderer/materials/material_prototype_loader.hpp"
#include "engine/renderer/materials/terrain_material_assets.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"
#include "engine/renderer/shaders/shader_compiler.hpp"
#include "engine/replay/command_replay.hpp"
#include "engine/rooms/room_descriptor_prototype.hpp"
#include "engine/rooms/room_extraction.hpp"
#include "engine/rooms/room_graph.hpp"
#include "engine/save/save_binary_codec.hpp"
#include "engine/save/save_compatibility.hpp"
#include "engine/save/save_database.hpp"
#include "engine/save/save_metadata.hpp"
#include "engine/save/save_migration.hpp"
#include "engine/save/save_slot.hpp"
#include "engine/save/save_snapshot.hpp"
#include "engine/save/save_text_codec.hpp"
#include "engine/scenarios/scenario_prototype.hpp"
#include "engine/scripting/script_host_event.hpp"
#include "engine/scripting/script_module_loader.hpp"
#include "engine/scripting/script_runtime.hpp"
#include "engine/simulation/simulation_lod.hpp"
#include "engine/workpieces/workpiece_codec.hpp"
#include "engine/workpieces/workpiece_definition.hpp"
#include "engine/workpieces/workpiece_grid.hpp"
#include "engine/workpieces/workpiece_prototype.hpp"
#include "engine/workpieces/workpiece_template.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/chunks/chunk_edit_delta_codec.hpp"
#include "engine/world/meshing/chunk_mesher.hpp"
#include "engine/world/operations/world_operation.hpp"
#include "engine/world/replication_delta.hpp"
#include "engine/world/replication_interest.hpp"
#include "engine/world/simulation_subjects.hpp"
#include "engine/world/streaming/chunk_streamer.hpp"
#include "engine/world/voxel_change.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "engine/world/voxels/voxel_surface_state.hpp"
#include "engine/world/world_commands.hpp"
#include "engine/world/world_snapshot.hpp"
#include "engine/world/world_state.hpp"
#include "engine/world/worldgen/terrain_generator.hpp"

#include <ktx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("failed to write " + path.string());
    }
    output << text;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("failed to write " + path.string());
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("failed to write bytes " + path.string());
    }
}

std::vector<std::uint8_t> minimal_wav_bytes() {
    return {
        'R',  'I',  'F',  'F',  0x26, 0x00, 0x00, 0x00, 'W',  'A',  'V',  'E',
        'f',  'm',  't',  ' ',  0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x40, 0x1F, 0x00, 0x00, 0x40, 0x1F, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00,
        'd',  'a',  't',  'a',  0x02, 0x00, 0x00, 0x00, 0x80, 0x81,
    };
}

std::vector<std::uint8_t> minimal_ogg_bytes() {
    std::vector<std::uint8_t> bytes{'O', 'g', 'g', 'S', 0x00, 0x02};
    bytes.insert(bytes.end(), 20, 0x00);
    bytes.push_back(0x01);
    bytes.push_back(0x1E);
    bytes.insert(bytes.end(), {0x01, 'v',  'o',  'r',  'b',  'i',  's',  0x00, 0x00, 0x00,
                               0x00, 0x01, 0x80, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB8, 0x01});
    return bytes;
}

std::vector<std::uint8_t> minimal_flac_bytes() {
    std::vector<std::uint8_t> bytes{'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, 0x22};
    bytes.insert(bytes.end(), 34, 0x00);
    return bytes;
}

std::vector<std::uint8_t> decode_base64_for_test(std::string_view encoded) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::uint8_t> decoded;
    std::uint32_t bits = 0;
    std::uint32_t bit_count = 0;
    for (const auto character : encoded) {
        if (character == '=') {
            break;
        }
        const auto value = alphabet.find(character);
        assert(value != std::string_view::npos);
        bits = (bits << 6U) | static_cast<std::uint32_t>(value);
        bit_count += 6U;
        if (bit_count >= 8U) {
            bit_count -= 8U;
            decoded.push_back(static_cast<std::uint8_t>((bits >> bit_count) & 0xFFU));
        }
    }
    return decoded;
}

std::vector<std::uint8_t> minimal_png_bytes() {
    return decode_base64_for_test(
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAG0lEQVR4nGPYF2Hw/90Wn/"
        "8M3+6t+x9jovwfAF7UCmZhHBP2AAAAAElFTkSuQmCC");
}

std::vector<std::uint8_t> minimal_jpeg_bytes() {
    return decode_base64_for_test(
        "/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsN"
        "DhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQU"
        "FBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wgARCAAJAAkDAREAAhEBAxEB/8QA"
        "FgABAQEAAAAAAAAAAAAAAAAAAAII/8QAFAEBAAAAAAAAAAAAAAAAAAAAAP/aAAwDAQACEAMQAAAB1MWA"
        "f//EABQQAQAAAAAAAAAAAAAAAAAAACD/2gAIAQEAAQUCH//EABQRAQAAAAAAAAAAAAAAAAAAACD/2gAI"
        "AQMBAT8BH//EABQRAQAAAAAAAAAAAAAAAAAAACD/2gAIAQIBAT8BH//EABUQAQEAAAAAAAAAAAAAAAAA"
        "ACDh/9oACAEBAAY/AqP/xAAXEAEAAwAAAAAAAAAAAAAAAAABICEx/9oACAEBAAE/IVAbMYP/2gAMAwEA"
        "AgADAAAAEBJP/8QAFBEBAAAAAAAAAAAAAAAAAAAAIP/aAAgBAwEBPxAf/8QAFBEBAAAAAAAAAAAAAAAA"
        "AAAAIP/aAAgBAgEBPxAf/8QAGBABAAMBAAAAAAAAAAAAAAAAASAhQfD/2gAIAQEAAT8QckAVVm9eQ//Z");
}

void append_le_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

std::vector<std::uint8_t> minimal_ktx2_bytes() {
    ktxTextureCreateInfo info{};
    info.vkFormat = 37;
    info.baseWidth = 1;
    info.baseHeight = 1;
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
    constexpr std::array<std::uint8_t, 4> pixel{255, 255, 255, 255};
    assert(ktxTexture_SetImageFromMemory(ktxTexture(texture.get()), 0, 0, 0, pixel.data(),
                                         pixel.size()) == KTX_SUCCESS);
    ktx_uint8_t* encoded = nullptr;
    ktx_size_t encoded_size = 0;
    assert(ktxTexture_WriteToMemory(ktxTexture(texture.get()), &encoded, &encoded_size) ==
           KTX_SUCCESS);
    assert(encoded != nullptr);
    std::vector<std::uint8_t> result(encoded, encoded + encoded_size);
    std::free(encoded);
    return result;
}

std::string minimal_gltf_text() {
    return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"triangle","mesh":0}],"meshes":[{"name":"triangle","primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"buffers":[{"uri":"data:application/octet-stream;base64,AAAAvwAAAAAAAAAAAAAAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA","byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-0.5,0,0],"max":[0.5,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]})";
}

std::string minimal_external_gltf_text() {
    auto text = minimal_gltf_text();
    const std::string embedded = "data:application/octet-stream;base64,"
                                 "AAAAvwAAAAAAAAAAAAAAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA";
    const auto offset = text.find(embedded);
    assert(offset != std::string::npos);
    text.replace(offset, embedded.size(), "triangle.bin");
    return text;
}

std::string minimal_external_textured_gltf_text(std::string_view alpha_mode = "OPAQUE") {
    auto text = minimal_external_gltf_text();
    const auto indices = text.find("\"indices\":1");
    assert(indices != std::string::npos);
    text.insert(indices + std::string_view{"\"indices\":1"}.size(), ",\"material\":0");
    const auto buffers = text.find("],\"buffers\"");
    assert(buffers != std::string::npos);
    text.insert(buffers + 1, ",\"materials\":[{\"name\":\"paint\",\"pbrMetallicRoughness\":{"
                             "\"baseColorFactor\":[0.25,0.5,0.75,0.8],"
                             "\"baseColorTexture\":{\"index\":0}},\"alphaMode\":\"" +
                                 std::string(alpha_mode) +
                                 "\",\"doubleSided\":true}],\"textures\":[{\"source\":0}],"
                                 "\"images\":[{\"name\":\"paint\",\"uri\":\"albedo.png\"}]");
    return text;
}

std::vector<std::uint8_t> minimal_triangle_buffer_bytes() {
    return {
        0x00, 0x00, 0x00, 0xbf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00,
    };
}

std::vector<std::uint8_t> one_pixel_png_bytes() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x04, 0x00, 0x00,
        0x00, 0xb5, 0x1c, 0x0c, 0x02, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54, 0x78,
        0xda, 0x63, 0x64, 0xf8, 0x0f, 0x00, 0x01, 0x05, 0x01, 0x01, 0x27, 0x18, 0xe3, 0x66,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

std::vector<std::uint8_t> minimal_glb_bytes() {
    std::string json = minimal_gltf_text();
    while (json.size() % 4U != 0) {
        json.push_back(' ');
    }

    std::vector<std::uint8_t> bytes;
    append_le_u32(bytes, 0x46546C67U);
    append_le_u32(bytes, 2);
    append_le_u32(bytes, static_cast<std::uint32_t>(12U + 8U + json.size()));
    append_le_u32(bytes, static_cast<std::uint32_t>(json.size()));
    append_le_u32(bytes, 0x4E4F534AU);
    bytes.insert(bytes.end(), json.begin(), json.end());
    return bytes;
}

std::vector<std::uint8_t> minimal_spirv_bytes() {
    const std::array<std::uint32_t, 5> words{
        0x07230203, 0x00010000, 0x00000000, 0x00000001, 0x00000000,
    };

    std::vector<std::uint8_t> bytes;
    bytes.reserve(words.size() * sizeof(std::uint32_t));
    for (const auto word : words) {
        bytes.push_back(static_cast<std::uint8_t>(word & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>((word >> 8U) & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>((word >> 16U) & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>((word >> 24U) & 0xFFU));
    }
    return bytes;
}

std::vector<std::uint8_t> minimal_sfnt_font_bytes() {
    return {
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00,
        0x00, 'n',  'a',  'm',  'e',  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x1C, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
    };
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool nearly_equal(float lhs, float rhs) {
    return std::abs(lhs - rhs) < 0.001F;
}

bool wait_for_completed_jobs(heartstead::jobs::IJobSystem& system, std::uint64_t expected_count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (system.completed_count() >= expected_count) {
            return true;
        }
        std::this_thread::yield();
    }
    return system.completed_count() >= expected_count;
}

std::filesystem::path make_temp_root() {
    const auto parent = std::filesystem::temp_directory_path() / "heartstead_engine_tests";
    std::filesystem::create_directories(parent);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 1024; ++attempt) {
        auto root = parent / (std::to_string(stamp) + "_" + std::to_string(attempt));
        if (std::filesystem::create_directory(root)) {
            return root;
        }
    }
    throw std::runtime_error("failed to create unique test temp root");
}

void test_prototype_ids() {
    const auto id = heartstead::core::PrototypeId::parse("base:items/raw_clay");
    assert(id.has_value());
    assert(id->namespace_id() == "base");
    assert(id->local_id() == "items/raw_clay");
    assert(!heartstead::core::PrototypeId::parse("Base:bad"));
    assert(!heartstead::core::PrototypeId::parse("base:../bad"));
}

void test_stable_hash64() {
    using heartstead::core::StableHash64;

    assert(heartstead::core::stable_hash64_hex("") == "14650fb0739d0383");
    assert(heartstead::core::stable_hash64_hex("Heartstead") == "b809929d61358b24");
    assert(heartstead::core::stable_hash64_hex("abc") == "e16801510db89efd");

    const std::vector<std::uint8_t> bytes{'a', 'b', 'c'};
    assert(heartstead::core::stable_hash64_hex(bytes) ==
           heartstead::core::stable_hash64_hex("abc"));

    StableHash64 hasher;
    hasher.add_u64_le(0x0807060504030201ULL);
    assert(hasher.hex() == "f2a57b68e1fd4d73");
    assert(hasher.nonzero_value() == hasher.value());
}

void test_flat_manifest_parser() {
    const auto root = make_temp_root();
    const auto manifest_path = root / "manifest.toml";
    write_text(manifest_path, "name = \"Hash # Value\" # trailing comment\n"
                              "escaped = \"quote: \\\"ok\\\" and slash \\\\ done\"\n"
                              "literal = 'literal # value'\n"
                              "enabled = true\n"
                              "count = 1\n");

    std::vector<heartstead::modding::ModDiagnostic> diagnostics;
    auto values = heartstead::modding::parse_flat_manifest(manifest_path, diagnostics,
                                                           {.diagnostic_prefix = "test.manifest"});
    assert(diagnostics.empty());
    assert(values.at("name") == "Hash # Value");
    assert(values.at("escaped") == "quote: \"ok\" and slash \\ done");
    assert(values.at("literal") == "literal # value");
    assert(values.at("enabled") == "true");
    assert(values.at("count") == "1");

    write_text(manifest_path, "id = \"first\"\nid = \"second\"\n");
    diagnostics.clear();
    values = heartstead::modding::parse_flat_manifest(manifest_path, diagnostics,
                                                      {.diagnostic_prefix = "test.manifest"});
    assert(values.at("id") == "first");
    assert(std::ranges::any_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "test.manifest.duplicate_key";
    }));

    write_text(manifest_path, "name = \"bad\\q\"\n");
    diagnostics.clear();
    values = heartstead::modding::parse_flat_manifest(manifest_path, diagnostics,
                                                      {.diagnostic_prefix = "test.manifest"});
    assert(values.empty());
    assert(std::ranges::any_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "test.manifest.syntax";
    }));

    diagnostics.clear();
    values = heartstead::modding::parse_flat_manifest(
        manifest_path, diagnostics, {.diagnostic_prefix = "test.manifest", .maximum_bytes = 4});
    assert(values.empty());
    assert(std::ranges::any_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "test.manifest.too_large";
    }));
}

void test_virtual_file_system() {
    const auto root = make_temp_root();
    write_text(root / "assets/textures/blocks/stone.txt", "stone texture placeholder");
    write_text(root / "assets/textures/items/base_only.txt", "base only texture placeholder");
    write_text(root / "assets/textures/items/raw_clay.txt", "raw clay texture placeholder");
    write_bytes(root / "assets/textures/items/raw_clay.bin", {0x00, 0x7f, 0x80, 0xff, 0x42});
    write_text(root / "assets/data/items/raw_clay.prototype", "id = base:items/raw_clay");
    write_text(root / "pack/textures/items/pack_only.txt", "pack only texture placeholder");
    write_text(root / "pack/textures/items/raw_clay.txt", "resource pack override");

    heartstead::assets::VirtualFileSystem vfs;
    auto mounted = vfs.mount("base", root / "assets");
    assert(mounted);
    mounted = vfs.mount("base", root / "pack");
    assert(mounted);
    assert(vfs.mounts().size() == 2);

    auto parsed = heartstead::assets::VirtualPath::parse("base:textures/items/raw_clay.txt");
    assert(parsed);
    assert(parsed.value().to_string() == "base:textures/items/raw_clay.txt");

    auto resolved = vfs.resolve_existing("base:textures/items/raw_clay.txt");
    assert(resolved);
    assert(resolved.value().filename() == "raw_clay.txt");
    assert(resolved.value().parent_path().filename() == "items");

    auto text = vfs.read_text("base:textures/items/raw_clay.txt");
    assert(text);
    assert(text.value() == "resource pack override");

    auto bytes = vfs.read_bytes("base:textures/items/raw_clay.bin");
    assert(bytes);
    assert(bytes.value().size() == 5);
    assert(bytes.value()[0] == 0x00);
    assert(bytes.value()[2] == 0x80);
    assert(bytes.value()[3] == 0xff);
    auto bounded_bytes = vfs.read_bytes("base:textures/items/raw_clay.bin", 4);
    assert(!bounded_bytes);
    assert(bounded_bytes.error().code == "vfs.file_too_large");
    auto bounded_text = vfs.read_text("base:textures/items/raw_clay.txt", 4);
    assert(!bounded_text);
    assert(bounded_text.error().code == "vfs.file_too_large");

    auto listed = vfs.list_files("base:textures");
    assert(listed);
    assert(listed.value().size() == 5);
    assert(listed.value()[0].virtual_path.to_string() == "base:textures/blocks/stone.txt");
    assert(listed.value()[1].virtual_path.to_string() == "base:textures/items/base_only.txt");
    assert(listed.value()[2].virtual_path.to_string() == "base:textures/items/pack_only.txt");
    assert(listed.value()[3].virtual_path.to_string() == "base:textures/items/raw_clay.bin");
    assert(listed.value()[4].virtual_path.to_string() == "base:textures/items/raw_clay.txt");

    const auto listed_raw_clay = std::ranges::find_if(listed.value(), [](const auto& entry) {
        return entry.virtual_path.to_string() == "base:textures/items/raw_clay.txt";
    });
    assert(listed_raw_clay != listed.value().end());
    assert(listed_raw_clay->mount_index == 1);
    assert(listed_raw_clay->resolved_path.parent_path().parent_path().filename() == "textures");

    const auto listed_raw_clay_binary = std::ranges::find_if(listed.value(), [](const auto& entry) {
        return entry.virtual_path.to_string() == "base:textures/items/raw_clay.bin";
    });
    assert(listed_raw_clay_binary != listed.value().end());
    assert(listed_raw_clay_binary->mount_index == 0);

    auto namespace_listed = vfs.list_namespace_files("base");
    assert(namespace_listed);
    assert(namespace_listed.value().size() == 6);
    assert(namespace_listed.value()[0].virtual_path.to_string() ==
           "base:data/items/raw_clay.prototype");

    auto entry_limited =
        vfs.list_namespace_files("base", {.maximum_entries = 1, .maximum_depth = 32});
    assert(!entry_limited);
    assert(entry_limited.error().code == "vfs.directory_too_large");
    auto depth_limited =
        vfs.list_namespace_files("base", {.maximum_entries = 128, .maximum_depth = 1});
    assert(!depth_limited);
    assert(depth_limited.error().code == "vfs.directory_too_deep");

    auto invalid_namespace_list = vfs.list_namespace_files("Base");
    assert(!invalid_namespace_list);
    assert(invalid_namespace_list.error().code == "vfs.invalid_namespace");

    auto missing_directory = vfs.list_files("base:missing");
    assert(missing_directory);
    assert(missing_directory.value().empty());

    auto missing_namespace = vfs.list_files("missing:textures");
    assert(!missing_namespace);
    assert(missing_namespace.error().code == "vfs.namespace_not_mounted");

    auto unsafe = heartstead::assets::VirtualPath::parse("base:../mod.toml");
    assert(!unsafe);
    auto unsafe_read = vfs.read_bytes("base:../mod.toml");
    assert(!unsafe_read);
    assert(unsafe_read.error().code == "vfs.unsafe_relative_path");
    auto unsafe_list = vfs.list_files("base:../textures");
    assert(!unsafe_list);
    assert(unsafe_list.error().code == "vfs.unsafe_relative_path");

    std::error_code link_error;
    std::filesystem::create_symlink(root / "pack/textures/items/raw_clay.txt",
                                    root / "pack/textures/items/linked.txt", link_error);
    if (!link_error) {
        auto linked_list = vfs.list_namespace_files("base");
        assert(!linked_list);
        assert(linked_list.error().code == "vfs.symlink_forbidden");
    }

    write_text(root / "outside.txt", "outside mount");
    std::error_code escape_link_error;
    std::filesystem::create_symlink(root / "outside.txt", root / "pack/textures/items/escaped.txt",
                                    escape_link_error);
    if (!escape_link_error) {
        const auto escaped_read = vfs.read_text("base:textures/items/escaped.txt");
        assert(!escaped_read);
        assert(escaped_read.error().code == "vfs.path_outside_root");
    }
}

void test_math_primitives() {
    using namespace heartstead::math;

    constexpr Vec3f x_axis{1.0F, 0.0F, 0.0F};
    constexpr Vec3f y_axis{0.0F, 1.0F, 0.0F};
    constexpr auto z_axis = cross(x_axis, y_axis);
    static_assert(z_axis.x == 0.0F);
    static_assert(z_axis.y == 0.0F);
    static_assert(z_axis.z == 1.0F);

    constexpr auto sum = Vec3f{1.0F, 2.0F, 3.0F} + Vec3f{4.0F, 5.0F, 6.0F};
    static_assert(sum.x == 5.0F);
    static_assert(sum.y == 7.0F);
    static_assert(sum.z == 9.0F);

    assert(dot(Vec3f{1.0F, 2.0F, 3.0F}, Vec3f{4.0F, 5.0F, 6.0F}) == 32.0F);
    assert(nearly_equal(static_cast<float>(length(Vec3f{0.0F, 3.0F, 4.0F})), 5.0F));
    const Vec3f finite_vector{1.0F, 2.0F, 3.0F};
    const Vec3f infinite_vector{1.0F, std::numeric_limits<float>::infinity(), 3.0F};
    assert(finite_vector.is_finite());
    assert(!infinite_vector.is_finite());

    Bounds3i bounds{{0, 0, 0}, {4, 4, 4}};
    assert(bounds.is_valid());
    assert(bounds.contains({2, 2, 2}));
    assert(!bounds.contains({5, 2, 2}));

    const auto expanded = bounds.expanded(1);
    assert(expanded.min.x == -1);
    assert(expanded.max.z == 5);

    const auto merged = bounds.merged_with(Bounds3i{{-2, 1, 1}, {2, 8, 2}});
    assert(merged.min.x == -2);
    assert(merged.max.y == 8);

    Transform3d transform;
    transform.position = {1.0, 2.0, 3.0};
    transform.rotation_degrees = {0.0, 90.0, 0.0};
    transform.scale = {1.0, 2.0, 3.0};
    assert(transform.is_finite());
    assert(transform.has_non_zero_scale());
    transform.scale.y = 0.0;
    assert(!transform.has_non_zero_scale());

    heartstead::build::Transform build_transform;
    build_transform.position = {1.0, 2.0, 3.0};
    assert(build_transform.position.approximate_global().x == 1.0);

    heartstead::physics::Vec3 physics_vector{1.0F, 2.0F, 3.0F};
    assert(physics_vector.is_finite());

    assert(axis3_name(Axis3::z) == "z");
}

void test_resource_pack_discovery_and_asset_catalog() {
    const auto root = make_temp_root();
    const auto mod_assets = root / "mods/base/assets";
    const auto pack_root = root / "resource_packs/hd_pack";
    const auto pack_assets = pack_root / "assets";
    const auto later_pack_root = root / "resource_packs/ultra_pack";
    const auto later_pack_assets = later_pack_root / "assets";

    write_text(mod_assets / "textures/items/raw_clay.txt", "mod texture");
    write_text(mod_assets / "models/building/wall.glb", "model placeholder");
    write_text(mod_assets / "shaders/wyrd_fog.slang", "shader placeholder");
    write_bytes(mod_assets / "sounds/tools/hammer.wav", minimal_wav_bytes());
    write_text(pack_root / "resource_pack.toml", "id = \"hd_pack\"\n"
                                                 "name = \"HD Pack\"\n"
                                                 "version = \"1.0.0\"\n"
                                                 "description = \"HD # Pack\"\n"
                                                 "shader_extensions = \"water\"\n");
    write_text(pack_assets / "textures/items/raw_clay.txt", "resource pack texture");
    write_text(pack_assets / "ui/debug_panel.txt", "debug ui");
    write_text(later_pack_root / "resource_pack.toml", "id = \"ultra_pack\"\n"
                                                       "name = \"Ultra Pack\"\n"
                                                       "version = \"1.0.0\"\n");
    write_text(later_pack_assets / "textures/items/raw_clay.txt", "ultra resource pack texture");

    heartstead::assets::ResourcePackDiscoverer discoverer;
    auto discovery = discoverer.discover(root / "resource_packs");
    assert(!discovery.has_errors());
    assert(discovery.packs.size() == 2);
    assert(discovery.packs.front().id == "hd_pack");
    assert(discovery.packs.front().description == "HD # Pack");
    assert(discovery.packs.front().shader_extensions.size() == 1);
    assert(discovery.packs.front().shader_extensions.front() ==
           heartstead::assets::ShaderExtensionPoint::water);
    assert(discovery.packs.back().id == "ultra_pack");

    const auto invalid_pack_root = root / "invalid_resource_packs/bad_pack";
    write_text(invalid_pack_root / "resource_pack.toml", "id = \"bad_pack\"\n"
                                                         "name = \"Bad Pack\"\n"
                                                         "name = \"Duplicate\"\n"
                                                         "version = \"1.0.0\"\n"
                                                         "gameplay = maybe\n"
                                                         "surprise = true\n");
    const auto invalid_discovery = discoverer.discover(root / "invalid_resource_packs");
    assert(invalid_discovery.has_errors());
    assert(invalid_discovery.packs.empty());
    assert(std::ranges::any_of(invalid_discovery.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "resource_pack.manifest.duplicate_key";
    }));
    assert(std::ranges::any_of(invalid_discovery.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "resource_pack.manifest.invalid_bool";
    }));
    assert(std::ranges::any_of(invalid_discovery.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "resource_pack.manifest.unknown_field";
    }));

    const auto unsafe_pack_root = root / "unsafe_resource_packs/unsafe_pack";
    write_text(unsafe_pack_root / "resource_pack.toml", "id = \"unsafe_pack\"\n"
                                                        "name = \"Unsafe Pack\"\n"
                                                        "version = \"1.0.0\"\n");
    write_text(unsafe_pack_root / "assets/data/rules.toml", "cheat = true\n");
    write_text(unsafe_pack_root / "assets/shaders/rogue.slang", "shader placeholder\n");
    const auto unsafe_discovery = discoverer.discover(root / "unsafe_resource_packs");
    assert(!unsafe_discovery.has_errors());
    assert(unsafe_discovery.packs.size() == 1);
    heartstead::assets::AssetCatalog unsafe_catalog;
    const auto unsafe_index = heartstead::assets::ResourcePackPolicy::index_assets(
        unsafe_catalog, unsafe_discovery.packs.front(), 1000);
    assert(unsafe_index.has_errors());
    assert(unsafe_catalog.record_count() == 0);
    assert(std::ranges::any_of(unsafe_index.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "resource_pack.non_presentation_asset_forbidden";
    }));
    assert(std::ranges::any_of(unsafe_index.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "resource_pack.unscoped_shader_forbidden";
    }));

    const auto linked_pack_root = root / "linked_resource_packs/linked_pack";
    const auto external_pack_manifest = root / "external_resource_pack.toml";
    write_text(external_pack_manifest, "id = \"linked_pack\"\n"
                                       "name = \"Linked Pack\"\n"
                                       "version = \"1\"\n");
    std::filesystem::create_directories(linked_pack_root);
    std::error_code pack_link_error;
    std::filesystem::create_symlink(external_pack_manifest, linked_pack_root / "resource_pack.toml",
                                    pack_link_error);
    if (!pack_link_error) {
        const auto linked_pack_discovery = discoverer.discover(root / "linked_resource_packs");
        assert(linked_pack_discovery.has_errors());
        assert(std::ranges::any_of(linked_pack_discovery.diagnostics, [](const auto& diagnostic) {
            return diagnostic.code == "resource_pack.manifest.unsafe_file";
        }));
    }

    auto pack_plan = heartstead::assets::ResourcePackLoadPlanner::plan(discovery.packs);
    assert(pack_plan);
    assert(pack_plan.value().size() == 2);
    assert(pack_plan.value().entries.front().manifest.id == "hd_pack");
    assert(pack_plan.value().entries.front().load_index == 0);
    assert(pack_plan.value().entries.front().asset_priority ==
           heartstead::assets::default_resource_pack_priority_base);
    assert(pack_plan.value().entries.back().manifest.id == "ultra_pack");
    assert(pack_plan.value().entries.back().load_index == 1);
    assert(pack_plan.value().entries.back().asset_priority ==
           heartstead::assets::default_resource_pack_priority_base +
               heartstead::assets::default_resource_pack_priority_step);
    assert(pack_plan.value().find("hd_pack") != nullptr);
    assert(pack_plan.value().find("missing") == nullptr);

    auto invalid_plan = heartstead::assets::ResourcePackLoadPlanner::plan(discovery.packs, 1000, 0);
    assert(!invalid_plan);
    assert(invalid_plan.error().code == "resource_pack_load_plan.invalid_priority_step");

    heartstead::assets::AssetCatalog catalog;
    auto mod_index = heartstead::assets::AssetCatalogBuilder::index_directory(
        catalog, mod_assets, "base", heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!mod_index.has_errors());
    for (const auto& entry : pack_plan.value().entries) {
        auto pack_index = heartstead::assets::ResourcePackPolicy::index_assets(
            catalog, entry.manifest, entry.asset_priority);
        assert(!pack_index.has_errors());
    }

    assert(catalog.record_count() == 7);
    assert(catalog.active_count() == 5);
    assert(catalog.records().size() == 7);
    assert(catalog.active_records().size() == 5);
    assert(catalog.count_kind(heartstead::assets::AssetKind::texture) == 3);
    assert(catalog.count_kind(heartstead::assets::AssetKind::model) == 1);
    assert(catalog.count_kind(heartstead::assets::AssetKind::shader) == 1);
    assert(catalog.count_kind(heartstead::assets::AssetKind::sound) == 1);

    const auto* active = catalog.find_active("base:textures/items/raw_clay.txt");
    assert(active != nullptr);
    assert(active->source_kind == heartstead::assets::AssetSourceKind::resource_pack);
    assert(active->source_id == "ultra_pack");
    assert(active->priority == heartstead::assets::default_resource_pack_priority_base +
                                   heartstead::assets::default_resource_pack_priority_step);
    assert(active->virtual_path.to_string() == "base:textures/items/raw_clay.txt");
    assert(!active->content_hash.empty());

    auto all_raw_clay = catalog.records_for("base:textures/items/raw_clay.txt");
    assert(all_raw_clay.size() == 3);

    heartstead::assets::VirtualFileSystem active_vfs;
    auto mounted = active_vfs.mount("base", mod_assets);
    assert(mounted);
    mounted = active_vfs.mount("base", pack_assets);
    assert(mounted);

    heartstead::assets::AssetCatalog active_overlay_catalog;
    auto active_overlay_index = heartstead::assets::AssetCatalogBuilder::index_virtual_namespace(
        active_overlay_catalog, active_vfs, "base", heartstead::assets::AssetSourceKind::mod,
        "active_overlay", 0);
    assert(!active_overlay_index.has_errors());
    assert(active_overlay_catalog.record_count() == 5);
    assert(active_overlay_catalog.active_count() == 5);
    const auto* active_overlay_texture =
        active_overlay_catalog.find_active("base:textures/items/raw_clay.txt");
    assert(active_overlay_texture != nullptr);
    assert(active_overlay_texture->virtual_path.to_string() == "base:textures/items/raw_clay.txt");
    assert(active_overlay_texture->source_path.string().find("resource_packs") !=
           std::string::npos);

    heartstead::assets::AssetCatalog active_texture_catalog;
    auto active_texture_index = heartstead::assets::AssetCatalogBuilder::index_virtual_directory(
        active_texture_catalog, active_vfs, "base:textures",
        heartstead::assets::AssetSourceKind::mod, "active_textures", 0);
    assert(!active_texture_index.has_errors());
    assert(active_texture_catalog.record_count() == 1);
    assert(active_texture_catalog.count_kind(heartstead::assets::AssetKind::texture) == 1);

    auto missing_vfs_index = heartstead::assets::AssetCatalogBuilder::index_virtual_namespace(
        active_texture_catalog, active_vfs, "missing", heartstead::assets::AssetSourceKind::mod,
        "missing", 0);
    assert(missing_vfs_index.has_errors());
    assert(missing_vfs_index.diagnostics.front().code == "vfs.namespace_not_mounted");

    heartstead::assets::AssetCatalog bounded_catalog;
    auto bounded_index = heartstead::assets::AssetCatalogBuilder::index_directory(
        bounded_catalog, mod_assets, "base", heartstead::assets::AssetSourceKind::mod, "base", 0,
        4);
    assert(bounded_index.has_errors());
    assert(std::ranges::any_of(bounded_index.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "asset_catalog.file_too_large";
    }));
    assert(bounded_catalog.record_count() == 0);

    assert(heartstead::assets::infer_asset_kind("models/building/wall.glb") ==
           heartstead::assets::AssetKind::model);
    assert(heartstead::assets::infer_asset_kind("sounds/tools/hammer.wav") ==
           heartstead::assets::AssetKind::sound);
    assert(heartstead::assets::infer_asset_kind("sounds/tools/hammer.ogg") ==
           heartstead::assets::AssetKind::sound);
    assert(heartstead::assets::infer_asset_kind("music/theme.wav") ==
           heartstead::assets::AssetKind::music);
    assert(heartstead::assets::infer_asset_kind("music/theme.flac") ==
           heartstead::assets::AssetKind::music);
    assert(heartstead::assets::infer_asset_kind("loose.wav") ==
           heartstead::assets::AssetKind::sound);
    assert(heartstead::assets::infer_asset_kind("loose.ogg") ==
           heartstead::assets::AssetKind::music);
    const auto dev_cook_backend = heartstead::assets::asset_cook_backend_info(
        heartstead::assets::AssetCookBackend::development_passthrough);
    assert(dev_cook_backend.available);
    assert(dev_cook_backend.name == "development_passthrough");
    const auto production_cook_backend = heartstead::assets::asset_cook_backend_info(
        heartstead::assets::AssetCookBackend::production_converters);
    assert(production_cook_backend.available);
    assert(production_cook_backend.name == "production_converters");
    const auto production_texture_pipeline = heartstead::assets::asset_cook_pipeline_info(
        heartstead::assets::AssetKind::texture,
        heartstead::assets::AssetCookBackend::production_converters);
    assert(production_texture_pipeline.available);
    assert(production_texture_pipeline.converts_source_format);
    assert(production_texture_pipeline.name == "texture_gpu_runtime_converter_v3");
    const auto production_model_pipeline = heartstead::assets::asset_cook_pipeline_info(
        heartstead::assets::AssetKind::model,
        heartstead::assets::AssetCookBackend::production_converters);
    assert(production_model_pipeline.available);
    assert(production_model_pipeline.converts_source_format);
    assert(production_model_pipeline.name == "model_gltf_runtime_converter_v5");
    const auto production_shader_pipeline = heartstead::assets::asset_cook_pipeline_info(
        heartstead::assets::AssetKind::shader,
        heartstead::assets::AssetCookBackend::production_converters);
    assert(production_shader_pipeline.available);
    assert(production_shader_pipeline.converts_source_format);
    assert(production_shader_pipeline.name == "shader_spirv_runtime_passthrough_v1");
    const auto production_material_pipeline = heartstead::assets::asset_cook_pipeline_info(
        heartstead::assets::AssetKind::material,
        heartstead::assets::AssetCookBackend::production_converters);
    assert(production_material_pipeline.available);
    assert(!production_material_pipeline.converts_source_format);
    assert(production_material_pipeline.name == "material_runtime_converter_v1");
    const auto production_sound_pipeline = heartstead::assets::asset_cook_pipeline_info(
        heartstead::assets::AssetKind::sound,
        heartstead::assets::AssetCookBackend::production_converters);
    assert(production_sound_pipeline.available);
    assert(production_sound_pipeline.converts_source_format);
    assert(production_sound_pipeline.name == "audio_runtime_converter_v3");
    const auto production_music_pipeline = heartstead::assets::asset_cook_pipeline_info(
        heartstead::assets::AssetKind::music,
        heartstead::assets::AssetCookBackend::production_converters);
    assert(production_music_pipeline.available);
    assert(production_music_pipeline.converts_source_format);
    assert(production_music_pipeline.name == "audio_runtime_converter_v3");
    const auto production_font_pipeline = heartstead::assets::asset_cook_pipeline_info(
        heartstead::assets::AssetKind::font,
        heartstead::assets::AssetCookBackend::production_converters);
    assert(production_font_pipeline.available);
    assert(production_font_pipeline.converts_source_format);
    assert(production_font_pipeline.name == "font_runtime_converter_v1");
    const auto production_data_pipeline = heartstead::assets::asset_cook_pipeline_info(
        heartstead::assets::AssetKind::data,
        heartstead::assets::AssetCookBackend::production_converters);
    assert(production_data_pipeline.available);
    assert(!production_data_pipeline.converts_source_format);
    assert(production_data_pipeline.name == "data_runtime_converter_v1");

    auto cooked = heartstead::assets::CookedAssetManifestBuilder::build(catalog);
    assert(cooked);
    assert(cooked.value().records.size() == 5);
    assert(cooked.value().active_count() == 5);
    assert(cooked.value().count_kind(heartstead::assets::AssetKind::texture) == 1);
    assert(cooked.value().count_kind(heartstead::assets::AssetKind::model) == 1);
    assert(cooked.value().count_kind(heartstead::assets::AssetKind::shader) == 1);
    assert(cooked.value().count_kind(heartstead::assets::AssetKind::sound) == 1);
    const auto* cooked_raw_clay = cooked.value().find("base:textures/items/raw_clay.txt");
    assert(cooked_raw_clay != nullptr);
    assert(cooked_raw_clay->source_id == "ultra_pack");
    assert(cooked_raw_clay->source_kind == heartstead::assets::AssetSourceKind::resource_pack);
    assert(cooked_raw_clay->active);
    assert(cooked_raw_clay->cooked_relative_path.generic_string() ==
           "ultra_pack/development/texture/base/textures/items/raw_clay.txt.cooked");
    assert(!cooked_raw_clay->cooked_hash.empty());
    assert(cooked.value().validate());

    const auto encoded = heartstead::assets::CookedAssetManifestTextCodec::encode(cooked.value());
    auto decoded = heartstead::assets::CookedAssetManifestTextCodec::decode(encoded);
    assert(decoded);
    assert(!heartstead::assets::CookedAssetManifestTextCodec::decode(encoded + "trailing=1\n"));
    assert(decoded.value().records.size() == 5);
    assert(decoded.value().find("base:textures/items/raw_clay.txt") != nullptr);
    assert(decoded.value().find("base:textures/items/raw_clay.txt")->source_id == "ultra_pack");

    auto all_cooked = heartstead::assets::CookedAssetManifestBuilder::build(
        catalog, heartstead::assets::CookedAssetBuildConfig{"development", false, 1});
    assert(all_cooked);
    assert(all_cooked.value().records.size() == 7);
    assert(all_cooked.value().active_count() == 5);
    assert(all_cooked.value().records_for("base:textures/items/raw_clay.txt").size() == 3);

    const auto dependency_assets = root / "dependency_assets";
    write_text(dependency_assets / "textures/items/clay.txt", "dependency texture");
    write_text(dependency_assets / "materials/clay.mat", "dependency material");
    auto dependency_texture_path =
        heartstead::assets::VirtualPath::parse("base:textures/items/clay.txt");
    auto dependency_material_path =
        heartstead::assets::VirtualPath::parse("base:materials/clay.mat");
    assert(dependency_texture_path);
    assert(dependency_material_path);

    heartstead::assets::AssetCatalog dependency_catalog;
    assert(dependency_catalog.add(heartstead::assets::AssetRecord{
        "base:textures/items/clay.txt",
        heartstead::assets::AssetKind::texture,
        dependency_texture_path.value(),
        heartstead::assets::AssetSourceKind::mod,
        "base",
        0,
        dependency_assets / "textures/items/clay.txt",
        heartstead::core::stable_hash64_hex("texture_hash"),
        false,
        {},
    }));
    assert(dependency_catalog.add(heartstead::assets::AssetRecord{
        "base:materials/clay.mat",
        heartstead::assets::AssetKind::material,
        dependency_material_path.value(),
        heartstead::assets::AssetSourceKind::mod,
        "base",
        0,
        dependency_assets / "materials/clay.mat",
        heartstead::core::stable_hash64_hex("material_hash"),
        false,
        {dependency_texture_path.value()},
    }));

    auto dependency_manifest =
        heartstead::assets::CookedAssetManifestBuilder::build(dependency_catalog);
    assert(dependency_manifest);
    assert(dependency_manifest.value().find_active("base:textures/items/clay.txt") != nullptr);
    const auto* material_record =
        dependency_manifest.value().find_active("base:materials/clay.mat");
    assert(material_record != nullptr);
    assert(material_record->dependencies.size() == 1);
    auto dependency_report = dependency_manifest.value().dependency_report();
    assert(!dependency_report.has_errors());
    assert(dependency_manifest.value().validate_dependencies());

    const auto encoded_dependency_manifest =
        heartstead::assets::CookedAssetManifestTextCodec::encode(dependency_manifest.value());
    auto decoded_dependency_manifest =
        heartstead::assets::CookedAssetManifestTextCodec::decode(encoded_dependency_manifest);
    assert(decoded_dependency_manifest);
    assert(decoded_dependency_manifest.value()
               .find_active("base:materials/clay.mat")
               ->dependencies.front()
               .to_string() == "base:textures/items/clay.txt");

    heartstead::assets::AssetCookConfig dependency_cook_config;
    dependency_cook_config.output_root = root / "dependency_cooked_assets";
    auto dependency_cook_result =
        heartstead::assets::AssetCooker::cook(dependency_catalog, dependency_cook_config);
    assert(dependency_cook_result);
    assert(dependency_cook_result.value().cooked_file_count == 2);
    assert(dependency_cook_result.value().manifest.schema_version == 2);
    assert(dependency_cook_result.value()
               .manifest.find_active("base:materials/clay.mat")
               ->dependencies.size() == 1);
    const auto* dependency_material =
        dependency_cook_result.value().manifest.find_active("base:materials/clay.mat");
    assert(dependency_material != nullptr);
    assert(dependency_material->dependency_hash != "0000000000000000");
    assert(dependency_material->pipeline_id != "unassigned");

    auto unchanged_dependency_cook =
        heartstead::assets::AssetCooker::cook(dependency_catalog, dependency_cook_config);
    assert(unchanged_dependency_cook);
    assert(unchanged_dependency_cook.value().cooked_file_count == 0);
    assert(unchanged_dependency_cook.value().reused_file_count == 2);
    assert(unchanged_dependency_cook.value().invalidated_file_count == 0);

    heartstead::assets::AssetCatalog changed_dependency_catalog;
    assert(changed_dependency_catalog.add(heartstead::assets::AssetRecord{
        "base:textures/items/clay.txt",
        heartstead::assets::AssetKind::texture,
        dependency_texture_path.value(),
        heartstead::assets::AssetSourceKind::mod,
        "base",
        0,
        dependency_assets / "textures/items/clay.txt",
        heartstead::core::stable_hash64_hex("texture_hash_changed"),
        false,
        {},
    }));
    assert(changed_dependency_catalog.add(heartstead::assets::AssetRecord{
        "base:materials/clay.mat",
        heartstead::assets::AssetKind::material,
        dependency_material_path.value(),
        heartstead::assets::AssetSourceKind::mod,
        "base",
        0,
        dependency_assets / "materials/clay.mat",
        heartstead::core::stable_hash64_hex("material_hash"),
        false,
        {dependency_texture_path.value()},
    }));
    auto invalidated_dependency_cook =
        heartstead::assets::AssetCooker::cook(changed_dependency_catalog, dependency_cook_config);
    assert(invalidated_dependency_cook);
    assert(invalidated_dependency_cook.value().cooked_file_count == 2);
    assert(invalidated_dependency_cook.value().reused_file_count == 0);
    assert(invalidated_dependency_cook.value().invalidated_file_count == 2);

    heartstead::assets::AssetCatalog missing_dependency_catalog;
    assert(missing_dependency_catalog.add(heartstead::assets::AssetRecord{
        "base:materials/missing.mat",
        heartstead::assets::AssetKind::material,
        heartstead::assets::VirtualPath::parse("base:materials/missing.mat").value(),
        heartstead::assets::AssetSourceKind::mod,
        "base",
        0,
        dependency_assets / "materials/clay.mat",
        heartstead::core::stable_hash64_hex("missing_material_hash"),
        false,
        {heartstead::assets::VirtualPath::parse("base:textures/items/missing.txt").value()},
    }));
    auto missing_dependency_manifest =
        heartstead::assets::CookedAssetManifestBuilder::build(missing_dependency_catalog);
    assert(!missing_dependency_manifest);
    assert(missing_dependency_manifest.error().code == "cooked_asset_manifest.missing_dependency");

    const auto cooked_output = root / "cooked_assets";
    heartstead::assets::AssetCookConfig cook_config;
    cook_config.output_root = cooked_output;
    assert(heartstead::assets::validate_asset_cook_config(cook_config));
    heartstead::assets::AssetCookConfig bad_cook_config = cook_config;
    bad_cook_config.output_root.clear();
    assert(!heartstead::assets::validate_asset_cook_config(bad_cook_config));
    bad_cook_config = cook_config;
    bad_cook_config.manifest_relative_path = "/absolute_manifest.txt";
    assert(!heartstead::assets::validate_asset_cook_config(bad_cook_config));
    bad_cook_config = cook_config;
    bad_cook_config.maximum_source_bytes = 0;
    assert(!heartstead::assets::validate_asset_cook_config(bad_cook_config));
    auto bounded_cook_config = cook_config;
    bounded_cook_config.output_root = root / "bounded_cooked_assets";
    bounded_cook_config.maximum_source_bytes = 4;
    auto bounded_cook =
        heartstead::assets::AssetCooker::cook(catalog, std::move(bounded_cook_config));
    assert(!bounded_cook && bounded_cook.error().code == "asset_cooker.source_too_large");
    auto production_cook_config = cook_config;
    production_cook_config.backend = heartstead::assets::AssetCookBackend::production_converters;
    auto invalid_production_cook =
        heartstead::assets::AssetCooker::cook(catalog, production_cook_config);
    assert(!invalid_production_cook);
    assert(invalid_production_cook.error().code == "asset_cooker.invalid_texture" ||
           invalid_production_cook.error().code == "asset_cooker.invalid_model" ||
           invalid_production_cook.error().code ==
               "shader_compiler.production_compiler_unavailable");

    const auto production_texture_assets = root / "production_texture_assets";
    write_bytes(production_texture_assets / "textures/items/raw_clay.png", minimal_png_bytes());
    write_bytes(production_texture_assets / "textures/voxels/clay.ktx2", minimal_ktx2_bytes());
    write_bytes(production_texture_assets / "textures/ui/settlement_icon.jpeg",
                minimal_jpeg_bytes());
    heartstead::assets::AssetCatalog production_texture_catalog;
    auto production_texture_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        production_texture_catalog, production_texture_assets, "base",
        heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!production_texture_indexed.has_errors());
    assert(production_texture_catalog.active_count() == 3);
    assert(production_texture_catalog.count_kind(heartstead::assets::AssetKind::texture) == 3);

    heartstead::assets::AssetCookConfig production_texture_cook_config;
    production_texture_cook_config.backend =
        heartstead::assets::AssetCookBackend::production_converters;
    production_texture_cook_config.output_root = root / "production_texture_cooked_assets";
    auto production_texture_cook = heartstead::assets::AssetCooker::cook(
        production_texture_catalog, production_texture_cook_config);
    if (!production_texture_cook) {
        throw std::runtime_error(production_texture_cook.error().code + ": " +
                                 production_texture_cook.error().message);
    }
    assert(production_texture_cook.value().manifest.profile == "production");
    assert(production_texture_cook.value().cooked_file_count == 3);
    const auto* production_png_record =
        production_texture_cook.value().manifest.find("base:textures/items/raw_clay.png");
    const auto* production_ktx2_record =
        production_texture_cook.value().manifest.find("base:textures/voxels/clay.ktx2");
    const auto* production_jpeg_record =
        production_texture_cook.value().manifest.find("base:textures/ui/settlement_icon.jpeg");
    assert(production_png_record != nullptr);
    assert(production_ktx2_record != nullptr);
    assert(production_jpeg_record != nullptr);
    assert(production_png_record->kind == heartstead::assets::AssetKind::texture);
    assert(production_ktx2_record->kind == heartstead::assets::AssetKind::texture);
    assert(production_jpeg_record->kind == heartstead::assets::AssetKind::texture);
    assert(read_text(production_texture_cook_config.output_root /
                     production_png_record->cooked_relative_path)
               .find("backend=texture_gpu_runtime_converter_v3") != std::string::npos);
    assert(read_text(production_texture_cook_config.output_root /
                     production_png_record->cooked_relative_path)
               .find("meta.texture.runtime_format=heartstead.texture.v2") != std::string::npos);
    auto production_texture_store =
        heartstead::assets::CookedAssetStore::load(production_texture_cook_config.output_root);
    assert(production_texture_store);
    auto production_png_payload =
        production_texture_store.value().load_payload("base:textures/items/raw_clay.png");
    assert(production_png_payload);
    assert(production_png_payload.value().kind == heartstead::assets::AssetKind::texture);
    assert(production_png_payload.value().backend == "texture_gpu_runtime_converter_v3");
    assert(production_png_payload.value().profile == "production");
    assert(production_png_payload.value().metadata.at("texture.source_container") == "png");
    assert(production_png_payload.value().metadata.at("texture.container") == "heartstead_texture");
    assert(production_png_payload.value().metadata.at("texture.runtime_format") ==
           "heartstead.texture.v2");
    assert(production_png_payload.value().metadata.at("texture.width") == "2");
    assert(production_png_payload.value().metadata.at("texture.height") == "2");
    assert(production_png_payload.value().metadata.at("texture.mip_levels") == "2");
    assert(production_png_payload.value().metadata.at("texture.color_space") == "srgb");
    assert(production_png_payload.value().metadata.at("texture.gpu_format") == "bc7_rgba");
    assert(production_png_payload.value().metadata.at("texture.color_type") == "6");
    auto production_png_texture =
        heartstead::assets::decode_texture_asset(production_png_payload.value().bytes);
    assert(production_png_texture);
    assert(production_png_texture.value().gpu_memory_bytes() == 32);
    auto production_ktx2_payload =
        production_texture_store.value().load_payload("base:textures/voxels/clay.ktx2");
    assert(production_ktx2_payload);
    assert(production_ktx2_payload.value().metadata.at("texture.source_container") == "ktx2");
    assert(production_ktx2_payload.value().metadata.at("texture.runtime_format") ==
           "heartstead.texture.v2");
    auto production_ktx2_texture =
        heartstead::assets::decode_texture_asset(production_ktx2_payload.value().bytes);
    assert(production_ktx2_texture);
    assert(production_ktx2_texture.value().format == heartstead::assets::TextureAssetFormat::rgba8);
    auto production_jpeg_payload =
        production_texture_store.value().load_payload("base:textures/ui/settlement_icon.jpeg");
    assert(production_jpeg_payload);
    assert(production_jpeg_payload.value().kind == heartstead::assets::AssetKind::texture);
    assert(production_jpeg_payload.value().backend == "texture_gpu_runtime_converter_v3");
    assert(production_jpeg_payload.value().metadata.at("texture.source_container") == "jpeg");
    assert(production_jpeg_payload.value().metadata.at("texture.container") ==
           "heartstead_texture");
    assert(production_jpeg_payload.value().metadata.at("texture.runtime_format") ==
           "heartstead.texture.v2");
    assert(production_jpeg_payload.value().metadata.at("texture.width") == "9");
    assert(production_jpeg_payload.value().metadata.at("texture.height") == "9");
    assert(production_jpeg_payload.value().metadata.at("texture.mip_levels") == "4");
    assert(production_jpeg_payload.value().metadata.at("texture.role") == "ui");
    assert(production_jpeg_payload.value().metadata.at("texture.component_count") == "3");
    auto production_jpeg_texture =
        heartstead::assets::decode_texture_asset(production_jpeg_payload.value().bytes);
    assert(production_jpeg_texture);
    assert(production_jpeg_texture.value().gpu_memory_bytes() == 192);

    const auto invalid_texture_assets = root / "invalid_texture_assets";
    write_text(invalid_texture_assets / "textures/bad.png", "not a png");
    heartstead::assets::AssetCatalog invalid_texture_catalog;
    auto invalid_texture_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        invalid_texture_catalog, invalid_texture_assets, "base",
        heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!invalid_texture_indexed.has_errors());
    heartstead::assets::AssetCookConfig invalid_texture_cook_config;
    invalid_texture_cook_config.backend =
        heartstead::assets::AssetCookBackend::production_converters;
    invalid_texture_cook_config.output_root = root / "invalid_texture_cooked_assets";
    auto invalid_texture_cook =
        heartstead::assets::AssetCooker::cook(invalid_texture_catalog, invalid_texture_cook_config);
    assert(!invalid_texture_cook);
    assert(invalid_texture_cook.error().code == "asset_cooker.invalid_texture");

    const auto invalid_jpeg_assets = root / "invalid_jpeg_assets";
    write_text(invalid_jpeg_assets / "textures/bad.jpg", "not a jpeg");
    heartstead::assets::AssetCatalog invalid_jpeg_catalog;
    auto invalid_jpeg_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        invalid_jpeg_catalog, invalid_jpeg_assets, "base", heartstead::assets::AssetSourceKind::mod,
        "base", 0);
    assert(!invalid_jpeg_indexed.has_errors());
    heartstead::assets::AssetCookConfig invalid_jpeg_cook_config;
    invalid_jpeg_cook_config.backend = heartstead::assets::AssetCookBackend::production_converters;
    invalid_jpeg_cook_config.output_root = root / "invalid_jpeg_cooked_assets";
    auto invalid_jpeg_cook =
        heartstead::assets::AssetCooker::cook(invalid_jpeg_catalog, invalid_jpeg_cook_config);
    assert(!invalid_jpeg_cook);
    assert(invalid_jpeg_cook.error().code == "asset_cooker.invalid_texture");

    const auto production_model_assets = root / "production_model_assets";
    write_text(production_model_assets / "models/building/wall.gltf", minimal_external_gltf_text());
    write_bytes(production_model_assets / "models/building/triangle.bin",
                minimal_triangle_buffer_bytes());
    write_bytes(production_model_assets / "models/building/gate.glb", minimal_glb_bytes());
    heartstead::assets::AssetCatalog production_model_catalog;
    auto production_model_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        production_model_catalog, production_model_assets, "base",
        heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!production_model_indexed.has_errors());
    assert(production_model_catalog.active_count() == 3);
    assert(production_model_catalog.count_kind(heartstead::assets::AssetKind::model) == 2);

    heartstead::assets::AssetCookConfig production_model_cook_config;
    production_model_cook_config.backend =
        heartstead::assets::AssetCookBackend::production_converters;
    production_model_cook_config.output_root = root / "production_model_cooked_assets";
    auto production_model_cook = heartstead::assets::AssetCooker::cook(
        production_model_catalog, production_model_cook_config);
    assert(production_model_cook);
    assert(production_model_cook.value().manifest.profile == "production");
    assert(production_model_cook.value().cooked_file_count == 3);
    const auto* production_gltf_record =
        production_model_cook.value().manifest.find("base:models/building/wall.gltf");
    const auto* production_glb_record =
        production_model_cook.value().manifest.find("base:models/building/gate.glb");
    assert(production_gltf_record != nullptr);
    assert(production_glb_record != nullptr);
    assert(production_gltf_record->dependencies.size() == 1);
    assert(production_gltf_record->dependencies.front().to_string() ==
           "base:models/building/triangle.bin");
    assert(production_gltf_record->kind == heartstead::assets::AssetKind::model);
    assert(production_glb_record->kind == heartstead::assets::AssetKind::model);
    assert(read_text(production_model_cook_config.output_root /
                     production_gltf_record->cooked_relative_path)
               .find("backend=model_gltf_runtime_converter_v5") != std::string::npos);
    auto production_model_store =
        heartstead::assets::CookedAssetStore::load(production_model_cook_config.output_root);
    assert(production_model_store);
    auto production_gltf_payload =
        production_model_store.value().load_payload("base:models/building/wall.gltf");
    assert(production_gltf_payload);
    assert(production_gltf_payload.value().kind == heartstead::assets::AssetKind::model);
    assert(production_gltf_payload.value().backend == "model_gltf_runtime_converter_v5");
    assert(production_gltf_payload.value().profile == "production");
    assert(production_gltf_payload.value().metadata.at("model.container") == "gltf");
    assert(production_gltf_payload.value().metadata.at("model.gltf_version") == "2.0");
    assert(production_gltf_payload.value().metadata.at("model.runtime_format") ==
           "heartstead.model.v5");
    assert(production_gltf_payload.value().metadata.at("model.vertices") == "3");
    assert(production_gltf_payload.value().metadata.at("model.indices") == "3");
    assert(production_gltf_payload.value().metadata.at("model.nodes") == "1");
    assert(production_gltf_payload.value().metadata.at("model.primitives") == "1");
    assert(production_gltf_payload.value().metadata.at("model.unlit_materials") == "0");
    assert(production_gltf_payload.value().metadata.at("model.skins") == "0");
    assert(production_gltf_payload.value().metadata.at("model.animations") == "0");
    auto decoded_production_gltf =
        heartstead::assets::decode_model_asset(production_gltf_payload.value().bytes);
    assert(decoded_production_gltf);
    assert(decoded_production_gltf.value().vertices.size() == 3);
    assert(decoded_production_gltf.value().indices == std::vector<std::uint32_t>({0, 1, 2}));
    auto production_glb_payload =
        production_model_store.value().load_payload("base:models/building/gate.glb");
    assert(production_glb_payload);
    assert(production_glb_payload.value().metadata.at("model.container") == "glb");
    assert(production_glb_payload.value().metadata.at("model.chunk_count") == "1");
    auto decoded_production_glb =
        heartstead::assets::decode_model_asset(production_glb_payload.value().bytes);
    assert(decoded_production_glb);
    assert(decoded_production_glb.value() == decoded_production_gltf.value());

    const auto invalid_model_assets = root / "invalid_model_assets";
    write_text(invalid_model_assets / "models/bad.glb", "not a glb");
    heartstead::assets::AssetCatalog invalid_model_catalog;
    auto invalid_model_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        invalid_model_catalog, invalid_model_assets, "base",
        heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!invalid_model_indexed.has_errors());
    heartstead::assets::AssetCookConfig invalid_model_cook_config;
    invalid_model_cook_config.backend = heartstead::assets::AssetCookBackend::production_converters;
    invalid_model_cook_config.output_root = root / "invalid_model_cooked_assets";
    auto invalid_model_cook =
        heartstead::assets::AssetCooker::cook(invalid_model_catalog, invalid_model_cook_config);
    assert(!invalid_model_cook);
    assert(invalid_model_cook.error().code == "asset_cooker.invalid_model");

    const auto production_assets = root / "production_assets";
    write_text(production_assets / "data/settings.toml", "schema = 1\n");
    write_text(production_assets / "locale/en_us.toml", "hello = \"Heartstead\"\n");
    write_text(production_assets / "materials/clay.mat", "shader = \"base:shaders/clay\"\n");
    write_text(production_assets / "ui/hud.json", "{\"root\":\"hud\"}\n");
    heartstead::assets::AssetCatalog production_catalog;
    auto production_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        production_catalog, production_assets, "base", heartstead::assets::AssetSourceKind::mod,
        "base", 0);
    assert(!production_indexed.has_errors());
    assert(production_catalog.active_count() == 4);

    heartstead::assets::AssetCookConfig production_data_cook_config;
    production_data_cook_config.backend =
        heartstead::assets::AssetCookBackend::production_converters;
    production_data_cook_config.output_root = root / "production_cooked_assets";
    auto production_data_cook =
        heartstead::assets::AssetCooker::cook(production_catalog, production_data_cook_config);
    assert(production_data_cook);
    assert(production_data_cook.value().backend ==
           heartstead::assets::AssetCookBackend::production_converters);
    assert(production_data_cook.value().manifest.profile == "production");
    assert(production_data_cook.value().cooked_file_count == 4);
    const auto* production_data_record =
        production_data_cook.value().manifest.find("base:data/settings.toml");
    assert(production_data_record != nullptr);
    assert(production_data_record->cooked_relative_path.generic_string() ==
           "base/production/data/base/data/settings.toml.cooked");
    assert(read_text(production_data_cook_config.output_root /
                     production_data_record->cooked_relative_path)
               .find("backend=data_runtime_converter_v1") != std::string::npos);
    const auto* production_locale_record =
        production_data_cook.value().manifest.find("base:locale/en_us.toml");
    assert(production_locale_record != nullptr);
    assert(read_text(production_data_cook_config.output_root /
                     production_locale_record->cooked_relative_path)
               .find("backend=localization_runtime_converter_v1") != std::string::npos);
    const auto* production_material_record =
        production_data_cook.value().manifest.find("base:materials/clay.mat");
    assert(production_material_record != nullptr);
    assert(production_material_record->kind == heartstead::assets::AssetKind::material);
    assert(read_text(production_data_cook_config.output_root /
                     production_material_record->cooked_relative_path)
               .find("backend=material_runtime_converter_v1") != std::string::npos);
    assert(read_text(production_data_cook_config.output_root /
                     production_material_record->cooked_relative_path)
               .find("profile=production") != std::string::npos);
    auto production_store =
        heartstead::assets::CookedAssetStore::load(production_data_cook_config.output_root);
    assert(production_store);
    auto production_material_payload =
        production_store.value().load_payload("base:materials/clay.mat");
    assert(production_material_payload);
    assert(production_material_payload.value().kind == heartstead::assets::AssetKind::material);
    assert(production_material_payload.value().backend == "material_runtime_converter_v1");
    assert(production_material_payload.value().profile == "production");
    assert(std::string(production_material_payload.value().bytes.begin(),
                       production_material_payload.value().bytes.end())
               .find("shader = \"base:shaders/clay\"") != std::string::npos);

    const auto production_audio_assets = root / "production_audio_assets";
    write_bytes(production_audio_assets / "sounds/tools/hammer.wav", minimal_wav_bytes());
    write_bytes(production_audio_assets / "sounds/tools/impact.ogg", minimal_ogg_bytes());
    write_text(production_audio_assets / "sounds/tools/knap.tone", "wave = \"noise\"\n"
                                                                   "amplitude = 0.2\n"
                                                                   "duration_ms = 125\n"
                                                                   "attack_ms = 5\n"
                                                                   "release_ms = 20\n"
                                                                   "seed = 42\n");
    write_bytes(production_audio_assets / "music/theme.wav", minimal_wav_bytes());
    write_bytes(production_audio_assets / "music/ambient.flac", minimal_flac_bytes());
    heartstead::assets::AssetCatalog production_audio_catalog;
    auto production_audio_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        production_audio_catalog, production_audio_assets, "base",
        heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!production_audio_indexed.has_errors());
    assert(production_audio_catalog.active_count() == 5);
    assert(production_audio_catalog.count_kind(heartstead::assets::AssetKind::sound) == 3);
    assert(production_audio_catalog.count_kind(heartstead::assets::AssetKind::music) == 2);

    heartstead::assets::AssetCookConfig production_audio_cook_config;
    production_audio_cook_config.backend =
        heartstead::assets::AssetCookBackend::production_converters;
    production_audio_cook_config.output_root = root / "production_audio_cooked_assets";
    auto production_audio_cook = heartstead::assets::AssetCooker::cook(
        production_audio_catalog, production_audio_cook_config);
    assert(production_audio_cook);
    assert(production_audio_cook.value().manifest.profile == "production");
    assert(production_audio_cook.value().cooked_file_count == 5);
    const auto* production_sound_record =
        production_audio_cook.value().manifest.find("base:sounds/tools/hammer.wav");
    const auto* production_ogg_record =
        production_audio_cook.value().manifest.find("base:sounds/tools/impact.ogg");
    const auto* production_tone_record =
        production_audio_cook.value().manifest.find("base:sounds/tools/knap.tone");
    const auto* production_music_record =
        production_audio_cook.value().manifest.find("base:music/theme.wav");
    const auto* production_flac_record =
        production_audio_cook.value().manifest.find("base:music/ambient.flac");
    assert(production_sound_record != nullptr);
    assert(production_ogg_record != nullptr);
    assert(production_tone_record != nullptr);
    assert(production_music_record != nullptr);
    assert(production_flac_record != nullptr);
    assert(production_sound_record->kind == heartstead::assets::AssetKind::sound);
    assert(production_ogg_record->kind == heartstead::assets::AssetKind::sound);
    assert(production_tone_record->kind == heartstead::assets::AssetKind::sound);
    assert(production_music_record->kind == heartstead::assets::AssetKind::music);
    assert(production_flac_record->kind == heartstead::assets::AssetKind::music);
    assert(read_text(production_audio_cook_config.output_root /
                     production_sound_record->cooked_relative_path)
               .find("backend=audio_runtime_converter_v3") != std::string::npos);

    auto production_audio_store =
        heartstead::assets::CookedAssetStore::load(production_audio_cook_config.output_root);
    assert(production_audio_store);
    auto production_sound_payload =
        production_audio_store.value().load_payload("base:sounds/tools/hammer.wav");
    assert(production_sound_payload);
    assert(production_sound_payload.value().kind == heartstead::assets::AssetKind::sound);
    assert(production_sound_payload.value().backend == "audio_runtime_converter_v3");
    assert(production_sound_payload.value().profile == "production");
    assert(production_sound_payload.value().metadata.at("audio.container") == "wav");
    assert(production_sound_payload.value().metadata.at("audio.runtime_format") ==
           "heartstead.audio.container.v1");
    assert(production_sound_payload.value().metadata.at("audio.channels") == "1");
    assert(production_sound_payload.value().metadata.at("audio.sample_rate") == "8000");
    assert(production_sound_payload.value().metadata.at("audio.bits_per_sample") == "8");
    assert(production_sound_payload.value().bytes.size() == minimal_wav_bytes().size());
    auto production_ogg_payload =
        production_audio_store.value().load_payload("base:sounds/tools/impact.ogg");
    assert(production_ogg_payload);
    assert(production_ogg_payload.value().kind == heartstead::assets::AssetKind::sound);
    assert(production_ogg_payload.value().backend == "audio_runtime_converter_v3");
    assert(production_ogg_payload.value().metadata.at("audio.container") == "ogg");
    assert(production_ogg_payload.value().metadata.at("audio.codec") == "vorbis");
    assert(production_ogg_payload.value().metadata.at("audio.runtime_format") ==
           "heartstead.audio.container.v1");
    assert(production_ogg_payload.value().metadata.at("audio.first_page_payload_bytes") == "30");
    assert(production_ogg_payload.value().bytes.size() == minimal_ogg_bytes().size());
    auto production_tone_payload =
        production_audio_store.value().load_payload("base:sounds/tools/knap.tone");
    assert(production_tone_payload);
    assert(production_tone_payload.value().kind == heartstead::assets::AssetKind::sound);
    assert(production_tone_payload.value().backend == "audio_runtime_converter_v3");
    assert(production_tone_payload.value().metadata.at("audio.container") == "tone");
    assert(production_tone_payload.value().metadata.at("audio.runtime_format") ==
           "heartstead.audio.procedural_tone.v1");
    assert(production_tone_payload.value().metadata.at("audio.channels") == "1");
    assert(production_tone_payload.value().metadata.at("audio.sample_rate") == "48000");
    assert(production_tone_payload.value().metadata.at("audio.frames") == "6000");
    auto production_music_payload =
        production_audio_store.value().load_payload("base:music/theme.wav");
    assert(production_music_payload);
    assert(production_music_payload.value().kind == heartstead::assets::AssetKind::music);
    auto production_flac_payload =
        production_audio_store.value().load_payload("base:music/ambient.flac");
    assert(production_flac_payload);
    assert(production_flac_payload.value().kind == heartstead::assets::AssetKind::music);
    assert(production_flac_payload.value().backend == "audio_runtime_converter_v3");
    assert(production_flac_payload.value().metadata.at("audio.container") == "flac");
    assert(production_flac_payload.value().metadata.at("audio.first_block_bytes") == "34");
    assert(production_flac_payload.value().bytes.size() == minimal_flac_bytes().size());

    const auto invalid_audio_assets = root / "invalid_audio_assets";
    write_text(invalid_audio_assets / "sounds/bad.wav", "not a wav");
    heartstead::assets::AssetCatalog invalid_audio_catalog;
    auto invalid_audio_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        invalid_audio_catalog, invalid_audio_assets, "base",
        heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!invalid_audio_indexed.has_errors());
    heartstead::assets::AssetCookConfig invalid_audio_cook_config;
    invalid_audio_cook_config.backend = heartstead::assets::AssetCookBackend::production_converters;
    invalid_audio_cook_config.output_root = root / "invalid_audio_cooked_assets";
    auto invalid_audio_cook =
        heartstead::assets::AssetCooker::cook(invalid_audio_catalog, invalid_audio_cook_config);
    assert(!invalid_audio_cook);
    assert(invalid_audio_cook.error().code == "asset_cooker.invalid_wav");

    const auto invalid_ogg_assets = root / "invalid_ogg_assets";
    write_text(invalid_ogg_assets / "sounds/bad.ogg", "not an ogg");
    heartstead::assets::AssetCatalog invalid_ogg_catalog;
    auto invalid_ogg_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        invalid_ogg_catalog, invalid_ogg_assets, "base", heartstead::assets::AssetSourceKind::mod,
        "base", 0);
    assert(!invalid_ogg_indexed.has_errors());
    heartstead::assets::AssetCookConfig invalid_ogg_cook_config;
    invalid_ogg_cook_config.backend = heartstead::assets::AssetCookBackend::production_converters;
    invalid_ogg_cook_config.output_root = root / "invalid_ogg_cooked_assets";
    auto invalid_ogg_cook =
        heartstead::assets::AssetCooker::cook(invalid_ogg_catalog, invalid_ogg_cook_config);
    assert(!invalid_ogg_cook);
    assert(invalid_ogg_cook.error().code == "asset_cooker.invalid_ogg");

    const auto unsupported_ogg_assets = root / "unsupported_ogg_assets";
    auto opus_ogg = minimal_ogg_bytes();
    constexpr std::size_t first_ogg_payload = 28;
    const std::array<std::uint8_t, 8> opus_header{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    std::ranges::copy(opus_header, opus_ogg.begin() + first_ogg_payload);
    write_bytes(unsupported_ogg_assets / "sounds/unsupported.ogg", opus_ogg);
    heartstead::assets::AssetCatalog unsupported_ogg_catalog;
    auto unsupported_ogg_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        unsupported_ogg_catalog, unsupported_ogg_assets, "base",
        heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!unsupported_ogg_indexed.has_errors());
    heartstead::assets::AssetCookConfig unsupported_ogg_config;
    unsupported_ogg_config.backend = heartstead::assets::AssetCookBackend::production_converters;
    unsupported_ogg_config.output_root = root / "unsupported_ogg_cooked_assets";
    auto unsupported_ogg_cook =
        heartstead::assets::AssetCooker::cook(unsupported_ogg_catalog, unsupported_ogg_config);
    assert(!unsupported_ogg_cook);
    assert(unsupported_ogg_cook.error().code == "asset_cooker.invalid_ogg");
    assert(unsupported_ogg_cook.error().message.find("Vorbis identification packet") !=
           std::string::npos);

    const auto invalid_flac_assets = root / "invalid_flac_assets";
    write_text(invalid_flac_assets / "music/bad.flac", "not flac");
    heartstead::assets::AssetCatalog invalid_flac_catalog;
    auto invalid_flac_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        invalid_flac_catalog, invalid_flac_assets, "base", heartstead::assets::AssetSourceKind::mod,
        "base", 0);
    assert(!invalid_flac_indexed.has_errors());
    heartstead::assets::AssetCookConfig invalid_flac_cook_config;
    invalid_flac_cook_config.backend = heartstead::assets::AssetCookBackend::production_converters;
    invalid_flac_cook_config.output_root = root / "invalid_flac_cooked_assets";
    auto invalid_flac_cook =
        heartstead::assets::AssetCooker::cook(invalid_flac_catalog, invalid_flac_cook_config);
    assert(!invalid_flac_cook);
    assert(invalid_flac_cook.error().code == "asset_cooker.invalid_flac");

    const auto production_font_assets = root / "production_font_assets";
    write_bytes(production_font_assets / "fonts/settlement.ttf", minimal_sfnt_font_bytes());
    heartstead::assets::AssetCatalog production_font_catalog;
    auto production_font_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        production_font_catalog, production_font_assets, "base",
        heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!production_font_indexed.has_errors());
    assert(production_font_catalog.active_count() == 1);
    assert(production_font_catalog.count_kind(heartstead::assets::AssetKind::font) == 1);

    heartstead::assets::AssetCookConfig production_font_cook_config;
    production_font_cook_config.backend =
        heartstead::assets::AssetCookBackend::production_converters;
    production_font_cook_config.output_root = root / "production_font_cooked_assets";
    auto production_font_cook =
        heartstead::assets::AssetCooker::cook(production_font_catalog, production_font_cook_config);
    assert(production_font_cook);
    assert(production_font_cook.value().manifest.profile == "production");
    assert(production_font_cook.value().cooked_file_count == 1);
    const auto* production_font_record =
        production_font_cook.value().manifest.find("base:fonts/settlement.ttf");
    assert(production_font_record != nullptr);
    assert(production_font_record->kind == heartstead::assets::AssetKind::font);
    assert(read_text(production_font_cook_config.output_root /
                     production_font_record->cooked_relative_path)
               .find("backend=font_runtime_converter_v1") != std::string::npos);
    auto production_font_store =
        heartstead::assets::CookedAssetStore::load(production_font_cook_config.output_root);
    assert(production_font_store);
    auto production_font_payload =
        production_font_store.value().load_payload("base:fonts/settlement.ttf");
    assert(production_font_payload);
    assert(production_font_payload.value().kind == heartstead::assets::AssetKind::font);
    assert(production_font_payload.value().backend == "font_runtime_converter_v1");
    assert(production_font_payload.value().profile == "production");
    assert(production_font_payload.value().metadata.at("font.container") == "sfnt");
    assert(production_font_payload.value().metadata.at("font.table_count") == "1");
    assert(production_font_payload.value().metadata.at("font.outline") == "truetype");
    assert(production_font_payload.value().bytes.size() == minimal_sfnt_font_bytes().size());

    const auto invalid_font_assets = root / "invalid_font_assets";
    write_text(invalid_font_assets / "fonts/bad.ttf", "not a font");
    heartstead::assets::AssetCatalog invalid_font_catalog;
    auto invalid_font_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        invalid_font_catalog, invalid_font_assets, "base", heartstead::assets::AssetSourceKind::mod,
        "base", 0);
    assert(!invalid_font_indexed.has_errors());
    heartstead::assets::AssetCookConfig invalid_font_cook_config;
    invalid_font_cook_config.backend = heartstead::assets::AssetCookBackend::production_converters;
    invalid_font_cook_config.output_root = root / "invalid_font_cooked_assets";
    auto invalid_font_cook =
        heartstead::assets::AssetCooker::cook(invalid_font_catalog, invalid_font_cook_config);
    assert(!invalid_font_cook);
    assert(invalid_font_cook.error().code == "asset_cooker.invalid_font");

    const auto production_shader_assets = root / "production_shader_assets";
    write_bytes(production_shader_assets / "shaders/minimal.vert.spv", minimal_spirv_bytes());
    heartstead::assets::AssetCatalog production_shader_catalog;
    auto production_shader_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        production_shader_catalog, production_shader_assets, "base",
        heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!production_shader_indexed.has_errors());
    assert(production_shader_catalog.active_count() == 1);
    assert(production_shader_catalog.count_kind(heartstead::assets::AssetKind::shader) == 1);

    heartstead::assets::AssetCookConfig production_shader_cook_config;
    production_shader_cook_config.backend =
        heartstead::assets::AssetCookBackend::production_converters;
    production_shader_cook_config.output_root = root / "production_shader_cooked_assets";
    auto production_shader_cook = heartstead::assets::AssetCooker::cook(
        production_shader_catalog, production_shader_cook_config);
    assert(production_shader_cook);
    assert(production_shader_cook.value().manifest.profile == "production");
    assert(production_shader_cook.value().cooked_file_count == 1);
    const auto* production_shader_record =
        production_shader_cook.value().manifest.find("base:shaders/minimal.vert.spv");
    assert(production_shader_record != nullptr);
    assert(production_shader_record->kind == heartstead::assets::AssetKind::shader);
    assert(read_text(production_shader_cook_config.output_root /
                     production_shader_record->cooked_relative_path)
               .find("backend=shader_spirv_runtime_passthrough_v1") != std::string::npos);
    auto production_shader_store =
        heartstead::assets::CookedAssetStore::load(production_shader_cook_config.output_root);
    assert(production_shader_store);
    auto production_shader_payload =
        production_shader_store.value().load_payload("base:shaders/minimal.vert.spv");
    assert(production_shader_payload);
    assert(production_shader_payload.value().kind == heartstead::assets::AssetKind::shader);
    assert(production_shader_payload.value().backend == "shader_spirv_runtime_passthrough_v1");
    assert(production_shader_payload.value().profile == "production");
    assert(production_shader_payload.value().bytes.size() == minimal_spirv_bytes().size());

    const auto invalid_cooked_spirv_assets = root / "invalid_cooked_spirv_assets";
    write_text(invalid_cooked_spirv_assets / "shaders/bad.frag.spv", "not spirv");
    heartstead::assets::AssetCatalog invalid_cooked_spirv_catalog;
    auto invalid_cooked_spirv_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        invalid_cooked_spirv_catalog, invalid_cooked_spirv_assets, "base",
        heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!invalid_cooked_spirv_indexed.has_errors());
    heartstead::assets::AssetCookConfig invalid_cooked_spirv_config;
    invalid_cooked_spirv_config.backend =
        heartstead::assets::AssetCookBackend::production_converters;
    invalid_cooked_spirv_config.output_root = root / "invalid_cooked_spirv_output";
    auto invalid_cooked_spirv = heartstead::assets::AssetCooker::cook(invalid_cooked_spirv_catalog,
                                                                      invalid_cooked_spirv_config);
    assert(!invalid_cooked_spirv);
    assert(invalid_cooked_spirv.error().code == "shader_compiler.invalid_spirv");

    const auto unavailable_cooked_shader_assets = root / "unavailable_cooked_shader_assets";
    write_text(unavailable_cooked_shader_assets / "shaders/wyrd_fog.slang", "shader placeholder");
    heartstead::assets::AssetCatalog unavailable_cooked_shader_catalog;
    auto unavailable_cooked_shader_indexed =
        heartstead::assets::AssetCatalogBuilder::index_directory(
            unavailable_cooked_shader_catalog, unavailable_cooked_shader_assets, "base",
            heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!unavailable_cooked_shader_indexed.has_errors());
    heartstead::assets::AssetCookConfig unavailable_cooked_shader_config;
    unavailable_cooked_shader_config.backend =
        heartstead::assets::AssetCookBackend::production_converters;
    unavailable_cooked_shader_config.output_root = root / "unavailable_cooked_shader_output";
    auto unavailable_cooked_shader = heartstead::assets::AssetCooker::cook(
        unavailable_cooked_shader_catalog, unavailable_cooked_shader_config);
    assert(!unavailable_cooked_shader);
    assert(unavailable_cooked_shader.error().code ==
           "shader_compiler.production_compiler_unavailable");

    auto cooked_result = heartstead::assets::AssetCooker::cook(catalog, cook_config);
    assert(cooked_result);
    assert(cooked_result.value().backend ==
           heartstead::assets::AssetCookBackend::development_passthrough);
    assert(cooked_result.value().cooked_file_count == 5);
    assert(cooked_result.value().manifest.records.size() == 5);
    assert(cooked_result.value().manifest.active_count() == 5);
    assert(std::filesystem::exists(cooked_result.value().manifest_path));

    const auto* cooked_model = cooked_result.value().manifest.find("base:models/building/wall.glb");
    const auto* cooked_shader = cooked_result.value().manifest.find("base:shaders/wyrd_fog.slang");
    const auto* cooked_sound = cooked_result.value().manifest.find("base:sounds/tools/hammer.wav");
    const auto* cooked_output_raw_clay =
        cooked_result.value().manifest.find("base:textures/items/raw_clay.txt");
    assert(cooked_output_raw_clay != nullptr);
    assert(cooked_model != nullptr);
    assert(cooked_shader != nullptr);
    assert(cooked_sound != nullptr);
    assert(read_text(cooked_output / cooked_output_raw_clay->cooked_relative_path)
               .find("backend=texture_dev_passthrough_v1") != std::string::npos);
    assert(read_text(cooked_output / cooked_model->cooked_relative_path)
               .find("backend=model_dev_passthrough_v1") != std::string::npos);
    assert(read_text(cooked_output / cooked_shader->cooked_relative_path)
               .find("backend=shader_dev_passthrough_v1") != std::string::npos);
    assert(read_text(cooked_output / cooked_sound->cooked_relative_path)
               .find("backend=audio_dev_passthrough_v1") != std::string::npos);

    const auto shader_output = root / "compiled_shaders";
    heartstead::renderer::shaders::ShaderCompileConfig shader_config;
    shader_config.output_root = shader_output;
    auto shader_compile =
        heartstead::renderer::shaders::ShaderCompiler::compile(catalog, shader_config);
    assert(shader_compile);
    assert(!shader_compile.value().has_errors());
    assert(shader_compile.value().compiled_shader_count == 1);
    assert(shader_compile.value().records.size() == 1);
    assert(shader_compile.value().records.front().logical_id == "base:shaders/wyrd_fog.slang");
    assert(shader_compile.value().records.front().language ==
           heartstead::renderer::shaders::ShaderSourceLanguage::slang);
    assert(shader_compile.value().records.front().role ==
           heartstead::renderer::shaders::ShaderSourceRole::library);
    assert(std::filesystem::exists(shader_compile.value().manifest_path));
    assert(read_text(shader_compile.value().manifest_path).find("heartstead.shader_manifest.v1") !=
           std::string::npos);
    assert(read_text(shader_output / shader_compile.value().records.front().compiled_relative_path)
               .find("backend=slang_dev_validation_v1") != std::string::npos);
    auto shader_record_inspection =
        heartstead::debug::Inspector::inspect(shader_compile.value().records.front());
    assert(shader_record_inspection.object_type == "compiled_shader_record");
    assert(shader_record_inspection.state == "compiled");
    assert(shader_record_inspection.find_field("logical_id")->value ==
           "base:shaders/wyrd_fog.slang");
    assert(shader_record_inspection.find_field("language")->value == "slang");
    assert(shader_record_inspection.find_field("role")->value == "library");
    assert(shader_record_inspection.find_field("backend")->value == "slang_dev_validation_v1");
    assert(shader_record_inspection.find_field("compiled_hash")->value.size() == 16);

    auto shader_result_inspection = heartstead::debug::Inspector::inspect(shader_compile.value());
    assert(shader_result_inspection.object_type == "shader_compile_result");
    assert(shader_result_inspection.state == "compiled");
    assert(shader_result_inspection.find_field("record_count")->value == "1");
    assert(shader_result_inspection.find_field("compiled_shader_count")->value == "1");
    assert(shader_result_inspection.find_field("slang_count")->value == "1");
    assert(shader_result_inspection.find_field("library_count")->value == "1");
    assert(shader_result_inspection.find_field("first_record_backend")->value ==
           "slang_dev_validation_v1");

    auto invalid_shader_limit_config = shader_config;
    invalid_shader_limit_config.maximum_source_bytes = 0;
    auto invalid_shader_limit = heartstead::renderer::shaders::ShaderCompiler::compile(
        catalog, std::move(invalid_shader_limit_config));
    assert(!invalid_shader_limit &&
           invalid_shader_limit.error().code == "shader_compiler.invalid_source_limit");

    auto bounded_shader_config = shader_config;
    bounded_shader_config.output_root = root / "bounded_compiled_shaders";
    bounded_shader_config.maximum_source_bytes = 4;
    auto bounded_shader_compile = heartstead::renderer::shaders::ShaderCompiler::compile(
        catalog, std::move(bounded_shader_config));
    assert(bounded_shader_compile && bounded_shader_compile.value().has_errors());
    assert(
        std::ranges::any_of(bounded_shader_compile.value().diagnostics, [](const auto& diagnostic) {
            return diagnostic.code == "shader_compiler.source_too_large";
        }));

    auto invalid_shader_record = shader_compile.value().records.front();
    invalid_shader_record.compiled_hash.clear();
    auto invalid_shader_record_inspection =
        heartstead::debug::Inspector::inspect(invalid_shader_record);
    assert(invalid_shader_record_inspection.state == "invalid");
    assert(invalid_shader_record_inspection.has_errors());
    assert(invalid_shader_record_inspection.issues.front().code ==
           "compiled_shader.missing_compiled_hash");
    assert(heartstead::renderer::shaders::infer_shader_role("shaders/templates/terrain.slang") ==
           heartstead::renderer::shaders::ShaderSourceRole::template_source);
    assert(heartstead::renderer::shaders::shader_compile_backend_name(
               heartstead::renderer::shaders::ShaderSourceLanguage::spirv, "production") ==
           "spirv_runtime_passthrough_v1");

    const auto production_spirv_assets = root / "production_spirv_assets";
    write_bytes(production_spirv_assets / "shaders/minimal.vert.spv", minimal_spirv_bytes());
    heartstead::assets::AssetCatalog production_spirv_catalog;
    auto production_spirv_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        production_spirv_catalog, production_spirv_assets, "base",
        heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!production_spirv_indexed.has_errors());
    assert(production_spirv_catalog.active_count() == 1);
    heartstead::renderer::shaders::ShaderCompileConfig production_shader_config;
    production_shader_config.output_root = root / "production_compiled_shaders";
    production_shader_config.profile = "production";
    auto production_shader_compile = heartstead::renderer::shaders::ShaderCompiler::compile(
        production_spirv_catalog, production_shader_config);
    assert(production_shader_compile);
    assert(!production_shader_compile.value().has_errors());
    assert(production_shader_compile.value().compiled_shader_count == 1);
    assert(production_shader_compile.value().records.size() == 1);
    assert(production_shader_compile.value().records.front().logical_id ==
           "base:shaders/minimal.vert.spv");
    assert(production_shader_compile.value().records.front().language ==
           heartstead::renderer::shaders::ShaderSourceLanguage::spirv);
    assert(production_shader_compile.value().records.front().role ==
           heartstead::renderer::shaders::ShaderSourceRole::vertex);
    assert(production_shader_compile.value().records.front().backend ==
           "spirv_runtime_passthrough_v1");
    assert(read_text(production_shader_compile.value().manifest_path).find("profile=production") !=
           std::string::npos);
    assert(read_text(production_shader_config.output_root /
                     production_shader_compile.value().records.front().compiled_relative_path)
               .find("backend=spirv_runtime_passthrough_v1") != std::string::npos);

    heartstead::assets::AssetCatalog production_slang_catalog;
    assert(production_slang_catalog.add(heartstead::assets::AssetRecord{
        "base:shaders/wyrd_fog.slang",
        heartstead::assets::AssetKind::shader,
        heartstead::assets::VirtualPath::parse("base:shaders/wyrd_fog.slang").value(),
        heartstead::assets::AssetSourceKind::mod,
        "base",
        0,
        mod_assets / "shaders/wyrd_fog.slang",
        "production_slang_hash",
        false,
        {},
    }));
    auto production_slang_compile = heartstead::renderer::shaders::ShaderCompiler::compile(
        production_slang_catalog, production_shader_config);
    assert(production_slang_compile);
    assert(production_slang_compile.value().has_errors());
    assert(production_slang_compile.value().compiled_shader_count == 0);
    assert(production_slang_compile.value().diagnostics.front().code ==
           "shader_compiler.production_compiler_unavailable");
    auto failed_shader_compile_inspection =
        heartstead::debug::Inspector::inspect(production_slang_compile.value());
    assert(failed_shader_compile_inspection.state == "invalid");
    assert(failed_shader_compile_inspection.find_field("diagnostic_count")->value == "1");
    assert(failed_shader_compile_inspection.find_field("first_diagnostic_code")->value ==
           "shader_compiler.production_compiler_unavailable");

    const auto invalid_spirv_assets = root / "invalid_spirv_assets";
    write_text(invalid_spirv_assets / "shaders/bad.frag.spv", "not spirv");
    heartstead::assets::AssetCatalog invalid_spirv_catalog;
    auto invalid_spirv_indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        invalid_spirv_catalog, invalid_spirv_assets, "base",
        heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!invalid_spirv_indexed.has_errors());
    auto invalid_spirv_compile = heartstead::renderer::shaders::ShaderCompiler::compile(
        invalid_spirv_catalog, production_shader_config);
    assert(invalid_spirv_compile);
    assert(invalid_spirv_compile.value().has_errors());
    assert(invalid_spirv_compile.value().compiled_shader_count == 0);
    assert(invalid_spirv_compile.value().diagnostics.front().code ==
           "shader_compiler.invalid_spirv");

    const auto invalid_shader_assets = root / "invalid_shader_assets";
    write_text(invalid_shader_assets / "shaders/bad.txt", "not a supported shader language");
    heartstead::assets::AssetCatalog invalid_shader_catalog;
    auto bad_shader_path = heartstead::assets::VirtualPath::parse("base:shaders/bad.txt");
    assert(bad_shader_path);
    assert(invalid_shader_catalog.add(heartstead::assets::AssetRecord{
        "base:shaders/bad.txt",
        heartstead::assets::AssetKind::shader,
        bad_shader_path.value(),
        heartstead::assets::AssetSourceKind::mod,
        "base",
        0,
        invalid_shader_assets / "shaders/bad.txt",
        "bad_shader_hash",
        false,
        {},
    }));
    shader_config.output_root = root / "invalid_compiled_shaders";
    auto invalid_shader_compile = heartstead::renderer::shaders::ShaderCompiler::compile(
        invalid_shader_catalog, shader_config);
    assert(invalid_shader_compile);
    assert(invalid_shader_compile.value().has_errors());
    assert(invalid_shader_compile.value().compiled_shader_count == 0);
    assert(invalid_shader_compile.value().diagnostics.front().code ==
           "shader_compiler.unknown_language");

    auto store = heartstead::assets::CookedAssetStore::load(cooked_output);
    assert(store);
    assert(store.value().manifest().records.size() == 5);
    auto raw_clay_payload = store.value().load_payload("base:textures/items/raw_clay.txt");
    assert(raw_clay_payload);
    assert(raw_clay_payload.value().logical_id == "base:textures/items/raw_clay.txt");
    assert(raw_clay_payload.value().kind == heartstead::assets::AssetKind::texture);
    assert(raw_clay_payload.value().backend == "texture_dev_passthrough_v1");
    assert(raw_clay_payload.value().profile == "development");
    assert(raw_clay_payload.value().source_path.generic_string() == "textures/items/raw_clay.txt");
    assert(std::string(raw_clay_payload.value().bytes.begin(),
                       raw_clay_payload.value().bytes.end()) == "ultra resource pack texture");

    auto missing_payload = store.value().load_payload("base:textures/items/missing.txt");
    assert(!missing_payload);
    assert(missing_payload.error().code == "cooked_asset_store.asset_not_found");

    auto bounded_manifest_store = heartstead::assets::CookedAssetStore::load(
        cooked_output, "asset_manifest.txt",
        {.maximum_manifest_bytes = 4, .maximum_payload_bytes = 1024});
    assert(!bounded_manifest_store &&
           bounded_manifest_store.error().code == "cooked_asset_store.file_too_large");

    auto bounded_payload_store = heartstead::assets::CookedAssetStore::load(
        cooked_output, "asset_manifest.txt",
        {.maximum_manifest_bytes = 1024 * 1024, .maximum_payload_bytes = 4});
    assert(bounded_payload_store);
    auto bounded_payload =
        bounded_payload_store.value().load_payload("base:textures/items/raw_clay.txt");
    assert(!bounded_payload && bounded_payload.error().code == "cooked_asset_store.file_too_large");

    const auto raw_payload_path = cooked_output / cooked_output_raw_clay->cooked_relative_path;
    const auto raw_payload_text = read_text(raw_payload_path);
    const std::vector<std::uint8_t> raw_payload_bytes(raw_payload_text.begin(),
                                                      raw_payload_text.end());
    assert(cooked_output_raw_clay->cooked_hash ==
           heartstead::core::stable_hash64_hex(raw_payload_bytes));

    auto tampered_body_bytes = raw_payload_bytes;
    assert(!tampered_body_bytes.empty());
    tampered_body_bytes.back() ^= 0x01U;
    write_bytes(raw_payload_path, tampered_body_bytes);
    auto tampered_store_payload = store.value().load_payload("base:textures/items/raw_clay.txt");
    assert(!tampered_store_payload);
    assert(tampered_store_payload.error().code == "cooked_asset_store.cooked_hash_mismatch");
    write_bytes(raw_payload_path, raw_payload_bytes);

    auto tampered_header_text = raw_payload_text;
    const auto source_hash_begin = tampered_header_text.find("source_hash=");
    assert(source_hash_begin != std::string::npos);
    const auto source_hash_end = tampered_header_text.find('\n', source_hash_begin);
    assert(source_hash_end != std::string::npos);
    tampered_header_text.replace(source_hash_begin, source_hash_end - source_hash_begin,
                                 "source_hash=tampered");
    const std::vector<std::uint8_t> tampered_header_bytes(tampered_header_text.begin(),
                                                          tampered_header_text.end());
    auto tampered_header_record = *cooked_output_raw_clay;
    tampered_header_record.cooked_hash = heartstead::core::stable_hash64_hex(tampered_header_bytes);
    auto tampered_header_payload = heartstead::assets::CookedAssetPayloadCodec::decode(
        tampered_header_bytes, tampered_header_record, cooked_result.value().manifest.profile);
    assert(!tampered_header_payload);
    assert(tampered_header_payload.error().code == "cooked_asset_store.header_mismatch");

    auto bounded_direct_payload = heartstead::assets::CookedAssetPayloadCodec::decode(
        raw_payload_bytes, *cooked_output_raw_clay, cooked_result.value().manifest.profile,
        raw_payload_bytes.size() - 1U);
    assert(!bounded_direct_payload &&
           bounded_direct_payload.error().code == "cooked_asset_store.file_too_large");

    auto invalid_decode = heartstead::assets::CookedAssetManifestTextCodec::decode("bad\n");
    assert(!invalid_decode);
}

void test_filtered_model_dependency_cooking() {
    const auto root = make_temp_root();
    const auto assets_root = root / "assets";
    const auto model_path = assets_root / "models/props/textured.gltf";
    const auto buffer_path = assets_root / "models/props/triangle.bin";
    const auto image_path = assets_root / "models/props/albedo.png";
    write_text(model_path, minimal_external_textured_gltf_text());
    write_bytes(buffer_path, minimal_triangle_buffer_bytes());
    write_bytes(image_path, minimal_png_bytes());

    const auto build_catalog = [&] {
        heartstead::assets::AssetCatalog catalog;
        const auto indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
            catalog, assets_root, "base", heartstead::assets::AssetSourceKind::mod, "base", 0);
        assert(!indexed.has_errors());
        const auto dependencies = heartstead::assets::discover_asset_dependencies(catalog);
        assert(dependencies);
        return catalog;
    };

    auto catalog = build_catalog();
    const std::vector<std::string> selected_ids{"base:models/props/textured.gltf"};
    auto filtered = heartstead::assets::select_asset_dependency_closure(catalog, selected_ids);
    assert(filtered);
    assert(filtered.value().active_count() == 3);
    assert(filtered.value().find_active("base:models/props/textured.gltf") != nullptr);
    assert(filtered.value().find_active("base:models/props/triangle.bin") != nullptr);
    assert(filtered.value().find_active("base:models/props/albedo.png") != nullptr);

    heartstead::assets::AssetCookConfig first_config;
    first_config.backend = heartstead::assets::AssetCookBackend::production_converters;
    first_config.output_root = root / "cooked_first";
    auto first_cook = heartstead::assets::AssetCooker::cook(filtered.value(), first_config);
    assert(first_cook);
    assert(first_cook.value().cooked_file_count == 3);
    const auto* first_model =
        first_cook.value().manifest.find_active("base:models/props/textured.gltf");
    assert(first_model != nullptr);
    assert(first_model->dependencies.size() == 2);
    const auto first_model_source_hash = first_model->source_hash;
    const auto first_model_cooked_hash = first_model->cooked_hash;
    auto first_store = heartstead::assets::CookedAssetStore::load(first_config.output_root);
    assert(first_store);
    auto first_payload = first_store.value().load_payload("base:models/props/textured.gltf");
    assert(first_payload);
    auto first_runtime_model = heartstead::assets::decode_model_asset(first_payload.value().bytes);
    assert(first_runtime_model);
    assert(first_runtime_model.value().images.size() == 1);
    assert(first_runtime_model.value().images.front().width == 2);
    assert(first_runtime_model.value().materials.front().base_color_texture.image == 0);

    write_bytes(image_path, one_pixel_png_bytes());
    auto changed_catalog = build_catalog();
    auto changed_filtered =
        heartstead::assets::select_asset_dependency_closure(changed_catalog, selected_ids);
    assert(changed_filtered);
    heartstead::assets::AssetCookConfig changed_config = first_config;
    changed_config.output_root = root / "cooked_changed";
    auto changed_cook =
        heartstead::assets::AssetCooker::cook(changed_filtered.value(), changed_config);
    assert(changed_cook);
    const auto* changed_model =
        changed_cook.value().manifest.find_active("base:models/props/textured.gltf");
    assert(changed_model != nullptr);
    assert(changed_model->source_hash == first_model_source_hash);
    assert(changed_model->cooked_hash != first_model_cooked_hash);

    write_text(model_path, minimal_external_textured_gltf_text("BLEND"));
    auto blend_catalog = build_catalog();
    auto blend_filtered =
        heartstead::assets::select_asset_dependency_closure(blend_catalog, selected_ids);
    assert(blend_filtered);
    heartstead::assets::AssetCookConfig blend_config = first_config;
    blend_config.output_root = root / "cooked_blend";
    auto blend_cook = heartstead::assets::AssetCooker::cook(blend_filtered.value(), blend_config);
    assert(blend_cook);
    auto blend_store = heartstead::assets::CookedAssetStore::load(blend_config.output_root);
    assert(blend_store);
    auto blend_payload = blend_store.value().load_payload("base:models/props/textured.gltf");
    assert(blend_payload);
    auto blend_model = heartstead::assets::decode_model_asset(blend_payload.value().bytes);
    assert(blend_model);
    assert(blend_model.value().materials.front().alpha_mode ==
           heartstead::assets::ModelAlphaMode::blend);

    std::filesystem::remove_all(root);
}

void test_terrain_material_asset_loading() {
    using namespace heartstead;
    const auto root = make_temp_root();
    const auto assets_root = root / "assets";
    constexpr std::array texture_names{
        "grass.png",      "grass_alt.png",      "grass_side.png",  "grass_side_alt.png",
        "grass_west.png", "grass_west_alt.png", "grass_east.png",  "grass_bottom.png",
        "grass_top.png",  "grass_top_alt.png",  "grass_north.png", "grass_south.png",
    };
    for (const auto* name : texture_names) {
        write_bytes(assets_root / "textures/voxels" / name, minimal_png_bytes());
    }

    assets::AssetCatalog catalog;
    const auto indexed = assets::AssetCatalogBuilder::index_directory(
        catalog, assets_root, "base", assets::AssetSourceKind::mod, "base", 0);
    assert(!indexed.has_errors());

    assets::AssetCookConfig cook_config;
    cook_config.backend = assets::AssetCookBackend::production_converters;
    cook_config.output_root = root / "cooked";
    const auto cooked = assets::AssetCooker::cook(catalog, cook_config);
    assert(cooked);
    const auto store = assets::CookedAssetStore::load(cook_config.output_root);
    assert(store);

    world::VoxelDefinition grass;
    grass.type = 7;
    grass.prototype_id = core::PrototypeId::parse("base:voxels/grass").value();
    grass.display_name = "Grass";
    grass.terrain_material = "grass";
    grass.mining_tool = "shovel";
    world::VoxelPalette palette;
    assert(palette.add(std::move(grass)));
    world::VoxelDefinition fallback;
    fallback.type = 8;
    fallback.prototype_id = core::PrototypeId::parse("base:voxels/fallback").value();
    fallback.display_name = "Fallback";
    fallback.terrain_material = "fallback";
    fallback.mining_tool = "shovel";
    assert(palette.add(std::move(fallback)));

    renderer::materials::MaterialDefinition material;
    material.id = core::PrototypeId::parse("base:materials/grass").value();
    material.domain = renderer::materials::MaterialDomain::terrain;
    material.blend_mode = renderer::materials::MaterialBlendMode::opaque;
    material.shader_template =
        assets::VirtualPath::parse("base:shaders/templates/terrain.slang").value();
    material.textures.push_back(
        {"albedo", assets::VirtualPath::parse("base:textures/voxels/grass.png").value(), true});
    material.textures.push_back(
        {"albedo.variant.1",
         assets::VirtualPath::parse("base:textures/voxels/grass_alt.png").value(), true});
    material.textures.push_back(
        {"side", assets::VirtualPath::parse("base:textures/voxels/grass_side.png").value(), true});
    material.textures.push_back(
        {"side.variant.1",
         assets::VirtualPath::parse("base:textures/voxels/grass_side_alt.png").value(), true});
    material.textures.push_back(
        {"west", assets::VirtualPath::parse("base:textures/voxels/grass_west.png").value(), true});
    material.textures.push_back(
        {"west.variant.1",
         assets::VirtualPath::parse("base:textures/voxels/grass_west_alt.png").value(), true});
    material.textures.push_back(
        {"east", assets::VirtualPath::parse("base:textures/voxels/grass_east.png").value(), true});
    material.textures.push_back(
        {"bottom", assets::VirtualPath::parse("base:textures/voxels/grass_bottom.png").value(),
         true});
    material.textures.push_back(
        {"top", assets::VirtualPath::parse("base:textures/voxels/grass_top.png").value(), true});
    material.textures.push_back(
        {"top.variant.1",
         assets::VirtualPath::parse("base:textures/voxels/grass_top_alt.png").value(), true});
    material.textures.push_back(
        {"north", assets::VirtualPath::parse("base:textures/voxels/grass_north.png").value(),
         true});
    material.textures.push_back(
        {"south", assets::VirtualPath::parse("base:textures/voxels/grass_south.png").value(),
         true});
    material.textures.push_back(
        {"normal.top", assets::VirtualPath::parse("base:textures/voxels/grass_top.png").value(),
         true});
    material.textures.push_back(
        {"surface.top",
         assets::VirtualPath::parse("base:textures/voxels/grass_top_alt.png").value(), true});
    material.textures.push_back(
        {"overlay.moss", assets::VirtualPath::parse("base:textures/voxels/grass_side.png").value(),
         true});
    material.scalars.push_back({"roughness", 0.9F});
    material.scalars.push_back({"texel_density", 2.0F});
    material.scalars.push_back({"stable_mirroring", 1.0F});
    material.scalars.push_back({"surface.moss.strength", 0.65F});
    material.colors.push_back({"tint", renderer::materials::MaterialColor{0.8F, 1.0F, 0.8F, 1.0F}});
    material.colors.push_back(
        {"biome_tint", renderer::materials::MaterialColor{0.7F, 0.9F, 0.6F, 1.0F}});
    renderer::materials::MaterialRegistry materials;
    assert(materials.add(std::move(material)));
    renderer::materials::MaterialDefinition fallback_material;
    fallback_material.id = core::PrototypeId::parse("base:materials/fallback").value();
    fallback_material.domain = renderer::materials::MaterialDomain::terrain;
    fallback_material.blend_mode = renderer::materials::MaterialBlendMode::opaque;
    fallback_material.shader_template =
        assets::VirtualPath::parse("base:shaders/templates/terrain.slang").value();
    fallback_material.textures = {
        {"albedo", assets::VirtualPath::parse("base:textures/voxels/grass.png").value(), true},
        {"albedo.variant.1",
         assets::VirtualPath::parse("base:textures/voxels/grass_alt.png").value(), true},
        {"side", assets::VirtualPath::parse("base:textures/voxels/grass_side.png").value(), true},
        {"side.variant.1",
         assets::VirtualPath::parse("base:textures/voxels/grass_side_alt.png").value(), true},
        {"top", assets::VirtualPath::parse("base:textures/voxels/grass_top.png").value(), true},
        {"top.variant.1",
         assets::VirtualPath::parse("base:textures/voxels/grass_top_alt.png").value(), true},
    };
    assert(materials.add(std::move(fallback_material)));

    const auto terrain_assets =
        renderer::materials::load_terrain_material_assets(palette, materials, store.value());
    assert(terrain_assets);
    assert(terrain_assets.value().textures.size() == texture_names.size());
    assert(terrain_assets.value().textures[0].image.width == 2);
    assert(terrain_assets.value().textures[0].image.height == 2);
    assert(terrain_assets.value().textures[0].image.rgba8.size() == 16);
    const auto face_texture_ids =
        [&terrain_assets](const renderer::materials::TerrainVoxelMaterialAsset& assignment,
                          renderer::VoxelMaterialFace face) {
            std::vector<std::string> result;
            for (const auto texture : assignment.textures_for(face)) {
                result.push_back(terrain_assets.value().textures[texture].logical_id);
            }
            return result;
        };
    const auto* assignment = terrain_assets.value().find(7);
    assert(assignment != nullptr);
    assert((face_texture_ids(*assignment, renderer::VoxelMaterialFace::west) ==
            std::vector<std::string>{"base:textures/voxels/grass_west.png",
                                     "base:textures/voxels/grass_west_alt.png"}));
    assert((face_texture_ids(*assignment, renderer::VoxelMaterialFace::east) ==
            std::vector<std::string>{"base:textures/voxels/grass_east.png"}));
    assert((face_texture_ids(*assignment, renderer::VoxelMaterialFace::bottom) ==
            std::vector<std::string>{"base:textures/voxels/grass_bottom.png"}));
    assert((face_texture_ids(*assignment, renderer::VoxelMaterialFace::top) ==
            std::vector<std::string>{"base:textures/voxels/grass_top.png",
                                     "base:textures/voxels/grass_top_alt.png"}));
    assert((face_texture_ids(*assignment, renderer::VoxelMaterialFace::north) ==
            std::vector<std::string>{"base:textures/voxels/grass_north.png"}));
    assert((face_texture_ids(*assignment, renderer::VoxelMaterialFace::south) ==
            std::vector<std::string>{"base:textures/voxels/grass_south.png"}));
    assert(assignment->roughness == 0.9F);
    assert(assignment->texel_density == 2.0F);
    assert(assignment->stable_mirroring);
    assert(assignment->normal_textures_for(renderer::VoxelMaterialFace::top).size() == 1);
    assert(assignment->surface_textures_for(renderer::VoxelMaterialFace::top).size() == 1);
    const auto moss_index = static_cast<std::size_t>(world::VoxelSurfaceState::moss);
    assert(assignment->surface_layers[moss_index].strength == 0.65F);
    assert(assignment->surface_layers[moss_index].texture !=
           renderer::materials::no_terrain_texture_asset);
    assert((assignment->base_color == std::array<float, 4>{0.8F, 1.0F, 0.8F, 1.0F}));
    const auto* fallback_assignment = terrain_assets.value().find(8);
    assert(fallback_assignment != nullptr);
    const std::vector<std::string> side_ids{"base:textures/voxels/grass_side.png",
                                            "base:textures/voxels/grass_side_alt.png"};
    assert(face_texture_ids(*fallback_assignment, renderer::VoxelMaterialFace::west) == side_ids);
    assert(face_texture_ids(*fallback_assignment, renderer::VoxelMaterialFace::east) == side_ids);
    assert(face_texture_ids(*fallback_assignment, renderer::VoxelMaterialFace::north) == side_ids);
    assert(face_texture_ids(*fallback_assignment, renderer::VoxelMaterialFace::south) == side_ids);
    assert((face_texture_ids(*fallback_assignment, renderer::VoxelMaterialFace::bottom) ==
            std::vector<std::string>{"base:textures/voxels/grass.png",
                                     "base:textures/voxels/grass_alt.png"}));

    world::VoxelDefinition invalid;
    invalid.type = 9;
    invalid.prototype_id = core::PrototypeId::parse("base:voxels/invalid").value();
    invalid.display_name = "Invalid";
    invalid.terrain_material = "invalid";
    invalid.mining_tool = "shovel";
    assert(palette.add(std::move(invalid)));
    renderer::materials::MaterialDefinition invalid_material;
    invalid_material.id = core::PrototypeId::parse("base:materials/invalid").value();
    invalid_material.domain = renderer::materials::MaterialDomain::terrain;
    invalid_material.blend_mode = renderer::materials::MaterialBlendMode::opaque;
    invalid_material.shader_template =
        assets::VirtualPath::parse("base:shaders/templates/terrain.slang").value();
    invalid_material.textures = {
        {"top.variant.1",
         assets::VirtualPath::parse("base:textures/voxels/grass_top_alt.png").value(), true},
    };
    renderer::materials::MaterialRegistry invalid_materials;
    assert(invalid_materials.add(std::move(invalid_material)));
    const auto invalid_assets = renderer::materials::load_terrain_material_assets(
        palette, invalid_materials, store.value());
    assert(!invalid_assets);
    assert(invalid_assets.error().code == "terrain_material_assets.variant_without_primary");

    std::filesystem::remove_all(root);
}

void test_voxel_surface_state_encoding() {
    using namespace heartstead::world;
    std::uint16_t all_layers = 0;
    for (const auto layer : voxel_surface_states()) {
        all_layers =
            static_cast<std::uint16_t>(all_layers | (1U << static_cast<std::uint8_t>(layer)));
        const auto encoded = encode_voxel_surface_states(
            {static_cast<std::uint16_t>(1U << static_cast<std::uint8_t>(layer)), 5});
        const auto decoded = decode_voxel_surface_states(encoded);
        assert(decoded);
        assert(decoded.value().contains(layer));
        assert(decoded.value().coverage == 5);
    }
    const auto encoded = encode_voxel_surface_states({all_layers, 99});
    const auto decoded = decode_voxel_surface_states(encoded);
    assert(decoded);
    assert(decoded.value().layers == voxel_surface_state_mask);
    assert(decoded.value().coverage == 7);
    assert(encode_voxel_surface_states(decoded.value()) == encoded);
    assert(!decode_voxel_surface_states(
        static_cast<std::uint16_t>(encoded | voxel_surface_reserved_mask)));
}

void test_headless_platform() {
    const auto headless_info = heartstead::platform::platform_backend_info(
        heartstead::platform::PlatformBackend::headless);
    assert(headless_info.available);
    assert(headless_info.name == "headless");
    const auto native_info =
        heartstead::platform::platform_backend_info(heartstead::platform::PlatformBackend::native);
    assert(native_info.name == "native");
    const auto headless_capabilities = heartstead::platform::platform_backend_capabilities(
        heartstead::platform::PlatformBackend::headless);
    assert(headless_capabilities.available);
    assert(headless_capabilities.headless);
    assert(headless_capabilities.supports_logical_windows);
    assert(!headless_capabilities.supports_native_windows);
    assert(headless_capabilities.supports_keyboard_input);
    assert(headless_capabilities.supports_text_input);
    assert(headless_capabilities.supports_mouse_input);
    assert(headless_capabilities.supports_display_metadata);
    assert(!headless_capabilities.supports_vulkan_surface);
    assert(headless_capabilities.supports_clipboard);
    assert(headless_capabilities.window_system == "headless");
    const auto native_capabilities = heartstead::platform::platform_backend_capabilities(
        heartstead::platform::PlatformBackend::native);
    assert(native_capabilities.available == native_info.available);
    assert(!native_capabilities.headless);
    if (native_info.available) {
        assert(native_capabilities.supports_logical_windows);
        assert(native_capabilities.supports_native_windows);
        assert(native_capabilities.supports_keyboard_input);
        assert(native_capabilities.supports_text_input);
        assert(native_capabilities.supports_mouse_input);
        assert(native_capabilities.supports_display_metadata);
        assert(native_capabilities.supports_vulkan_surface);
        assert(native_capabilities.supports_clipboard);
        assert(native_capabilities.window_system == "x11");
    } else {
        assert(native_capabilities.window_system == "x11" ||
               native_capabilities.window_system == "sdl3_or_equivalent");
        assert(native_capabilities.supports_clipboard ==
               (native_capabilities.window_system == "x11"));
    }

    assert(heartstead::platform::validate_platform_desc(
        heartstead::platform::PlatformDesc{heartstead::platform::PlatformBackend::headless}));
    auto native_platform = heartstead::platform::create_platform(
        heartstead::platform::PlatformDesc{heartstead::platform::PlatformBackend::native});
    if (native_info.available) {
        assert(native_platform);
        assert(native_platform.value()->backend() == heartstead::platform::PlatformBackend::native);
        assert(native_platform.value()->backend_name() == "native");
        assert(native_platform.value()->capabilities().supports_clipboard);
        assert(native_platform.value()->set_clipboard_text("native clipboard smoke"));
        auto native_clipboard_text = native_platform.value()->clipboard_text();
        assert(native_clipboard_text);
        assert(native_clipboard_text.value() == "native clipboard smoke");
        std::string native_large_clipboard(192 * 1024, 'n');
        native_large_clipboard.front() = 'H';
        native_large_clipboard.back() = 'd';
        assert(native_platform.value()->set_clipboard_text(native_large_clipboard));
        native_clipboard_text = native_platform.value()->clipboard_text();
        assert(native_clipboard_text);
        assert(native_clipboard_text.value() == native_large_clipboard);
        const auto native_displays = native_platform.value()->displays();
        assert(!native_displays.empty());
        assert(std::ranges::any_of(native_displays,
                                   [](const heartstead::platform::DisplayInfo& display) {
                                       return display.primary && !display.name.empty() &&
                                              display.width_px > 0 && display.height_px > 0;
                                   }));
        auto native_window =
            native_platform.value()->create_window({"Heartstead Native Test", 320, 180, false});
        assert(native_window);
        const auto native_handle =
            native_platform.value()->native_window_handle(native_window.value());
        assert(native_handle);
        assert(native_handle->system == heartstead::platform::NativeWindowSystem::x11);
        assert(native_handle->display != nullptr);
        assert(native_handle->window != 0);
        assert(native_platform.value()->open_window_count() == 1);
        std::uint32_t native_frames = 0;
        auto native_run = heartstead::platform::run_platform_app(
            *native_platform.value(), heartstead::platform::AppRunConfig{2},
            [&native_frames](heartstead::platform::IPlatform& active_platform, std::uint64_t frame,
                             std::int64_t) {
                ++native_frames;
                while (auto event = active_platform.poll_event()) {
                    assert(event->kind != heartstead::platform::PlatformEventKind::quit_requested);
                }
                if (frame == 1) {
                    active_platform.request_quit();
                }
                return heartstead::core::Status::ok();
            });
        assert(native_run);
        assert(native_frames == 2);
        assert(native_platform.value()->should_quit());
        assert(native_platform.value()->close_window(native_window.value()));
        assert(native_platform.value()->open_window_count() == 0);
    } else {
        assert(!native_platform);
        assert(native_platform.error().code == "platform.native_unavailable");
    }

    auto platform_interface = heartstead::platform::create_platform(
        heartstead::platform::PlatformDesc{heartstead::platform::PlatformBackend::headless});
    assert(platform_interface);
    assert(platform_interface.value()->backend() ==
           heartstead::platform::PlatformBackend::headless);
    assert(platform_interface.value()->backend_name() == "headless");
    assert(platform_interface.value()->capabilities().supports_logical_windows);
    assert(platform_interface.value()->capabilities().supports_clipboard);
    assert(platform_interface.value()->set_clipboard_text("interface clipboard"));
    auto interface_clipboard = platform_interface.value()->clipboard_text();
    assert(interface_clipboard);
    assert(interface_clipboard.value() == "interface clipboard");
    auto interface_window =
        platform_interface.value()->create_window({"Heartstead Interface Test", 320, 180, false});
    assert(interface_window);
    assert(!platform_interface.value()->native_window_handle(interface_window.value()));
    assert(platform_interface.value()->open_window_count() == 1);
    std::uint32_t interface_frames = 0;
    auto interface_run = heartstead::platform::run_platform_app(
        *platform_interface.value(), heartstead::platform::AppRunConfig{2},
        [&interface_frames](heartstead::platform::IPlatform& active_platform, std::uint64_t frame,
                            std::int64_t) {
            ++interface_frames;
            while (auto event = active_platform.poll_event()) {
                assert(event->kind == heartstead::platform::PlatformEventKind::window_created);
            }
            if (frame == 1) {
                active_platform.request_quit();
            }
            return heartstead::core::Status::ok();
        });
    assert(interface_run);
    assert(interface_frames == 2);
    assert(platform_interface.value()->should_quit());

    heartstead::platform::HeadlessPlatform platform;
    assert(platform.backend() == heartstead::platform::PlatformBackend::headless);
    assert(platform.backend_name() == "headless");
    auto initial_clipboard = platform.clipboard_text();
    assert(initial_clipboard);
    assert(initial_clipboard.value().empty());
    assert(platform.set_clipboard_text("clay vessel"));
    auto clipboard = platform.clipboard_text();
    assert(clipboard);
    assert(clipboard.value() == "clay vessel");
    std::string large_clipboard(256 * 1024, 'c');
    large_clipboard.front() = 'H';
    large_clipboard.back() = 'd';
    assert(platform.set_clipboard_text(large_clipboard));
    clipboard = platform.clipboard_text();
    assert(clipboard);
    assert(clipboard.value() == large_clipboard);
    assert(platform.set_clipboard_text(""));
    clipboard = platform.clipboard_text();
    assert(clipboard);
    assert(clipboard.value().empty());
    const auto headless_displays = platform.displays();
    assert(headless_displays.size() == 1);
    assert(headless_displays.front().index == 0);
    assert(headless_displays.front().name == "headless");
    assert(headless_displays.front().x_px == 0);
    assert(headless_displays.front().y_px == 0);
    assert(headless_displays.front().width_px == 1280);
    assert(headless_displays.front().height_px == 720);
    assert(headless_displays.front().primary);
    assert(headless_displays.front().dpi_x == 96.0);
    assert(headless_displays.front().dpi_y == 96.0);
    assert(headless_displays.front().refresh_hz == 0.0);
    auto display_inspection = heartstead::debug::Inspector::inspect(headless_displays.front());
    assert(display_inspection.object_type == "platform_display");
    assert(display_inspection.state == "primary");
    assert(display_inspection.find_field("x_px") != nullptr);
    assert(display_inspection.find_field("x_px")->value == "0");
    assert(display_inspection.find_field("width_px") != nullptr);
    assert(display_inspection.find_field("width_px")->value == "1280");
    assert(display_inspection.find_field("refresh_hz") != nullptr);
    assert(!heartstead::platform::validate_window_desc({"", 800, 600, true}));
    auto invalid_window = platform.create_window({"", 800, 600, true});
    assert(!invalid_window);

    auto window = platform.create_window({"Heartstead Test", 800, 600, true, true});
    assert(window);
    assert(platform.open_window_count() == 1);

    const auto* state = platform.find_window(window.value());
    assert(state != nullptr);
    assert(state->width == 800);
    assert(state->height == 600);
    assert(state->fullscreen);

    auto first_event = platform.poll_event();
    assert(first_event);
    assert(first_event->kind == heartstead::platform::PlatformEventKind::window_created);

    auto invalid_resize =
        platform.queue_event({heartstead::platform::PlatformEventKind::window_resized,
                              window.value(),
                              heartstead::platform::KeyCode::unknown,
                              0,
                              768,
                              {}});
    assert(!invalid_resize);
    assert(invalid_resize.error().code == "platform.invalid_window_size");

    auto invalid_key = platform.queue_event({heartstead::platform::PlatformEventKind::key_down,
                                             window.value(),
                                             heartstead::platform::KeyCode::unknown,
                                             0,
                                             0,
                                             {}});
    assert(!invalid_key);
    assert(invalid_key.error().code == "platform.invalid_key");

    auto invalid_mouse =
        platform.queue_event({heartstead::platform::PlatformEventKind::mouse_button_down,
                              window.value(),
                              heartstead::platform::KeyCode::unknown,
                              0,
                              0,
                              {},
                              heartstead::platform::MouseButton::unknown,
                              16,
                              24,
                              0,
                              0});
    assert(!invalid_mouse);
    assert(invalid_mouse.error().code == "platform.invalid_mouse_button");

    auto invalid_wheel = platform.queue_event({heartstead::platform::PlatformEventKind::mouse_wheel,
                                               window.value(),
                                               heartstead::platform::KeyCode::unknown,
                                               0,
                                               0,
                                               {},
                                               heartstead::platform::MouseButton::unknown,
                                               16,
                                               24,
                                               0,
                                               0});
    assert(!invalid_wheel);
    assert(invalid_wheel.error().code == "platform.invalid_mouse_wheel");

    assert(platform.queue_event({heartstead::platform::PlatformEventKind::window_resized,
                                 window.value(),
                                 heartstead::platform::KeyCode::unknown,
                                 1024,
                                 768,
                                 {}}));
    state = platform.find_window(window.value());
    assert(state != nullptr);
    assert(state->width == 1024);
    assert(state->height == 768);

    assert(
        platform.queue_event({heartstead::platform::PlatformEventKind::text_input, window.value(),
                              heartstead::platform::KeyCode::unknown, 0, 0, "hello"}));
    assert(platform.queue_event({heartstead::platform::PlatformEventKind::key_down,
                                 window.value(),
                                 heartstead::platform::KeyCode::escape,
                                 0,
                                 0,
                                 {}}));
    assert(platform.queue_event({heartstead::platform::PlatformEventKind::mouse_moved,
                                 window.value(),
                                 heartstead::platform::KeyCode::unknown,
                                 0,
                                 0,
                                 {},
                                 heartstead::platform::MouseButton::unknown,
                                 320,
                                 180,
                                 0,
                                 0}));
    assert(platform.queue_event({heartstead::platform::PlatformEventKind::mouse_button_down,
                                 window.value(),
                                 heartstead::platform::KeyCode::unknown,
                                 0,
                                 0,
                                 {},
                                 heartstead::platform::MouseButton::left,
                                 320,
                                 180,
                                 0,
                                 0}));

    std::uint32_t key_events = 0;
    std::uint32_t text_events = 0;
    std::uint32_t mouse_move_events = 0;
    std::uint32_t mouse_button_events = 0;
    std::uint32_t mouse_wheel_events = 0;
    std::uint32_t text_snapshots = 0;
    std::uint32_t pressed_frames = 0;
    std::uint32_t released_frames = 0;
    std::uint32_t mouse_pressed_frames = 0;
    std::uint32_t mouse_released_frames = 0;
    const auto window_id = window.value();
    auto run_status = heartstead::platform::run_headless_app(
        platform, heartstead::platform::AppRunConfig{4},
        [window_id, &key_events, &text_events, &mouse_move_events, &mouse_button_events,
         &mouse_wheel_events, &text_snapshots, &pressed_frames, &released_frames,
         &mouse_pressed_frames,
         &mouse_released_frames](heartstead::platform::HeadlessPlatform& active_platform,
                                 std::uint64_t frame, std::int64_t) {
            while (auto event = active_platform.poll_event()) {
                if (event->kind == heartstead::platform::PlatformEventKind::key_down &&
                    event->key == heartstead::platform::KeyCode::escape) {
                    ++key_events;
                }
                if (event->kind == heartstead::platform::PlatformEventKind::text_input &&
                    event->text == "hello") {
                    ++text_events;
                }
                if (event->kind == heartstead::platform::PlatformEventKind::mouse_moved &&
                    event->mouse_x == 320 && event->mouse_y == 180) {
                    ++mouse_move_events;
                }
                if ((event->kind == heartstead::platform::PlatformEventKind::mouse_button_down ||
                     event->kind == heartstead::platform::PlatformEventKind::mouse_button_up) &&
                    event->mouse_button == heartstead::platform::MouseButton::left) {
                    ++mouse_button_events;
                }
                if (event->kind == heartstead::platform::PlatformEventKind::mouse_wheel &&
                    event->wheel_delta_y == -1) {
                    ++mouse_wheel_events;
                }
            }
            const auto snapshot = active_platform.input_snapshot(window_id);
            assert(snapshot);
            if (!snapshot->text.empty()) {
                assert(snapshot->text.size() == 1);
                assert(snapshot->text.front() == "hello");
                ++text_snapshots;
            }
            if (active_platform.was_key_pressed(window_id, heartstead::platform::KeyCode::escape)) {
                ++pressed_frames;
                assert(
                    active_platform.is_key_down(window_id, heartstead::platform::KeyCode::escape));
                assert(snapshot->pressed_keys.size() == 1);
                assert(snapshot->pressed_keys.front() == heartstead::platform::KeyCode::escape);
                auto queued = active_platform.queue_event({
                    heartstead::platform::PlatformEventKind::key_up,
                    window_id,
                    heartstead::platform::KeyCode::escape,
                    0,
                    0,
                    {},
                });
                assert(queued);
            }
            if (active_platform.was_mouse_button_pressed(window_id,
                                                         heartstead::platform::MouseButton::left)) {
                ++mouse_pressed_frames;
                assert(active_platform.is_mouse_button_down(
                    window_id, heartstead::platform::MouseButton::left));
                assert(snapshot->mouse.inside);
                assert(snapshot->mouse.x == 320);
                assert(snapshot->mouse.y == 180);
                assert(snapshot->pressed_mouse_buttons.size() == 1);
                assert(snapshot->pressed_mouse_buttons.front() ==
                       heartstead::platform::MouseButton::left);
                auto released = active_platform.queue_event({
                    heartstead::platform::PlatformEventKind::mouse_button_up,
                    window_id,
                    heartstead::platform::KeyCode::unknown,
                    0,
                    0,
                    {},
                    heartstead::platform::MouseButton::left,
                    321,
                    181,
                    0,
                    0,
                });
                assert(released);
                auto wheel = active_platform.queue_event({
                    heartstead::platform::PlatformEventKind::mouse_wheel,
                    window_id,
                    heartstead::platform::KeyCode::unknown,
                    0,
                    0,
                    {},
                    heartstead::platform::MouseButton::unknown,
                    321,
                    181,
                    0,
                    -1,
                });
                assert(wheel);
            }
            if (active_platform.was_key_released(window_id,
                                                 heartstead::platform::KeyCode::escape)) {
                ++released_frames;
                assert(
                    !active_platform.is_key_down(window_id, heartstead::platform::KeyCode::escape));
            }
            if (active_platform.was_mouse_button_released(
                    window_id, heartstead::platform::MouseButton::left)) {
                ++mouse_released_frames;
                assert(!active_platform.is_mouse_button_down(
                    window_id, heartstead::platform::MouseButton::left));
                assert(snapshot->released_mouse_buttons.size() == 1);
                assert(snapshot->released_mouse_buttons.front() ==
                       heartstead::platform::MouseButton::left);
                assert(snapshot->wheel_delta_x == 0);
                assert(snapshot->wheel_delta_y == -1);
                assert(snapshot->mouse.x == 321);
                assert(snapshot->mouse.y == 181);
            }
            if (frame == 2) {
                assert(!active_platform.was_key_pressed(window_id,
                                                        heartstead::platform::KeyCode::escape));
                assert(!active_platform.was_key_released(window_id,
                                                         heartstead::platform::KeyCode::escape));
                assert(!active_platform.was_mouse_button_pressed(
                    window_id, heartstead::platform::MouseButton::left));
                assert(!active_platform.was_mouse_button_released(
                    window_id, heartstead::platform::MouseButton::left));
                assert(snapshot->wheel_delta_x == 0);
                assert(snapshot->wheel_delta_y == 0);
                active_platform.request_quit();
            }
            return heartstead::core::Status::ok();
        });

    assert(run_status);
    assert(key_events == 1);
    assert(text_events == 1);
    assert(mouse_move_events == 1);
    assert(mouse_button_events == 2);
    assert(mouse_wheel_events == 1);
    assert(text_snapshots == 1);
    assert(pressed_frames == 1);
    assert(released_frames == 1);
    assert(mouse_pressed_frames == 1);
    assert(mouse_released_frames == 1);
    assert(platform.should_quit());

    auto close_status = platform.close_window(window.value());
    assert(close_status);
    assert(platform.open_window_count() == 0);
    assert(heartstead::platform::platform_event_kind_name(
               heartstead::platform::PlatformEventKind::window_closed) == "window_closed");
    assert(heartstead::platform::platform_event_kind_name(
               heartstead::platform::PlatformEventKind::mouse_wheel) == "mouse_wheel");
    assert(heartstead::platform::key_code_name(heartstead::platform::KeyCode::escape) == "escape");
    assert(heartstead::platform::key_code_name(heartstead::platform::KeyCode::f7) == "f7");
    assert(heartstead::platform::mouse_button_name(heartstead::platform::MouseButton::left) ==
           "left");
    assert(heartstead::platform::platform_backend_name(
               heartstead::platform::PlatformBackend::native) == "native");
}

void test_renderer_rhi() {
    using namespace heartstead::renderer::rhi;
    using namespace heartstead::renderer::materials;

    constexpr std::array<std::uint32_t, 35> minimal_compute_spirv{
        0x07230203, 0x00010000, 0x00000000, 0x00000006, 0x00000000, 0x00020011, 0x00000001,
        0x0003000e, 0x00000000, 0x00000001, 0x0005000f, 0x00000005, 0x00000004, 0x6e69616d,
        0x00000000, 0x00060010, 0x00000004, 0x00000011, 0x00000001, 0x00000001, 0x00000001,
        0x00020013, 0x00000001, 0x00030021, 0x00000002, 0x00000001, 0x00050036, 0x00000001,
        0x00000004, 0x00000000, 0x00000002, 0x000200f8, 0x00000005, 0x000100fd, 0x00010038,
    };
    constexpr std::array<std::uint32_t, 91> minimal_vertex_spirv{
        0x07230203, 0x00010000, 0x00000000, 0x00000011, 0x00000000, 0x00020011, 0x00000001,
        0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000000, 0x0000000e, 0x6e69616d,
        0x00000000, 0x00000008, 0x00050048, 0x00000005, 0x00000000, 0x0000000b, 0x00000000,
        0x00030047, 0x00000005, 0x00000002, 0x00020013, 0x00000001, 0x00030021, 0x00000002,
        0x00000001, 0x00030016, 0x00000003, 0x00000020, 0x00040017, 0x00000004, 0x00000003,
        0x00000004, 0x0003001e, 0x00000005, 0x00000004, 0x00040020, 0x00000006, 0x00000003,
        0x00000005, 0x00040020, 0x00000007, 0x00000003, 0x00000004, 0x0004003b, 0x00000006,
        0x00000008, 0x00000003, 0x0004002b, 0x00000003, 0x00000009, 0x00000000, 0x0004002b,
        0x00000003, 0x0000000a, 0x3f800000, 0x00040015, 0x0000000b, 0x00000020, 0x00000001,
        0x0004002b, 0x0000000b, 0x0000000c, 0x00000000, 0x0007002c, 0x00000004, 0x0000000d,
        0x00000009, 0x00000009, 0x00000009, 0x0000000a, 0x00050036, 0x00000001, 0x0000000e,
        0x00000000, 0x00000002, 0x000200f8, 0x0000000f, 0x00050041, 0x00000007, 0x00000010,
        0x00000008, 0x0000000c, 0x0003003e, 0x00000010, 0x0000000d, 0x000100fd, 0x00010038,
    };
    constexpr std::array<std::uint32_t, 70> minimal_fragment_spirv{
        0x07230203, 0x00010000, 0x00000000, 0x0000000c, 0x00000000, 0x00020011, 0x00000001,
        0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000004, 0x0000000a, 0x6e69616d,
        0x00000000, 0x00000006, 0x00030010, 0x0000000a, 0x00000007, 0x00040047, 0x00000006,
        0x0000001e, 0x00000000, 0x00020013, 0x00000001, 0x00030021, 0x00000002, 0x00000001,
        0x00030016, 0x00000003, 0x00000020, 0x00040017, 0x00000004, 0x00000003, 0x00000004,
        0x00040020, 0x00000005, 0x00000003, 0x00000004, 0x0004003b, 0x00000005, 0x00000006,
        0x00000003, 0x0004002b, 0x00000003, 0x00000007, 0x00000000, 0x0004002b, 0x00000003,
        0x00000008, 0x3f800000, 0x0007002c, 0x00000004, 0x00000009, 0x00000008, 0x00000007,
        0x00000008, 0x00000008, 0x00050036, 0x00000001, 0x0000000a, 0x00000000, 0x00000002,
        0x000200f8, 0x0000000b, 0x0003003e, 0x00000006, 0x00000009, 0x000100fd, 0x00010038,
    };
    constexpr std::array<std::uint32_t, 4> material_uniform_words{
        0x3f400000,
        0x3f19999a,
        0x3f333333,
        0x3f800000,
    };
    constexpr std::array<std::uint8_t, 16> material_texture_pixels{
        255, 96, 64, 255, 64, 180, 255, 255, 96, 255, 128, 255, 255, 240, 96, 255,
    };

    const auto headless_info = renderer_backend_info(RenderBackend::headless);
    assert(headless_info.available);
    assert(headless_info.name == "headless");

    const auto vulkan_info = renderer_backend_info(RenderBackend::vulkan);
    assert(vulkan_info.name == "vulkan");

    auto material_id = heartstead::core::PrototypeId::parse("base:materials/debug_clay");
    auto shader_template =
        heartstead::assets::VirtualPath::parse("base:shaders/templates/surface.slang");
    auto albedo_texture = heartstead::assets::VirtualPath::parse("base:textures/terrain/clay.ktx2");
    assert(material_id);
    assert(shader_template);
    assert(albedo_texture);

    MaterialDefinition material;
    material.id = material_id.value();
    material.domain = MaterialDomain::terrain;
    material.blend_mode = MaterialBlendMode::opaque;
    material.shader_template = shader_template.value();
    material.double_sided = true;
    material.textures.push_back({"albedo", albedo_texture.value(), true});
    material.scalars.push_back({"roughness", 0.75F});
    material.colors.push_back({"tint", MaterialColor{0.8F, 0.7F, 0.6F, 1.0F}});

    assert(validate_material_definition(material));

    MaterialRegistry materials;
    assert(materials.empty());
    assert(materials.add(material));
    assert(materials.size() == 1);
    assert(materials.definitions().size() == 1);
    assert(materials.count_domain(MaterialDomain::terrain) == 1);
    const auto* found_material = materials.find("base:materials/debug_clay");
    assert(found_material != nullptr);
    assert(found_material->textures.size() == 1);
    assert(found_material->double_sided);

    auto material_pipeline_layout = render_pipeline_layout_from_material(material);
    assert(material_pipeline_layout);
    assert(material_pipeline_layout.value().material_id == material.id);
    assert(material_pipeline_layout.value().shader_template.to_string() ==
           "base:shaders/templates/surface.slang");
    assert(material_pipeline_layout.value().descriptors.size() == 3);
    assert(material_pipeline_layout.value().descriptors.front().kind ==
           RenderDescriptorKind::sampled_texture);
    assert(render_descriptor_kind_name(RenderDescriptorKind::sampled_texture) == "sampled_texture");
    auto invalid_pipeline_layout_version = render_pipeline_layout_from_material(material, 0);
    assert(!invalid_pipeline_layout_version);
    assert(invalid_pipeline_layout_version.error().code == "renderer.invalid_pipeline_version");

    assert(!materials.add(material));

    auto bad_scalar = material;
    bad_scalar.id = heartstead::core::PrototypeId::parse("base:materials/bad_scalar").value();
    bad_scalar.scalars.front().value = std::numeric_limits<float>::infinity();
    auto bad_scalar_status = validate_material_definition(bad_scalar);
    assert(!bad_scalar_status);
    assert(bad_scalar_status.error().code == "material.invalid_scalar");

    auto duplicate_parameter = material;
    duplicate_parameter.id =
        heartstead::core::PrototypeId::parse("base:materials/duplicate_parameter").value();
    duplicate_parameter.scalars.push_back({"albedo", 1.0F});
    auto duplicate_parameter_status = validate_material_definition(duplicate_parameter);
    assert(!duplicate_parameter_status);
    assert(duplicate_parameter_status.error().code == "material.duplicate_parameter");

    auto bad_shader = material;
    bad_shader.id = heartstead::core::PrototypeId::parse("base:materials/bad_shader").value();
    bad_shader.shader_template = {};
    auto bad_shader_status = validate_material_definition(bad_shader);
    assert(!bad_shader_status);
    assert(bad_shader_status.error().code == "material.invalid_shader_template");

    auto bad_color = material;
    bad_color.id = heartstead::core::PrototypeId::parse("base:materials/bad_color").value();
    bad_color.colors.front().value.alpha = 2.0F;
    auto bad_color_status = validate_material_definition(bad_color);
    assert(!bad_color_status);
    assert(bad_color_status.error().code == "material.invalid_color");

    assert(material_domain_name(MaterialDomain::water) == "water");
    assert(material_blend_mode_name(MaterialBlendMode::additive) == "additive");
    assert(parse_material_domain("terrain") == MaterialDomain::terrain);
    assert(!parse_material_domain("unsupported"));
    assert(parse_material_blend_mode("translucent") == MaterialBlendMode::translucent);
    assert(!parse_material_blend_mode("multiply"));

    heartstead::modding::GenericPrototype material_prototype;
    material_prototype.kind = std::string(heartstead::modding::PrototypeKinds::material);
    material_prototype.id = heartstead::core::PrototypeId::parse("base:materials/clay").value();
    material_prototype.display_name = "Clay Material";
    material_prototype.fields = {
        {"kind", "material"},
        {"id", "base:materials/clay"},
        {"display_name", "Clay Material"},
        {"domain", "terrain"},
        {"blend_mode", "opaque"},
        {"shader_template", "base:shaders/templates/terrain.slang"},
        {"texture.albedo", "base:textures/voxels/clay.txt"},
        {"scalar.roughness", "0.85"},
        {"color.tint", "0.55,0.38,0.26,1.0"},
    };

    auto loaded_material = material_definition_from_prototype(material_prototype);
    assert(loaded_material);
    assert(loaded_material.value().id.value() == "base:materials/clay");
    assert(loaded_material.value().domain == MaterialDomain::terrain);
    assert(loaded_material.value().textures.size() == 1);
    assert(loaded_material.value().scalars.size() == 1);
    assert(loaded_material.value().colors.size() == 1);

    auto variant_material_prototype = material_prototype;
    variant_material_prototype.id =
        heartstead::core::PrototypeId::parse("base:materials/clay_variants").value();
    variant_material_prototype.fields["id"] = "base:materials/clay_variants";
    variant_material_prototype.fields["texture.top.variant.1"] =
        "base:textures/voxels/clay_top_variant.png";
    auto loaded_variant_material = material_definition_from_prototype(variant_material_prototype);
    assert(loaded_variant_material);
    assert(loaded_variant_material.value().textures.size() == 2);
    assert(std::ranges::any_of(
        loaded_variant_material.value().textures,
        [](const MaterialTextureBinding& binding) { return binding.name == "top.variant.1"; }));

    heartstead::modding::PrototypeRegistry material_prototypes;
    auto material_prototype_registry_result = material_prototypes.build({material_prototype});
    assert(!material_prototype_registry_result.has_errors());
    auto loaded_materials = material_registry_from_prototypes(material_prototypes);
    assert(loaded_materials);
    assert(loaded_materials.value().size() == 1);
    assert(loaded_materials.value().find("base:materials/clay") != nullptr);

    auto invalid_material_prototype = material_prototype;
    invalid_material_prototype.id =
        heartstead::core::PrototypeId::parse("base:materials/bad_material").value();
    invalid_material_prototype.fields["id"] = "base:materials/bad_material";
    invalid_material_prototype.fields["scalar.roughness"] = "nan";
    auto invalid_loaded_material = material_definition_from_prototype(invalid_material_prototype);
    assert(!invalid_loaded_material);
    assert(invalid_loaded_material.error().code == "material_prototype.invalid_scalar");

    auto terrain_shader_asset =
        heartstead::assets::VirtualPath::parse("base:shaders/templates/terrain.slang");
    auto clay_texture_asset =
        heartstead::assets::VirtualPath::parse("base:textures/voxels/clay.txt");
    auto hd_clay_texture_asset =
        heartstead::assets::VirtualPath::parse("base:textures/voxels/clay.txt");
    auto moss_texture_asset =
        heartstead::assets::VirtualPath::parse("base:textures/voxels/moss.txt");
    assert(terrain_shader_asset);
    assert(clay_texture_asset);
    assert(hd_clay_texture_asset);
    assert(moss_texture_asset);

    heartstead::assets::AssetCatalog material_asset_catalog;
    assert(material_asset_catalog.add(heartstead::assets::AssetRecord{
        heartstead::assets::asset_logical_id(terrain_shader_asset.value()),
        heartstead::assets::AssetKind::shader,
        terrain_shader_asset.value(),
        heartstead::assets::AssetSourceKind::mod,
        "base",
        0,
        std::filesystem::path("mods/base/assets/shaders/templates/terrain.slang"),
        "terrain_shader_hash",
        false,
        {},
    }));
    assert(material_asset_catalog.add(heartstead::assets::AssetRecord{
        heartstead::assets::asset_logical_id(clay_texture_asset.value()),
        heartstead::assets::AssetKind::texture,
        clay_texture_asset.value(),
        heartstead::assets::AssetSourceKind::mod,
        "base",
        0,
        std::filesystem::path("mods/base/assets/textures/voxels/clay.txt"),
        "clay_texture_hash",
        false,
        {},
    }));
    assert(material_asset_catalog.add(heartstead::assets::AssetRecord{
        heartstead::assets::asset_logical_id(hd_clay_texture_asset.value()),
        heartstead::assets::AssetKind::texture,
        hd_clay_texture_asset.value(),
        heartstead::assets::AssetSourceKind::resource_pack,
        "hd_pack",
        1000,
        std::filesystem::path("resource_packs/hd_pack/assets/textures/voxels/clay.txt"),
        "hd_clay_texture_hash",
        false,
        {},
    }));

    auto asset_checked_material = loaded_material.value();
    asset_checked_material.textures.push_back({"moss", moss_texture_asset.value(), false});
    MaterialRegistry asset_checked_materials;
    assert(asset_checked_materials.add(asset_checked_material));
    auto material_asset_report =
        validate_material_asset_references(asset_checked_materials, material_asset_catalog);
    assert(!material_asset_report.has_errors());
    assert(material_asset_report.references.size() == 2);
    assert(material_asset_report.count_severity(heartstead::modding::DiagnosticSeverity::warning) ==
           1);
    assert(material_asset_report.count_severity(heartstead::modding::DiagnosticSeverity::error) ==
           0);
    assert(material_asset_report.override_count() == 1);

    const auto texture_reference = std::ranges::find_if(
        material_asset_report.references, [](const MaterialAssetReference& reference) {
            return reference.kind == MaterialAssetReferenceKind::texture &&
                   reference.binding_name == "albedo";
        });
    assert(texture_reference != material_asset_report.references.end());
    assert(texture_reference->declared_path.to_string() == "base:textures/voxels/clay.txt");
    assert(texture_reference->active_path.to_string() == "base:textures/voxels/clay.txt");
    assert(texture_reference->source_kind == heartstead::assets::AssetSourceKind::resource_pack);
    assert(texture_reference->overridden);
    assert(material_asset_reference_kind_name(MaterialAssetReferenceKind::shader_template) ==
           "shader_template");

    auto missing_texture_material = loaded_material.value();
    missing_texture_material.id =
        heartstead::core::PrototypeId::parse("base:materials/missing_texture").value();
    missing_texture_material.textures = {
        {"albedo",
         heartstead::assets::VirtualPath::parse("base:textures/voxels/missing.txt").value(), true},
    };
    MaterialRegistry missing_texture_materials;
    assert(missing_texture_materials.add(missing_texture_material));
    auto missing_texture_report =
        validate_material_asset_references(missing_texture_materials, material_asset_catalog);
    assert(missing_texture_report.has_errors());
    assert(missing_texture_report.count_severity(heartstead::modding::DiagnosticSeverity::error) ==
           1);
    assert(missing_texture_report.diagnostics.front().code == "material_assets.missing_texture");

    auto clear_plan = make_clear_present_frame_plan(RenderExtent{320, 180},
                                                    ClearColor{0.1F, 0.2F, 0.3F, 1.0F}, true);
    assert(clear_plan.validate());
    assert(clear_plan.resources.size() == 1);
    assert(clear_plan.passes.size() == 2);
    assert(clear_plan.pass_count(RenderPassKind::clear) == 1);
    assert(clear_plan.pass_count(RenderPassKind::present) == 1);
    assert(clear_plan.has_present_pass());
    assert(clear_plan.find_resource("swapchain") != nullptr);
    assert(clear_plan.find_resource("swapchain")->lifetime == RenderResourceLifetime::external);

    RenderFramePlanBuilder custom_plan(RenderExtent{640, 360});
    assert(custom_plan.add_resource(
        {"swapchain", RenderExtent{640, 360}, RenderResourceLifetime::external}));
    assert(custom_plan.add_resource(
        {"scene_color", RenderExtent{640, 360}, RenderResourceLifetime::transient}));
    assert(custom_plan.add_pass({"world", RenderPassKind::world, {}, {"scene_color"}, {}, false}));
    assert(custom_plan.add_pass(
        {"post", RenderPassKind::post_process, {"scene_color"}, {"swapchain"}, {}, false}));
    assert(custom_plan.add_pass({"present", RenderPassKind::present, {"swapchain"}, {}, {}, true}));
    auto built_plan = custom_plan.build();
    assert(built_plan);
    assert(built_plan.value().passes.size() == 3);
    assert(built_plan.value().pass_count(RenderPassKind::post_process) == 1);
    auto execution_plan = built_plan.value().build_execution_plan();
    assert(execution_plan);
    assert(execution_plan.value().ordered_passes.size() == 3);
    assert(execution_plan.value().ordered_passes[0] == "world");
    assert(execution_plan.value().resource_uses.size() == 4);
    assert(execution_plan.value().dependencies.size() == 2);
    assert(execution_plan.value().transitions.size() == 4);
    assert(execution_plan.value().resource_uses[0].required_state ==
           RenderResourceState::color_attachment_write);
    assert(execution_plan.value().resource_uses[1].required_state ==
           RenderResourceState::shader_read);
    assert(execution_plan.value().resource_uses[2].required_state ==
           RenderResourceState::color_attachment_write);
    assert(execution_plan.value().resource_uses[3].required_state == RenderResourceState::present);
    assert(execution_plan.value().dependencies[0].resource_name == "scene_color");
    assert(execution_plan.value().dependencies[0].source_pass_index == 0);
    assert(execution_plan.value().dependencies[0].destination_pass_index == 1);
    assert(execution_plan.value().dependencies[0].source_access == RenderResourceAccess::write);
    assert(execution_plan.value().dependencies[0].destination_access == RenderResourceAccess::read);
    assert(execution_plan.value().dependencies[1].resource_name == "swapchain");
    assert(execution_plan.value().dependencies[1].destination_access ==
           RenderResourceAccess::present);
    assert(!execution_plan.value().transitions[0].has_source_use);
    assert(execution_plan.value().transitions[0].resource_name == "scene_color");
    assert(execution_plan.value().transitions[0].before_state == RenderResourceState::undefined);
    assert(execution_plan.value().transitions[0].after_state ==
           RenderResourceState::color_attachment_write);
    assert(execution_plan.value().transitions[1].has_source_use);
    assert(execution_plan.value().transitions[1].before_state ==
           RenderResourceState::color_attachment_write);
    assert(execution_plan.value().transitions[1].after_state == RenderResourceState::shader_read);
    assert(execution_plan.value().transitions[2].resource_name == "swapchain");
    assert(execution_plan.value().transitions[2].before_state == RenderResourceState::external);
    assert(execution_plan.value().transitions[3].after_state == RenderResourceState::present);
    assert(render_resource_access_name(RenderResourceAccess::read_write) == "read_write");
    assert(render_resource_access_name(RenderResourceAccess::present) == "present");
    assert(render_resource_state_name(RenderResourceState::color_attachment_read_write) ==
           "color_attachment_read_write");
    assert(render_resource_state_name(RenderResourceState::present) == "present");

    RenderFramePlanBuilder read_then_write_plan(RenderExtent{64, 64});
    assert(read_then_write_plan.add_resource(
        {"history", RenderExtent{64, 64}, RenderResourceLifetime::external}));
    assert(read_then_write_plan.add_pass(
        {"sample_history", RenderPassKind::debug, {"history"}, {}, {}, false}));
    assert(read_then_write_plan.add_pass(
        {"overwrite_history", RenderPassKind::post_process, {}, {"history"}, {}, false}));
    auto read_then_write_built = read_then_write_plan.build();
    assert(read_then_write_built);
    auto read_then_write_execution = read_then_write_built.value().build_execution_plan();
    assert(read_then_write_execution);
    assert(read_then_write_execution.value().resource_uses.size() == 2);
    assert(read_then_write_execution.value().dependencies.size() == 1);
    assert(read_then_write_execution.value().transitions.size() == 2);
    assert(read_then_write_execution.value().dependencies[0].source_access ==
           RenderResourceAccess::read);
    assert(read_then_write_execution.value().dependencies[0].destination_access ==
           RenderResourceAccess::write);
    assert(read_then_write_execution.value().transitions[0].before_state ==
           RenderResourceState::external);
    assert(read_then_write_execution.value().transitions[0].after_state ==
           RenderResourceState::shader_read);
    assert(read_then_write_execution.value().transitions[1].before_state ==
           RenderResourceState::shader_read);
    assert(read_then_write_execution.value().transitions[1].after_state ==
           RenderResourceState::color_attachment_write);

    RenderFramePlanBuilder duplicate_resource(RenderExtent{64, 64});
    assert(duplicate_resource.add_resource(
        {"color", RenderExtent{64, 64}, RenderResourceLifetime::transient}));
    assert(!duplicate_resource.add_resource(
        {"color", RenderExtent{64, 64}, RenderResourceLifetime::transient}));

    RenderFramePlanBuilder duplicate_ref(RenderExtent{64, 64});
    assert(duplicate_ref.add_resource(
        {"color", RenderExtent{64, 64}, RenderResourceLifetime::external}));
    assert(duplicate_ref.add_pass(
        {"bad_debug", RenderPassKind::debug, {"color", "color"}, {}, {}, false}));
    auto duplicate_ref_result = duplicate_ref.build();
    assert(!duplicate_ref_result);
    assert(duplicate_ref_result.error().code == "renderer_plan.duplicate_pass_resource_ref");

    RenderFramePlanBuilder read_before_write(RenderExtent{64, 64});
    assert(read_before_write.add_resource(
        {"color", RenderExtent{64, 64}, RenderResourceLifetime::transient}));
    assert(read_before_write.add_pass({"debug", RenderPassKind::debug, {"color"}, {}, {}, false}));
    auto read_before_write_result = read_before_write.build();
    assert(!read_before_write_result);
    assert(read_before_write_result.error().code == "renderer_plan.read_before_write");

    RenderFramePlanBuilder invalid_present(RenderExtent{64, 64});
    assert(invalid_present.add_resource(
        {"swapchain", RenderExtent{64, 64}, RenderResourceLifetime::external}));
    assert(invalid_present.add_pass(
        {"present_bad", RenderPassKind::debug, {"swapchain"}, {}, {}, true}));
    auto invalid_present_result = invalid_present.build();
    assert(!invalid_present_result);
    assert(invalid_present_result.error().code == "renderer_plan.invalid_present_pass");

    RenderFramePlanBuilder transient_present(RenderExtent{64, 64});
    assert(transient_present.add_resource(
        {"color", RenderExtent{64, 64}, RenderResourceLifetime::transient}));
    assert(transient_present.add_pass({"clear", RenderPassKind::clear, {}, {"color"}, {}, false}));
    assert(
        transient_present.add_pass({"present", RenderPassKind::present, {"color"}, {}, {}, true}));
    auto transient_present_result = transient_present.build();
    assert(!transient_present_result);
    assert(transient_present_result.error().code ==
           "renderer_plan.present_without_external_resource");

    RenderFramePlanBuilder ambiguous_present(RenderExtent{64, 64});
    assert(ambiguous_present.add_resource(
        {"swapchain", RenderExtent{64, 64}, RenderResourceLifetime::external}));
    assert(ambiguous_present.add_resource(
        {"mirror", RenderExtent{64, 64}, RenderResourceLifetime::external}));
    assert(ambiguous_present.add_pass(
        {"present", RenderPassKind::present, {"swapchain", "mirror"}, {}, {}, true}));
    auto ambiguous_present_result = ambiguous_present.build();
    assert(!ambiguous_present_result);
    assert(ambiguous_present_result.error().code == "renderer_plan.ambiguous_present_resource");

    RenderDeviceDesc invalid_name;
    invalid_name.application_name.clear();
    assert(!create_render_device(invalid_name));

    RenderDeviceDesc invalid_extent;
    invalid_extent.initial_extent = RenderExtent{0, 720};
    assert(!create_render_device(invalid_extent));

    const auto headless_backend_info = renderer_backend_info(RenderBackend::headless);
    assert(headless_backend_info.available);
    assert(headless_backend_info.name == "headless");
    const auto vulkan_backend_info = renderer_backend_info(RenderBackend::vulkan);
    assert(vulkan_backend_info.name == "vulkan");
    const auto headless_backend_caps = render_backend_capabilities(RenderBackend::headless);
    assert(headless_backend_caps.available);
    assert(headless_backend_caps.supports_headless);
    assert(headless_backend_caps.supports_shader_modules);
    assert(headless_backend_caps.supports_pipeline_layout);
    assert(headless_backend_caps.supports_compute_pipelines);
    assert(headless_backend_caps.supports_graphics_pipelines);
    assert(headless_backend_caps.supports_descriptor_writes);
    assert(headless_backend_caps.supports_buffer_upload);
    assert(headless_backend_caps.supports_image_upload);
    assert(headless_backend_caps.supports_draw_binding);
    assert(!headless_backend_caps.requires_gpu_device);
    assert(headless_backend_caps.graphics_api == "headless");
    const auto vulkan_backend_caps = render_backend_capabilities(RenderBackend::vulkan);
    assert(vulkan_backend_caps.available == vulkan_backend_info.available);
    assert(vulkan_backend_caps.supports_present);
    assert(vulkan_backend_caps.supports_validation);
    assert(vulkan_backend_caps.supports_debug_markers);
    assert(vulkan_backend_caps.supports_shader_modules);
    assert(vulkan_backend_caps.supports_pipeline_layout);
    assert(vulkan_backend_caps.supports_compute_pipelines);
    assert(vulkan_backend_caps.supports_graphics_pipelines);
    assert(vulkan_backend_caps.supports_descriptor_writes);
    assert(vulkan_backend_caps.supports_buffer_upload);
    assert(vulkan_backend_caps.supports_image_upload);
    assert(vulkan_backend_caps.supports_draw_binding);
    assert(vulkan_backend_caps.requires_window_surface);
    assert(vulkan_backend_caps.requires_gpu_device);
    assert(!vulkan_backend_caps.supports_headless);
    assert(vulkan_backend_caps.recommended_frames_in_flight == 2);
    assert(vulkan_backend_caps.graphics_api == "vulkan");

    RenderDeviceDesc desc;
    desc.backend = RenderBackend::headless;
    desc.application_name = "Heartstead Renderer Test";
    desc.initial_extent = RenderExtent{640, 360};

    auto invalid_frame_count_desc = desc;
    invalid_frame_count_desc.frames_in_flight = 0;
    auto invalid_frame_count_device = create_render_device(invalid_frame_count_desc);
    assert(!invalid_frame_count_device);
    assert(invalid_frame_count_device.error().code == "renderer.invalid_frames_in_flight");

    auto device = create_render_device(desc);
    assert(device);
    assert(device.value()->backend() == RenderBackend::headless);
    assert(device.value()->backend_name() == "headless");
    const auto capabilities = device.value()->capabilities();
    assert(capabilities.backend == RenderBackend::headless);
    assert(capabilities.max_extent.width >= desc.initial_extent.width);
    assert(capabilities.max_extent.height >= desc.initial_extent.height);
    assert(capabilities.supports_present);
    assert(capabilities.supports_validation);
    assert(capabilities.supports_debug_markers);
    assert(capabilities.supports_shader_modules);
    assert(capabilities.supports_pipeline_layout);
    assert(capabilities.supports_compute_pipelines);
    assert(capabilities.supports_graphics_pipelines);
    assert(capabilities.supports_descriptor_writes);
    assert(capabilities.supports_buffer_upload);
    assert(capabilities.supports_image_upload);
    assert(capabilities.supports_draw_binding);
    assert(capabilities.headless);
    assert(device.value()->live_resource_count() == 0);
    assert(device.value()->completed_frame_count() == 0);
    assert(device.value()->last_submission_serial() == 0);
    assert(device.value()->completed_submission_serial() == 0);
    assert(device.value()->current_extent().width == 640);
    assert(device.value()->current_extent().height == 360);

    auto frame = device.value()->render_frame(
        RenderFrameDesc{ClearColor{0.25F, 0.5F, 0.75F, 1.0F}, {}, true});
    assert(frame);
    assert(frame.value().frame_index == 0);
    assert(frame.value().submission_serial == 1);
    assert(frame.value().completed_submission_serial == 1);
    assert(frame.value().extent.width == 640);
    assert(frame.value().extent.height == 360);
    assert(frame.value().clear_color.blue == 0.75F);
    assert(frame.value().presented);
    assert(frame.value().render_pass_count == 2);
    assert(frame.value().present_pass_count == 1);
    assert(frame.value().resource_use_count == 2);
    assert(frame.value().dependency_count == 1);
    assert(frame.value().transition_count == 2);
    assert(frame.value().synchronization_barrier_count == 2);
    assert(frame.value().submitted_synchronization_barrier_count == 2);
    assert(device.value()->completed_frame_count() == 1);
    assert(device.value()->last_submission_serial() == 1);
    assert(device.value()->completed_submission_serial() == 1);

    auto resized = device.value()->resize(RenderExtent{800, 450});
    assert(resized);
    auto second_frame = device.value()->render_frame(
        RenderFrameDesc{ClearColor{0.1F, 0.2F, 0.3F, 1.0F}, {}, false});
    assert(second_frame);
    assert(second_frame.value().frame_index == 1);
    assert(second_frame.value().extent.width == 800);
    assert(second_frame.value().extent.height == 450);
    assert(!second_frame.value().presented);
    assert(second_frame.value().render_pass_count == 1);
    assert(second_frame.value().present_pass_count == 0);
    assert(second_frame.value().resource_use_count == 1);
    assert(second_frame.value().dependency_count == 0);
    assert(second_frame.value().transition_count == 1);
    assert(second_frame.value().synchronization_barrier_count == 1);
    assert(second_frame.value().submitted_synchronization_barrier_count == 1);

    auto invalid_frame =
        device.value()->render_frame(RenderFrameDesc{ClearColor{}, RenderExtent{1280, 0}, true});
    assert(!invalid_frame);

    RenderFramePlanBuilder headless_execution_plan(RenderExtent{320, 200});
    assert(headless_execution_plan.add_resource(
        {"swapchain", RenderExtent{320, 200}, RenderResourceLifetime::external}));
    assert(headless_execution_plan.add_resource(
        {"scene_color", RenderExtent{320, 200}, RenderResourceLifetime::transient}));
    assert(headless_execution_plan.add_pass({"clear_scene",
                                             RenderPassKind::clear,
                                             {},
                                             {"scene_color"},
                                             ClearColor{0.3F, 0.2F, 0.1F, 1.0F},
                                             false}));
    assert(headless_execution_plan.add_pass(
        {"world", RenderPassKind::world, {"scene_color"}, {"scene_color"}, {}, false}));
    assert(headless_execution_plan.add_pass(
        {"post", RenderPassKind::post_process, {"scene_color"}, {"swapchain"}, {}, false}));
    assert(headless_execution_plan.add_pass(
        {"present", RenderPassKind::present, {"swapchain"}, {}, {}, true}));
    auto headless_plan = headless_execution_plan.build();
    assert(headless_plan);
    assert(headless_plan.value().first_clear_color().red == 0.3F);
    auto planned_frame = device.value()->execute_frame_plan(headless_plan.value());
    assert(planned_frame);
    assert(planned_frame.value().backend == RenderBackend::headless);
    assert(planned_frame.value().frame_index == 2);
    assert(planned_frame.value().extent.width == 320);
    assert(planned_frame.value().extent.height == 200);
    assert(planned_frame.value().presented);
    assert(planned_frame.value().clear_color.red == 0.3F);
    assert(planned_frame.value().render_pass_count == 4);
    assert(planned_frame.value().present_pass_count == 1);
    assert(planned_frame.value().resource_use_count == 5);
    assert(planned_frame.value().dependency_count == 3);
    assert(planned_frame.value().transition_count == 5);
    assert(planned_frame.value().synchronization_barrier_count == 5);
    assert(planned_frame.value().submitted_synchronization_barrier_count == 5);
    assert(device.value()->completed_frame_count() == 3);

    const RenderShaderModuleDesc compute_shader_desc{
        RenderShaderStage::compute,
        "minimal_compute_shader",
    };
    auto invalid_shader_module =
        device.value()->create_shader_module(compute_shader_desc, std::span<const std::uint32_t>{});
    assert(!invalid_shader_module);
    assert(invalid_shader_module.error().code == "renderer.empty_shader_module");
    auto headless_shader_module =
        device.value()->create_shader_module(compute_shader_desc, minimal_compute_spirv);
    assert(headless_shader_module);
    assert(headless_shader_module.value().backend == RenderBackend::headless);
    assert(headless_shader_module.value().handle.is_valid());
    assert(headless_shader_module.value().stage == RenderShaderStage::compute);
    assert(headless_shader_module.value().word_count == minimal_compute_spirv.size());
    assert(headless_shader_module.value().live_shader_module_count == 1);
    assert(!headless_shader_module.value().gpu_backed);
    assert(render_shader_stage_name(RenderShaderStage::compute) == "compute");
    assert(device.value()->live_resource_count() == 1);
    assert(device.value()->release_resource(headless_shader_module.value().handle));
    assert(device.value()->live_resource_count() == 0);
    assert(!device.value()->release_resource(headless_shader_module.value().handle));

    heartstead::world::VoxelChunk render_upload_chunk({0, 0, 0});
    assert(render_upload_chunk.set({0, 0, 0}, heartstead::world::VoxelCell{1, 15}));
    auto render_upload_mesh =
        heartstead::world::ChunkMesher::build_surface_mesh(render_upload_chunk);
    assert(render_upload_mesh);
    const auto vertex_upload_bytes = std::as_bytes(std::span(render_upload_mesh.value().vertices));
    const auto index_upload_bytes = std::as_bytes(std::span(render_upload_mesh.value().indices));
    auto invalid_upload = device.value()->upload_buffer(
        RenderBufferDesc{RenderBufferUsage::vertex, vertex_upload_bytes.size() + 1,
                         "invalid_chunk_vertices"},
        vertex_upload_bytes);
    assert(!invalid_upload);
    assert(invalid_upload.error().code == "renderer.buffer_upload_size_mismatch");

    auto vertex_upload = device.value()->upload_buffer(
        RenderBufferDesc{RenderBufferUsage::vertex, vertex_upload_bytes.size(), "chunk_vertices"},
        vertex_upload_bytes);
    assert(vertex_upload);
    assert(vertex_upload.value().backend == RenderBackend::headless);
    assert(vertex_upload.value().handle.is_valid());
    assert(vertex_upload.value().usage == RenderBufferUsage::vertex);
    assert(vertex_upload.value().byte_size == vertex_upload_bytes.size());
    assert(vertex_upload.value().live_resource_count == 1);
    assert(!vertex_upload.value().gpu_backed);
    assert(device.value()->live_resource_count() == 1);

    auto index_upload = device.value()->upload_buffer(
        RenderBufferDesc{RenderBufferUsage::index, index_upload_bytes.size(), "chunk_indices"},
        index_upload_bytes);
    assert(index_upload);
    assert(index_upload.value().handle.is_valid());
    assert(index_upload.value().usage == RenderBufferUsage::index);
    assert(index_upload.value().byte_size == index_upload_bytes.size());
    assert(index_upload.value().live_resource_count == 2);
    assert(device.value()->live_resource_count() == 2);
    const RenderMeshBinding invalid_material_draw{
        vertex_upload.value().handle,
        index_upload.value().handle,
        {},
        static_cast<std::uint32_t>(render_upload_mesh.value().vertices.size()),
        static_cast<std::uint32_t>(render_upload_mesh.value().indices.size()),
        1,
        "invalid_material_draw",
    };
    auto invalid_draw = device.value()->bind_mesh_draws(
        std::span<const RenderMeshBinding>{&invalid_material_draw, 1});
    assert(!invalid_draw);
    assert(invalid_draw.error().code == "renderer.invalid_material");

    const RenderMeshBinding chunk_draw{
        vertex_upload.value().handle,
        index_upload.value().handle,
        material.id,
        static_cast<std::uint32_t>(render_upload_mesh.value().vertices.size()),
        static_cast<std::uint32_t>(render_upload_mesh.value().indices.size()),
        1,
        "headless_chunk_draw",
    };
    auto unbound_draw =
        device.value()->bind_mesh_draws(std::span<const RenderMeshBinding>{&chunk_draw, 1});
    assert(!unbound_draw);
    assert(unbound_draw.error().code == "renderer.unbound_material_graphics_pipeline");
    auto pipeline_stats = device.value()->bind_pipeline_layout(material_pipeline_layout.value());
    assert(pipeline_stats);
    assert(pipeline_stats.value().backend == RenderBackend::headless);
    assert(pipeline_stats.value().material_id == material.id);
    assert(pipeline_stats.value().pipeline_version == 1);
    assert(pipeline_stats.value().descriptor_count == 3);
    assert(pipeline_stats.value().sampled_texture_count == 1);
    assert(pipeline_stats.value().uniform_count == 2);
    assert(pipeline_stats.value().bound_pipeline_count == 1);
    assert(!pipeline_stats.value().gpu_backed);
    const auto material_uniform_bytes = std::as_bytes(std::span(material_uniform_words));
    auto uniform_upload = device.value()->upload_buffer(
        RenderBufferDesc{RenderBufferUsage::uniform, material_uniform_bytes.size(),
                         "headless_material_uniforms"},
        material_uniform_bytes);
    assert(uniform_upload);
    assert(uniform_upload.value().usage == RenderBufferUsage::uniform);
    assert(device.value()->live_resource_count() == 3);
    const auto material_texture_bytes = std::as_bytes(std::span(material_texture_pixels));
    auto invalid_image_upload = device.value()->upload_image(
        RenderImageDesc{RenderImageFormat::rgba8_unorm, 2, 2, "invalid_material_albedo"},
        std::span<const std::byte>{material_texture_bytes.data(),
                                   material_texture_bytes.size() - 1});
    assert(!invalid_image_upload);
    assert(invalid_image_upload.error().code == "renderer.image_upload_size_mismatch");
    auto texture_upload = device.value()->upload_image(
        RenderImageDesc{RenderImageFormat::rgba8_unorm, 2, 2, "headless_material_albedo"},
        material_texture_bytes);
    assert(texture_upload);
    assert(texture_upload.value().backend == RenderBackend::headless);
    assert(texture_upload.value().handle.is_valid());
    assert(texture_upload.value().format == RenderImageFormat::rgba8_unorm);
    assert(texture_upload.value().width == 2);
    assert(texture_upload.value().height == 2);
    assert(texture_upload.value().byte_size == material_texture_bytes.size());
    assert(texture_upload.value().cpu_gpu_wait_ms == 0.0);
    assert(!texture_upload.value().gpu_backed);
    assert(device.value()->live_resource_count() == 4);
    const RenderDescriptorWrite roughness_write{
        material.id, "roughness", uniform_upload.value().handle, 0, material_uniform_bytes.size(),
    };
    auto descriptor_write = device.value()->write_descriptors(
        std::span<const RenderDescriptorWrite>{&roughness_write, 1});
    assert(descriptor_write);
    assert(descriptor_write.value().backend == RenderBackend::headless);
    assert(descriptor_write.value().write_count == 1);
    assert(descriptor_write.value().uniform_write_count == 1);
    assert(descriptor_write.value().sampled_texture_write_count == 0);
    assert(descriptor_write.value().material_count == 1);
    assert(!descriptor_write.value().gpu_backed);
    const RenderDescriptorWrite tint_write{
        material.id, "tint", uniform_upload.value().handle, 0, material_uniform_bytes.size(),
    };
    assert(
        device.value()->write_descriptors(std::span<const RenderDescriptorWrite>{&tint_write, 1}));
    const RenderDescriptorWrite invalid_sampled_texture_write{
        material.id,
        "albedo",
        uniform_upload.value().handle,
    };
    auto invalid_texture_descriptor = device.value()->write_descriptors(
        std::span<const RenderDescriptorWrite>{&invalid_sampled_texture_write, 1});
    assert(!invalid_texture_descriptor);
    assert(invalid_texture_descriptor.error().code == "renderer.invalid_descriptor_resource_usage");
    const RenderDescriptorWrite sampled_texture_write{
        material.id,
        "albedo",
        texture_upload.value().handle,
    };
    auto texture_descriptor =
        device.value()->write_descriptors(std::span<const RenderDescriptorWrite>{
            &sampled_texture_write,
            1,
        });
    assert(texture_descriptor);
    assert(texture_descriptor.value().backend == RenderBackend::headless);
    assert(texture_descriptor.value().write_count == 1);
    assert(texture_descriptor.value().uniform_write_count == 0);
    assert(texture_descriptor.value().sampled_texture_write_count == 1);
    assert(texture_descriptor.value().material_count == 1);
    assert(!texture_descriptor.value().gpu_backed);
    auto pipeline_shader_module =
        device.value()->create_shader_module(compute_shader_desc, minimal_compute_spirv);
    assert(pipeline_shader_module);
    const RenderComputePipelineDesc compute_pipeline_desc{
        pipeline_shader_module.value().handle,
        material.id,
        "main",
        "headless_compute_pipeline",
    };
    auto headless_compute_pipeline = device.value()->create_compute_pipeline(compute_pipeline_desc);
    assert(headless_compute_pipeline);
    assert(headless_compute_pipeline.value().backend == RenderBackend::headless);
    assert(headless_compute_pipeline.value().handle.is_valid());
    assert(headless_compute_pipeline.value().compute_shader.value ==
           pipeline_shader_module.value().handle.value);
    assert(headless_compute_pipeline.value().material_id == material.id);
    assert(headless_compute_pipeline.value().live_compute_pipeline_count == 1);
    assert(!headless_compute_pipeline.value().gpu_backed);
    auto vertex_shader_module = device.value()->create_shader_module(
        RenderShaderModuleDesc{RenderShaderStage::vertex, "minimal_vertex_shader"},
        minimal_vertex_spirv);
    assert(vertex_shader_module);
    auto fragment_shader_module = device.value()->create_shader_module(
        RenderShaderModuleDesc{RenderShaderStage::fragment, "minimal_fragment_shader"},
        minimal_fragment_spirv);
    assert(fragment_shader_module);
    const RenderGraphicsPipelineDesc graphics_pipeline_desc{
        vertex_shader_module.value().handle,
        fragment_shader_module.value().handle,
        material.id,
        "main",
        "main",
        "headless_graphics_pipeline",
    };
    auto headless_graphics_pipeline =
        device.value()->create_graphics_pipeline(graphics_pipeline_desc);
    assert(headless_graphics_pipeline);
    assert(headless_graphics_pipeline.value().backend == RenderBackend::headless);
    assert(headless_graphics_pipeline.value().handle.is_valid());
    assert(headless_graphics_pipeline.value().vertex_shader.value ==
           vertex_shader_module.value().handle.value);
    assert(headless_graphics_pipeline.value().fragment_shader.value ==
           fragment_shader_module.value().handle.value);
    assert(headless_graphics_pipeline.value().material_id == material.id);
    assert(headless_graphics_pipeline.value().live_graphics_pipeline_count == 1);
    assert(!headless_graphics_pipeline.value().gpu_backed);
    assert(device.value()->live_resource_count() == 9);
    assert(device.value()->release_resource(headless_compute_pipeline.value().handle));
    assert(device.value()->live_resource_count() == 8);
    assert(device.value()->release_resource(pipeline_shader_module.value().handle));
    assert(device.value()->live_resource_count() == 7);
    auto draw_stats =
        device.value()->bind_mesh_draws(std::span<const RenderMeshBinding>{&chunk_draw, 1});
    assert(draw_stats);
    assert(draw_stats.value().backend == RenderBackend::headless);
    assert(draw_stats.value().draw_count == 1);
    assert(draw_stats.value().indexed_draw_count == 1);
    assert(draw_stats.value().material_count == 1);
    assert(draw_stats.value().total_vertices == render_upload_mesh.value().vertices.size());
    assert(draw_stats.value().total_indices == render_upload_mesh.value().indices.size());
    assert(!draw_stats.value().gpu_backed);
    assert(!draw_stats.value().draw_commands_submitted);
    assert(device.value()->release_resource(uniform_upload.value().handle));
    assert(device.value()->live_resource_count() == 6);
    assert(device.value()->release_resource(headless_graphics_pipeline.value().handle));
    assert(device.value()->live_resource_count() == 5);
    assert(device.value()->release_resource(vertex_shader_module.value().handle));
    assert(device.value()->live_resource_count() == 4);
    assert(device.value()->release_resource(fragment_shader_module.value().handle));
    assert(device.value()->live_resource_count() == 3);
    assert(device.value()->release_resource(vertex_upload.value().handle));
    assert(device.value()->live_resource_count() == 2);
    assert(device.value()->release_resource(index_upload.value().handle));
    assert(device.value()->live_resource_count() == 1);
    assert(device.value()->release_resource(texture_upload.value().handle));
    assert(device.value()->live_resource_count() == 0);
    assert(!device.value()->release_resource(vertex_upload.value().handle));
    assert(render_buffer_usage_name(RenderBufferUsage::vertex) == "vertex");
    assert(render_image_format_name(RenderImageFormat::rgba8_unorm) == "rgba8_unorm");
    assert(render_image_format_bytes_per_pixel(RenderImageFormat::rgba8_unorm) == 4);

    RenderDeviceDesc vulkan_desc;
    vulkan_desc.backend = RenderBackend::vulkan;
    vulkan_desc.application_name = "Heartstead Vulkan Renderer Test";
    vulkan_desc.initial_extent = RenderExtent{320, 180};
    auto vulkan_device = create_render_device(vulkan_desc);
    if (vulkan_backend_info.available) {
        assert(vulkan_device);
        assert(vulkan_device.value()->backend() == RenderBackend::vulkan);
        assert(vulkan_device.value()->backend_name() == "vulkan");
        const auto vulkan_device_caps = vulkan_device.value()->capabilities();
        assert(vulkan_device_caps.backend == RenderBackend::vulkan);
        assert(vulkan_device_caps.max_extent.width >= vulkan_desc.initial_extent.width);
        assert(vulkan_device_caps.max_extent.height >= vulkan_desc.initial_extent.height);
        assert(!vulkan_device_caps.headless);
        assert(!vulkan_device_caps.supports_present);
        assert(vulkan_device_caps.supports_shader_modules);
        assert(vulkan_device_caps.supports_pipeline_layout);
        assert(vulkan_device_caps.supports_compute_pipelines);
        assert(vulkan_device_caps.supports_graphics_pipelines);
        assert(vulkan_device_caps.supports_descriptor_writes);
        assert(vulkan_device_caps.supports_buffer_upload);
        assert(vulkan_device_caps.supports_image_upload);
        assert(vulkan_device_caps.supports_draw_binding);
        assert(vulkan_device.value()->live_resource_count() == 0);
        auto vulkan_shader_module =
            vulkan_device.value()->create_shader_module(compute_shader_desc, minimal_compute_spirv);
        assert(vulkan_shader_module);
        assert(vulkan_shader_module.value().backend == RenderBackend::vulkan);
        assert(vulkan_shader_module.value().handle.is_valid());
        assert(vulkan_shader_module.value().stage == RenderShaderStage::compute);
        assert(vulkan_shader_module.value().word_count == minimal_compute_spirv.size());
        assert(vulkan_shader_module.value().live_shader_module_count == 1);
        assert(vulkan_shader_module.value().gpu_backed);
        assert(vulkan_device.value()->live_resource_count() == 1);
        assert(vulkan_device.value()->release_resource(vulkan_shader_module.value().handle));
        assert(vulkan_device.value()->live_resource_count() == 0);
        auto vulkan_upload = vulkan_device.value()->upload_buffer(
            RenderBufferDesc{RenderBufferUsage::vertex, vertex_upload_bytes.size(),
                             "vulkan_chunk_vertices"},
            vertex_upload_bytes);
        assert(vulkan_upload);
        assert(vulkan_upload.value().backend == RenderBackend::vulkan);
        assert(vulkan_upload.value().handle.is_valid());
        assert(vulkan_upload.value().usage == RenderBufferUsage::vertex);
        assert(vulkan_upload.value().byte_size == vertex_upload_bytes.size());
        assert(vulkan_upload.value().live_resource_count == 1);
        assert(vulkan_upload.value().gpu_backed);
        assert(vulkan_device.value()->live_resource_count() == 1);
        auto vulkan_index_upload = vulkan_device.value()->upload_buffer(
            RenderBufferDesc{RenderBufferUsage::index, index_upload_bytes.size(),
                             "vulkan_chunk_indices"},
            index_upload_bytes);
        assert(vulkan_index_upload);
        const RenderMeshBinding vulkan_chunk_draw{
            vulkan_upload.value().handle,
            vulkan_index_upload.value().handle,
            material.id,
            static_cast<std::uint32_t>(render_upload_mesh.value().vertices.size()),
            static_cast<std::uint32_t>(render_upload_mesh.value().indices.size()),
            1,
            "vulkan_chunk_draw",
        };
        auto vulkan_pipeline_stats =
            vulkan_device.value()->bind_pipeline_layout(material_pipeline_layout.value());
        assert(vulkan_pipeline_stats);
        assert(vulkan_pipeline_stats.value().backend == RenderBackend::vulkan);
        assert(vulkan_pipeline_stats.value().material_id == material.id);
        assert(vulkan_pipeline_stats.value().descriptor_count == 3);
        assert(vulkan_pipeline_stats.value().sampled_texture_count == 1);
        assert(vulkan_pipeline_stats.value().uniform_count == 2);
        assert(vulkan_pipeline_stats.value().bound_pipeline_count == 1);
        assert(vulkan_pipeline_stats.value().gpu_backed);
        auto vulkan_uniform_upload = vulkan_device.value()->upload_buffer(
            RenderBufferDesc{RenderBufferUsage::uniform, material_uniform_bytes.size(),
                             "vulkan_material_uniforms"},
            material_uniform_bytes);
        assert(vulkan_uniform_upload);
        assert(vulkan_uniform_upload.value().usage == RenderBufferUsage::uniform);
        assert(vulkan_device.value()->live_resource_count() == 3);
        auto vulkan_texture_upload = vulkan_device.value()->upload_image(
            RenderImageDesc{RenderImageFormat::rgba8_unorm, 2, 2, "vulkan_material_albedo"},
            material_texture_bytes);
        assert(vulkan_texture_upload);
        assert(vulkan_texture_upload.value().backend == RenderBackend::vulkan);
        assert(vulkan_texture_upload.value().handle.is_valid());
        assert(vulkan_texture_upload.value().format == RenderImageFormat::rgba8_unorm);
        assert(vulkan_texture_upload.value().width == 2);
        assert(vulkan_texture_upload.value().height == 2);
        assert(vulkan_texture_upload.value().byte_size == material_texture_bytes.size());
        assert(vulkan_texture_upload.value().gpu_backed);
        assert(vulkan_device.value()->live_resource_count() == 4);
        const RenderDescriptorWrite vulkan_roughness_write{
            material.id,
            "roughness",
            vulkan_uniform_upload.value().handle,
            0,
            material_uniform_bytes.size(),
        };
        auto vulkan_descriptor_write = vulkan_device.value()->write_descriptors(
            std::span<const RenderDescriptorWrite>{&vulkan_roughness_write, 1});
        assert(vulkan_descriptor_write);
        assert(vulkan_descriptor_write.value().backend == RenderBackend::vulkan);
        assert(vulkan_descriptor_write.value().write_count == 1);
        assert(vulkan_descriptor_write.value().uniform_write_count == 1);
        assert(vulkan_descriptor_write.value().sampled_texture_write_count == 0);
        assert(vulkan_descriptor_write.value().material_count == 1);
        assert(vulkan_descriptor_write.value().gpu_backed);
        const RenderDescriptorWrite vulkan_tint_write{
            material.id,
            "tint",
            vulkan_uniform_upload.value().handle,
            0,
            material_uniform_bytes.size(),
        };
        assert(vulkan_device.value()->write_descriptors(
            std::span<const RenderDescriptorWrite>{&vulkan_tint_write, 1}));
        const RenderDescriptorWrite vulkan_texture_write{
            material.id,
            "albedo",
            vulkan_texture_upload.value().handle,
        };
        auto vulkan_texture_descriptor = vulkan_device.value()->write_descriptors(
            std::span<const RenderDescriptorWrite>{&vulkan_texture_write, 1});
        assert(vulkan_texture_descriptor);
        assert(vulkan_texture_descriptor.value().backend == RenderBackend::vulkan);
        assert(vulkan_texture_descriptor.value().write_count == 1);
        assert(vulkan_texture_descriptor.value().uniform_write_count == 0);
        assert(vulkan_texture_descriptor.value().sampled_texture_write_count == 1);
        assert(vulkan_texture_descriptor.value().material_count == 1);
        assert(vulkan_texture_descriptor.value().gpu_backed);
        auto vulkan_pipeline_shader =
            vulkan_device.value()->create_shader_module(compute_shader_desc, minimal_compute_spirv);
        assert(vulkan_pipeline_shader);
        const RenderComputePipelineDesc vulkan_compute_pipeline_desc{
            vulkan_pipeline_shader.value().handle,
            material.id,
            "main",
            "vulkan_compute_pipeline",
        };
        auto vulkan_compute_pipeline =
            vulkan_device.value()->create_compute_pipeline(vulkan_compute_pipeline_desc);
        assert(vulkan_compute_pipeline);
        assert(vulkan_compute_pipeline.value().backend == RenderBackend::vulkan);
        assert(vulkan_compute_pipeline.value().handle.is_valid());
        assert(vulkan_compute_pipeline.value().compute_shader.value ==
               vulkan_pipeline_shader.value().handle.value);
        assert(vulkan_compute_pipeline.value().material_id == material.id);
        assert(vulkan_compute_pipeline.value().live_compute_pipeline_count == 1);
        assert(vulkan_compute_pipeline.value().gpu_backed);
        auto vulkan_vertex_shader = vulkan_device.value()->create_shader_module(
            RenderShaderModuleDesc{RenderShaderStage::vertex, "vulkan_vertex_shader"},
            minimal_vertex_spirv);
        assert(vulkan_vertex_shader);
        auto vulkan_fragment_shader = vulkan_device.value()->create_shader_module(
            RenderShaderModuleDesc{RenderShaderStage::fragment, "vulkan_fragment_shader"},
            minimal_fragment_spirv);
        assert(vulkan_fragment_shader);
        const RenderGraphicsPipelineDesc vulkan_graphics_pipeline_desc{
            vulkan_vertex_shader.value().handle,
            vulkan_fragment_shader.value().handle,
            material.id,
            "main",
            "main",
            "vulkan_graphics_pipeline",
        };
        auto vulkan_graphics_pipeline =
            vulkan_device.value()->create_graphics_pipeline(vulkan_graphics_pipeline_desc);
        assert(vulkan_graphics_pipeline);
        assert(vulkan_graphics_pipeline.value().backend == RenderBackend::vulkan);
        assert(vulkan_graphics_pipeline.value().handle.is_valid());
        assert(vulkan_graphics_pipeline.value().vertex_shader.value ==
               vulkan_vertex_shader.value().handle.value);
        assert(vulkan_graphics_pipeline.value().fragment_shader.value ==
               vulkan_fragment_shader.value().handle.value);
        assert(vulkan_graphics_pipeline.value().material_id == material.id);
        assert(vulkan_graphics_pipeline.value().live_graphics_pipeline_count == 1);
        assert(vulkan_graphics_pipeline.value().gpu_backed);
        assert(vulkan_device.value()->live_resource_count() == 9);
        auto previous_vulkan_resource_count = vulkan_device.value()->live_resource_count();
        assert(vulkan_device.value()->release_resource(vulkan_compute_pipeline.value().handle));
        assert(vulkan_device.value()->live_resource_count() <= previous_vulkan_resource_count);
        previous_vulkan_resource_count = vulkan_device.value()->live_resource_count();
        assert(vulkan_device.value()->release_resource(vulkan_pipeline_shader.value().handle));
        assert(vulkan_device.value()->live_resource_count() <= previous_vulkan_resource_count);
        auto vulkan_draw_stats = vulkan_device.value()->bind_mesh_draws(
            std::span<const RenderMeshBinding>{&vulkan_chunk_draw, 1});
        assert(vulkan_draw_stats);
        assert(vulkan_draw_stats.value().backend == RenderBackend::vulkan);
        assert(vulkan_draw_stats.value().draw_count == 1);
        assert(vulkan_draw_stats.value().indexed_draw_count == 1);
        assert(vulkan_draw_stats.value().material_count == 1);
        assert(vulkan_draw_stats.value().total_vertices ==
               render_upload_mesh.value().vertices.size());
        assert(vulkan_draw_stats.value().total_indices ==
               render_upload_mesh.value().indices.size());
        assert(vulkan_draw_stats.value().gpu_backed);
        assert(vulkan_draw_stats.value().draw_commands_submitted);
        previous_vulkan_resource_count = vulkan_device.value()->live_resource_count();
        assert(vulkan_device.value()->release_resource(vulkan_uniform_upload.value().handle));
        assert(vulkan_device.value()->live_resource_count() <= previous_vulkan_resource_count);
        previous_vulkan_resource_count = vulkan_device.value()->live_resource_count();
        assert(vulkan_device.value()->release_resource(vulkan_graphics_pipeline.value().handle));
        assert(vulkan_device.value()->live_resource_count() <= previous_vulkan_resource_count);
        previous_vulkan_resource_count = vulkan_device.value()->live_resource_count();
        assert(vulkan_device.value()->release_resource(vulkan_vertex_shader.value().handle));
        assert(vulkan_device.value()->live_resource_count() <= previous_vulkan_resource_count);
        previous_vulkan_resource_count = vulkan_device.value()->live_resource_count();
        assert(vulkan_device.value()->release_resource(vulkan_fragment_shader.value().handle));
        assert(vulkan_device.value()->live_resource_count() <= previous_vulkan_resource_count);
        assert(vulkan_device.value()->release_resource(vulkan_upload.value().handle));
        assert(vulkan_device.value()->release_resource(vulkan_index_upload.value().handle));
        assert(vulkan_device.value()->release_resource(vulkan_texture_upload.value().handle));
        assert(vulkan_device.value()->live_resource_count() <= previous_vulkan_resource_count);
        auto invalid_vulkan_present = vulkan_device.value()->render_frame(
            RenderFrameDesc{ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, {}, true});
        assert(!invalid_vulkan_present);
        assert(invalid_vulkan_present.error().code == "renderer.vulkan_present_unavailable");
        auto vulkan_frame = vulkan_device.value()->render_frame(
            RenderFrameDesc{ClearColor{0.05F, 0.1F, 0.2F, 1.0F}, {}, false});
        assert(vulkan_frame);
        assert(vulkan_frame.value().backend == RenderBackend::vulkan);
        assert(vulkan_frame.value().frame_index == 0);
        assert(vulkan_device.value()->live_resource_count() == 0);
        assert(vulkan_frame.value().extent.width == 320);
        assert(vulkan_frame.value().extent.height == 180);
        assert(!vulkan_frame.value().presented);
        assert(vulkan_frame.value().render_pass_count == 1);
        assert(vulkan_frame.value().present_pass_count == 0);
        assert(vulkan_frame.value().resource_use_count == 1);
        assert(vulkan_frame.value().dependency_count == 0);
        assert(vulkan_frame.value().transition_count == 1);
        assert(vulkan_frame.value().synchronization_barrier_count == 1);
        assert(vulkan_frame.value().submitted_synchronization_barrier_count == 1);
        assert(vulkan_device.value()->completed_frame_count() == 1);
        auto vulkan_resize = vulkan_device.value()->resize(RenderExtent{640, 360});
        assert(vulkan_resize);
        auto resized_vulkan_frame = vulkan_device.value()->render_frame(
            RenderFrameDesc{ClearColor{0.2F, 0.1F, 0.05F, 1.0F}, {}, false});
        assert(resized_vulkan_frame);
        assert(resized_vulkan_frame.value().frame_index == 1);
        assert(resized_vulkan_frame.value().extent.width == 640);
        assert(resized_vulkan_frame.value().extent.height == 360);
        assert(vulkan_device.value()->completed_frame_count() == 2);

        RenderFramePlanBuilder vulkan_execution_plan(RenderExtent{256, 128});
        assert(vulkan_execution_plan.add_resource(
            {"scene_color", RenderExtent{256, 128}, RenderResourceLifetime::transient}));
        assert(vulkan_execution_plan.add_pass({"clear_scene",
                                               RenderPassKind::clear,
                                               {},
                                               {"scene_color"},
                                               ClearColor{0.12F, 0.07F, 0.04F, 1.0F},
                                               false}));
        assert(vulkan_execution_plan.add_pass(
            {"debug", RenderPassKind::debug, {"scene_color"}, {"scene_color"}, {}, false}));
        auto vulkan_plan = vulkan_execution_plan.build();
        assert(vulkan_plan);
        auto vulkan_planned_frame = vulkan_device.value()->execute_frame_plan(vulkan_plan.value());
        assert(!vulkan_planned_frame);
        assert(vulkan_planned_frame.error().code == "renderer.vulkan_unsupported_frame_plan");
        assert(vulkan_device.value()->completed_frame_count() == 2);

        RenderFramePlanBuilder vulkan_multi_resource_plan(RenderExtent{256, 128});
        assert(vulkan_multi_resource_plan.add_resource(
            {"scene_color", RenderExtent{256, 128}, RenderResourceLifetime::transient}));
        assert(vulkan_multi_resource_plan.add_resource(
            {"scratch", RenderExtent{256, 128}, RenderResourceLifetime::transient}));
        assert(vulkan_multi_resource_plan.add_pass({"clear_scene",
                                                    RenderPassKind::clear,
                                                    {},
                                                    {"scene_color"},
                                                    ClearColor{0.05F, 0.06F, 0.07F, 1.0F},
                                                    false}));
        assert(vulkan_multi_resource_plan.add_pass(
            {"shade_scene", RenderPassKind::debug, {"scene_color"}, {"scene_color"}, {}, false}));
        assert(vulkan_multi_resource_plan.add_pass(
            {"clear_scratch", RenderPassKind::clear, {}, {"scratch"}, {}, false}));
        assert(vulkan_multi_resource_plan.add_pass(
            {"sample_scratch", RenderPassKind::debug, {"scratch"}, {}, {}, false}));
        auto vulkan_multi_plan = vulkan_multi_resource_plan.build();
        assert(vulkan_multi_plan);
        auto vulkan_multi_frame =
            vulkan_device.value()->execute_frame_plan(vulkan_multi_plan.value());
        assert(!vulkan_multi_frame);
        assert(vulkan_multi_frame.error().code == "renderer.vulkan_unsupported_frame_plan");
        assert(vulkan_device.value()->completed_frame_count() == 2);

        RenderDeviceDesc invalid_native_window_desc;
        invalid_native_window_desc.backend = RenderBackend::vulkan;
        invalid_native_window_desc.application_name = "Heartstead Invalid Native Window Test";
        invalid_native_window_desc.native_window = heartstead::platform::NativeWindowHandle{
            heartstead::platform::NativeWindowSystem::x11, nullptr, 0};
        auto invalid_native_window_device = create_render_device(invalid_native_window_desc);
        assert(!invalid_native_window_device);

        const auto native_info = heartstead::platform::platform_backend_info(
            heartstead::platform::PlatformBackend::native);
        if (native_info.available) {
            auto native_platform = heartstead::platform::create_platform(
                heartstead::platform::PlatformDesc{heartstead::platform::PlatformBackend::native});
            assert(native_platform);
            auto native_window = native_platform.value()->create_window(
                heartstead::platform::WindowDesc{"Heartstead Vulkan Present Test", 320, 180, true});
            assert(native_window);
            auto native_handle =
                native_platform.value()->native_window_handle(native_window.value());
            assert(native_handle);

            RenderDeviceDesc vulkan_present_desc;
            vulkan_present_desc.backend = RenderBackend::vulkan;
            vulkan_present_desc.application_name = "Heartstead Vulkan Present Renderer Test";
            vulkan_present_desc.initial_extent = RenderExtent{320, 180};
            vulkan_present_desc.native_window = native_handle.value();
            auto vulkan_present_device = create_render_device(vulkan_present_desc);
            if (vulkan_present_device) {
                const auto vulkan_present_caps = vulkan_present_device.value()->capabilities();
                assert(vulkan_present_caps.backend == RenderBackend::vulkan);
                assert(vulkan_present_caps.supports_present);
                auto vulkan_present_frame = vulkan_present_device.value()->render_frame(
                    RenderFrameDesc{ClearColor{0.02F, 0.04F, 0.08F, 1.0F}, {}, true});
                assert(vulkan_present_frame);
                assert(vulkan_present_frame.value().backend == RenderBackend::vulkan);
                assert(vulkan_present_frame.value().presented);
                assert(vulkan_present_frame.value().render_pass_count == 2);
                assert(vulkan_present_frame.value().present_pass_count == 1);
                assert(vulkan_present_frame.value().resource_use_count == 2);
                assert(vulkan_present_frame.value().dependency_count == 1);
                assert(vulkan_present_frame.value().transition_count == 2);
                assert(vulkan_present_frame.value().synchronization_barrier_count == 2);
                assert(vulkan_present_frame.value().submitted_synchronization_barrier_count == 0);
                assert(vulkan_present_device.value()->completed_frame_count() == 1);
            } else {
                assert(!vulkan_present_device.error().code.empty());
            }
        }
    } else {
        assert(!vulkan_device);
        assert(!vulkan_device.error().code.empty());
    }

    assert(present_mode_name(PresentMode::mailbox) == "mailbox");
    assert(render_pass_kind_name(RenderPassKind::post_process) == "post_process");
    assert(render_resource_lifetime_name(RenderResourceLifetime::external) == "external");
}

void test_physics_world() {
    using namespace heartstead::physics;

    const auto headless_info = physics_backend_info(PhysicsBackend::headless);
    assert(headless_info.available);
    assert(headless_info.name == "headless");

    const auto jolt_info = physics_backend_info(PhysicsBackend::jolt);
    assert(jolt_info.name == "jolt");
    const auto headless_capabilities = physics_backend_capabilities(PhysicsBackend::headless);
    assert(headless_capabilities.available);
    assert(headless_capabilities.deterministic);
    assert(headless_capabilities.supports_dynamic_bodies);
    assert(headless_capabilities.supports_kinematic_bodies);
    assert(headless_capabilities.supports_static_bodies);
    assert(headless_capabilities.supports_compound_shapes);
    assert(headless_capabilities.supports_aabb_queries);
    assert(headless_capabilities.supports_contacts);
    assert(headless_capabilities.supports_sleeping);
    assert(!headless_capabilities.supports_constraints);
    assert(headless_capabilities.supports_collision_response);
    assert(headless_capabilities.library == "headless");
    const auto jolt_capabilities = physics_backend_capabilities(PhysicsBackend::jolt);
    assert(jolt_capabilities.available == jolt_info.available);
    assert(jolt_capabilities.deterministic);
    assert(jolt_capabilities.supports_dynamic_bodies);
    assert(jolt_capabilities.supports_kinematic_bodies);
    assert(jolt_capabilities.supports_static_bodies);
    assert(jolt_capabilities.supports_compound_shapes);
    assert(jolt_capabilities.supports_aabb_queries);
    assert(jolt_capabilities.supports_contacts);
    assert(jolt_capabilities.supports_sleeping);
    assert(jolt_capabilities.supports_character_controllers);
    assert(jolt_capabilities.supports_constraints);
    assert(jolt_capabilities.supports_collision_response);
    assert(jolt_capabilities.library == "jolt");

    PhysicsWorldDesc invalid_world;
    invalid_world.gravity = Vec3{0.0F, std::numeric_limits<float>::infinity(), 0.0F};
    assert(!create_physics_world(invalid_world));

    PhysicsWorldDesc world_desc;
    world_desc.gravity = Vec3{0.0F, -10.0F, 0.0F};
    auto world = create_physics_world(world_desc);
    assert(world);
    assert(world.value()->backend() == PhysicsBackend::headless);
    assert(world.value()->backend_name() == "headless");
    assert(world.value()->capabilities().supports_aabb_queries);

    PhysicsBodyDesc invalid_dynamic;
    invalid_dynamic.motion_type = BodyMotionType::dynamic;
    invalid_dynamic.mass = 0.0F;
    assert(!world.value()->create_body(invalid_dynamic));

    PhysicsBodyDesc static_body;
    static_body.motion_type = BodyMotionType::static_body;
    static_body.shape.kind = ShapeKind::box;
    static_body.shape.half_extents = Vec3{10.0F, 0.5F, 10.0F};
    auto ground = world.value()->create_body(static_body);
    assert(ground);

    PhysicsBodyDesc dynamic_body;
    dynamic_body.motion_type = BodyMotionType::dynamic;
    dynamic_body.mass = 2.0F;
    dynamic_body.position = Vec3{0.0F, 10.0F, 0.0F};
    dynamic_body.user_data = 42;
    auto body = world.value()->create_body(dynamic_body);
    assert(body);
    assert(world.value()->body_count() == 2);

    auto impulse = world.value()->apply_impulse(body.value(), Vec3{4.0F, 0.0F, 0.0F});
    assert(impulse);

    auto stats = world.value()->step(PhysicsStepDesc{0.5F});
    assert(stats);
    assert(stats.value().body_count == 2);
    assert(stats.value().dynamic_body_count == 1);
    assert(stats.value().integrated_body_count == 1);

    auto state = world.value()->body_state(body.value());
    assert(state);
    assert(state->user_data == 42);
    assert(nearly_equal(state->linear_velocity.x, 2.0F));
    assert(nearly_equal(state->linear_velocity.y, -5.0F));
    assert(nearly_equal(state->position.x, 1.0F));
    assert(nearly_equal(state->position.y, 7.5F));
    assert(world.value()->set_angular_velocity(body.value(), {0.0F, 0.25F, 0.0F}));
    state = world.value()->body_state(body.value());
    assert(state);
    assert(state->angular_velocity == (Vec3{0.0F, 0.25F, 0.0F}));

    auto static_velocity =
        world.value()->set_linear_velocity(ground.value(), Vec3{1.0F, 0.0F, 0.0F});
    assert(!static_velocity);
    assert(!world.value()->set_angular_velocity(ground.value(), Vec3{0.0F, 1.0F, 0.0F}));

    PhysicsBodyDesc kinematic_body;
    kinematic_body.motion_type = BodyMotionType::kinematic;
    kinematic_body.linear_velocity = Vec3{0.0F, 0.0F, 3.0F};
    kinematic_body.user_data = 77;
    auto mover = world.value()->create_body(kinematic_body);
    assert(mover);
    const auto ground_id = ground.value();
    const auto mover_id = mover.value();
    auto second_step = world.value()->step(PhysicsStepDesc{2.0F});
    assert(second_step);
    assert(second_step.value().integrated_body_count == 2);
    assert(second_step.value().contact_count >= 1);
    auto mover_state = world.value()->body_state(mover.value());
    assert(mover_state);
    assert(nearly_equal(mover_state->position.z, 6.0F));
    assert(world.value()->set_body_rotation(mover.value(), {0.0F, 30.0F, 0.0F}));
    mover_state = world.value()->body_state(mover.value());
    assert(mover_state);
    assert(mover_state->rotation_degrees == (Vec3{0.0F, 30.0F, 0.0F}));

    auto invalid_query =
        world.value()->query_aabb(PhysicsAabb{Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 1.0F}});
    assert(!invalid_query);
    assert(invalid_query.error().code == "physics.invalid_aabb");

    auto query = world.value()->query_aabb(
        PhysicsAabb{Vec3{-0.25F, -0.25F, 5.75F}, Vec3{0.25F, 0.25F, 6.25F}});
    assert(query);
    assert(std::any_of(query.value().begin(), query.value().end(),
                       [mover_id](const PhysicsOverlap& overlap) {
                           return overlap.body == mover_id && overlap.user_data == 77 &&
                                  overlap.bounds.contains(Vec3{0.0F, 0.0F, 6.0F});
                       }));

    auto contacts = world.value()->drain_contacts();
    assert(!contacts.empty());
    assert(std::any_of(contacts.begin(), contacts.end(),
                       [ground_id, mover_id](const PhysicsContact& contact) {
                           return contact.first == ground_id && contact.second == mover_id &&
                                  contact.second_user_data == 77 &&
                                  contact.penetration_depth >= 0.0F && contact.normal.is_finite();
                       }));
    assert(world.value()->drain_contacts().empty());

    PhysicsBodyDesc compound_body;
    compound_body.motion_type = BodyMotionType::dynamic;
    compound_body.mass = 4.0F;
    compound_body.shape.kind = ShapeKind::compound;
    compound_body.shape.children.push_back(
        CompoundShapeChild{ShapeKind::box, Vec3{}, Vec3{1.0F, 0.5F, 0.25F}, 0.5F, 0.5F});
    compound_body.shape.children.push_back(
        CompoundShapeChild{ShapeKind::sphere, Vec3{0.0F, 1.0F, 0.0F}, Vec3{}, 0.25F, 0.5F});
    auto compound = world.value()->create_body(compound_body);
    assert(compound);

    auto destroyed = world.value()->destroy_body(compound.value());
    assert(destroyed);
    assert(!world.value()->body_state(compound.value()));

    auto settling_world = create_physics_world(world_desc);
    assert(settling_world);
    auto settling_ground = settling_world.value()->create_body(static_body);
    assert(settling_ground);

    PhysicsBodyDesc falling_body;
    falling_body.motion_type = BodyMotionType::dynamic;
    falling_body.mass = 1.0F;
    falling_body.position = Vec3{0.0F, 2.0F, 0.0F};
    auto falling = settling_world.value()->create_body(falling_body);
    assert(falling);

    for (std::uint32_t step_index = 0; step_index < 8; ++step_index) {
        auto settle_step = settling_world.value()->step(PhysicsStepDesc{0.1F});
        assert(settle_step);
        assert(settle_step.value().dynamic_body_count == 1);
    }

    auto settled_state = settling_world.value()->body_state(falling.value());
    assert(settled_state);
    assert(settled_state->sleeping);
    assert(nearly_equal(settled_state->position.y, 1.0F));
    assert(nearly_equal(settled_state->linear_velocity.y, 0.0F));

    auto sleeping_step = settling_world.value()->step(PhysicsStepDesc{0.1F});
    assert(sleeping_step);
    assert(sleeping_step.value().dynamic_body_count == 1);
    assert(sleeping_step.value().integrated_body_count == 0);
    auto sleeping_state = settling_world.value()->body_state(falling.value());
    assert(sleeping_state);
    assert(sleeping_state->sleeping);
    assert(nearly_equal(sleeping_state->position.y, 1.0F));

    PhysicsWorldDesc jolt_desc;
    jolt_desc.backend = PhysicsBackend::jolt;
    auto jolt_world = create_physics_world(jolt_desc);
    if (jolt_info.available) {
        assert(jolt_world);
        assert(jolt_world.value()->backend() == PhysicsBackend::jolt);
        assert(jolt_world.value()->backend_name() == "jolt");

        auto jolt_ground = jolt_world.value()->create_body(static_body);
        assert(jolt_ground);
        PhysicsBodyDesc jolt_falling_body = falling_body;
        jolt_falling_body.position = Vec3{0.0F, 2.0F, 0.0F};
        auto jolt_falling = jolt_world.value()->create_body(jolt_falling_body);
        assert(jolt_falling);
        bool observed_contact = false;
        for (std::uint32_t step_index = 0; step_index < 180; ++step_index) {
            auto jolt_step = jolt_world.value()->step(PhysicsStepDesc{1.0F / 60.0F});
            assert(jolt_step);
            observed_contact = observed_contact || jolt_step.value().contact_count > 0;
            static_cast<void>(jolt_world.value()->drain_contacts());
        }
        const auto jolt_settled = jolt_world.value()->body_state(jolt_falling.value());
        assert(jolt_settled);
        assert(observed_contact);
        assert(jolt_settled->sleeping);
        assert(std::abs(jolt_settled->position.y - 1.0F) <= 0.02F);
        assert(std::abs(jolt_settled->linear_velocity.y) <= 0.05F);
    } else {
        assert(!jolt_world);
        assert(jolt_world.error().code == "physics.jolt_unavailable");
    }

    assert(body_motion_type_name(BodyMotionType::dynamic) == "dynamic");
    assert(shape_kind_name(ShapeKind::compound) == "compound");
}

void test_physical_resource_lifecycle() {
    using namespace heartstead;

    const auto tree_id = core::PrototypeId::parse("base:entities/felled_oak");
    const auto cargo_id = core::PrototypeId::parse("base:cargo/heavy_log");
    assert(tree_id);
    assert(cargo_id);

    entities::PhysicalResourceRecord resource;
    resource.resource_id = core::SaveId::from_value(1200);
    resource.prototype_id = tree_id.value();
    resource.cargo_prototype_id = cargo_id.value();
    resource.kind = entities::PhysicalResourceKind::felled_tree;
    resource.mass_grams = 90000;
    resource.volume_milliliters = 180000;
    resource.allowed_transport_modes = cargo::CargoTransportModes::of(
        {cargo::CargoTransportMode::cart, cargo::CargoTransportMode::wagon,
         cargo::CargoTransportMode::animal});
    resource.hazard_tags.push_back("crush");
    resource.segments.push_back(entities::PhysicalResourceSegment{
        physics::ShapeKind::box, physics::Vec3{}, physics::Vec3{3.0F, 0.35F, 0.35F}, 0.5F, 0.5F});
    resource.segments.push_back(
        entities::PhysicalResourceSegment{physics::ShapeKind::box, physics::Vec3{2.0F, 0.4F, 0.0F},
                                          physics::Vec3{1.0F, 0.2F, 0.2F}, 0.5F, 0.5F});
    assert(resource.validate());

    auto body_desc = entities::make_physical_resource_body_desc(
        resource, physics::Vec3{0.0F, 5.0F, 0.0F}, physics::Vec3{0.0F, -1.0F, 0.0F});
    assert(body_desc);
    assert(body_desc.value().motion_type == physics::BodyMotionType::dynamic);
    assert(body_desc.value().shape.kind == physics::ShapeKind::compound);
    assert(body_desc.value().shape.children.size() == 2);
    assert(body_desc.value().user_data == resource.resource_id.value());

    auto world = physics::create_physics_world(physics::PhysicsWorldDesc{});
    assert(world);
    auto body = world.value()->create_body(body_desc.value());
    assert(body);
    assert(entities::attach_physical_resource_body(resource, body.value()));
    assert(resource.state == entities::PhysicalResourceState::dynamic);
    assert(resource.physics_body_id == body.value());

    auto inspection = debug::Inspector::inspect(resource);
    assert(inspection.object_type == "physical_resource");
    assert(inspection.state == "dynamic");
    assert(inspection.find_field("segment_count") != nullptr);

    assert(entities::mark_physical_resource_settled(resource));
    assert(resource.state == entities::PhysicalResourceState::settled_sleeping);
    assert(entities::freeze_physical_resource(resource));
    assert(resource.state == entities::PhysicalResourceState::frozen_static);

    auto cargo_record = entities::convert_physical_resource_to_cargo(
        resource, core::SaveId::from_value(1201), *world.value());
    assert(cargo_record);
    assert(cargo_record.value().prototype_id == cargo_id.value());
    assert(cargo_record.value().mass_grams == 90000);
    assert(cargo_record.value().allowed_transport_modes.allows(cargo::CargoTransportMode::wagon));
    assert(resource.state == entities::PhysicalResourceState::converted_to_cargo);
    assert(!resource.physics_body_id.is_valid());
    assert(world.value()->body_count() == 0);
    assert(!world.value()->body_state(body.value()).has_value());
    assert(!entities::make_physical_resource_body_desc(resource));

    auto frozen_resource = resource;
    frozen_resource.state = entities::PhysicalResourceState::frozen_static;
    frozen_resource.physics_body_id = {};
    frozen_resource.needs_physics_rebuild = true;
    auto frozen_desc = entities::make_physical_resource_body_desc(
        frozen_resource, physics::Vec3{1.0F, 2.0F, 3.0F}, physics::Vec3{4.0F, 5.0F, 6.0F});
    assert(frozen_desc);
    assert(frozen_desc.value().motion_type == physics::BodyMotionType::static_body);
    assert(frozen_desc.value().mass == 0.0F);
    assert((frozen_desc.value().linear_velocity == physics::Vec3{}));
    auto frozen_body = world.value()->create_body(frozen_desc.value());
    assert(frozen_body);
    assert(entities::attach_physical_resource_body(frozen_resource, frozen_body.value()));
    assert(frozen_resource.state == entities::PhysicalResourceState::frozen_static);
    assert(!frozen_resource.needs_physics_rebuild);
    auto frozen_body_state = world.value()->body_state(frozen_body.value());
    assert(frozen_body_state);
    assert(frozen_body_state->motion_type == physics::BodyMotionType::static_body);
}

void test_scripting_runtime() {
    using namespace heartstead::scripting;

    const auto disabled_info = script_backend_info(ScriptBackend::disabled);
    assert(disabled_info.available);
    assert(disabled_info.name == "disabled");

    const auto luau_info = script_backend_info(ScriptBackend::luau);
    assert(luau_info.name == "luau");

    assert(!create_script_runtime(ScriptRuntimeDesc{ScriptBackend::disabled, 0}));
    assert(!create_script_runtime(ScriptRuntimeDesc{ScriptBackend::disabled, 128, 0}));
    assert(!create_script_runtime(ScriptRuntimeDesc{ScriptBackend::disabled, 128, 1, 0}));
    assert(!create_script_runtime(ScriptRuntimeDesc{ScriptBackend::disabled, 128, 1, 1, 0}));
    assert(validate_script_value(ScriptValue::string("1234"), 4));
    auto oversized_script_value = validate_script_value(ScriptValue::string("12345"), 4);
    assert(!oversized_script_value);
    assert(oversized_script_value.error().code == "scripting.string_argument_too_large");
    assert(validate_script_return_value(ScriptValue::string("1234"), 4));
    auto oversized_return_value = validate_script_return_value(ScriptValue::string("12345"), 4);
    assert(!oversized_return_value);
    assert(oversized_return_value.error().code == "scripting.string_return_too_large");

    ScriptHostApiDesc set_voxel_api;
    set_voxel_api.api_id = "world.set_voxel";
    set_voxel_api.stage = ScriptStage::runtime_server;
    set_voxel_api.min_module_api_version = 1;
    set_voxel_api.required_permissions = {ScriptPermission::emit_commands};
    set_voxel_api.arguments = {ScriptHostApiArgument{"chunk", ScriptValueKind::string},
                               ScriptHostApiArgument{"voxel", ScriptValueKind::string},
                               ScriptHostApiArgument{"cell", ScriptValueKind::string}};
    assert(validate_script_host_api_desc(set_voxel_api));
    assert(validate_script_host_api_registry({set_voxel_api}));
    assert(is_valid_script_host_api_id(set_voxel_api.api_id));
    assert(find_script_host_api({set_voxel_api}, "world.set_voxel") != nullptr);
    assert(find_script_host_api({set_voxel_api}, "world.missing") == nullptr);

    ScriptHostApiDesc invalid_host_api = set_voxel_api;
    invalid_host_api.api_id = "World.set_voxel";
    auto invalid_host_api_status = validate_script_host_api_desc(invalid_host_api);
    assert(!invalid_host_api_status);
    assert(invalid_host_api_status.error().code == "scripting.invalid_host_api_id");

    ScriptRuntimeDesc duplicate_host_api_desc;
    duplicate_host_api_desc.host_apis = {set_voxel_api, set_voxel_api};
    auto duplicate_host_api_runtime = create_script_runtime(duplicate_host_api_desc);
    assert(!duplicate_host_api_runtime);
    assert(duplicate_host_api_runtime.error().code == "scripting.duplicate_host_api");

    auto runtime = create_script_runtime(ScriptRuntimeDesc{ScriptBackend::disabled, 128});
    assert(runtime);
    assert(runtime.value()->backend() == ScriptBackend::disabled);
    assert(runtime.value()->backend_name() == "disabled");
    assert(runtime.value()->module_count() == 0);

    ScriptModuleDesc module;
    module.module_id = "base:scripts/runtime_server/debug_ping";
    module.source_mod_id = "base";
    module.source_path = "mods/base/scripts/runtime_server/debug_ping.luau";
    module.source = "return { ping = function(value) return value end }";
    module.stage = ScriptStage::runtime_server;
    module.permissions = {ScriptPermission::read_prototypes, ScriptPermission::emit_commands};

    auto loaded = runtime.value()->load_module(module);
    assert(loaded);
    assert(runtime.value()->module_count() == 1);
    const auto* info = runtime.value()->find_module(module.module_id);
    assert(info != nullptr);
    assert(info->module_id == module.module_id);
    assert(info->source_mod_id == "base");
    assert(info->source_bytes == module.source.size());
    assert(info->permissions.size() == 2);
    assert(script_module_has_permission(*info, ScriptPermission::read_prototypes));
    assert(script_module_has_permission(*info, ScriptPermission::emit_commands));
    assert(!script_module_has_permission(*info, ScriptPermission::write_mod_state));
    assert(validate_script_host_api_call(*info, set_voxel_api));
    ScriptCallResult oversized_result;
    oversized_result.return_value = ScriptValue::string("12345");
    auto oversized_call_result = validate_script_call_result(
        *info, oversized_result, ScriptRuntimeDesc{ScriptBackend::disabled, 128, 256, 32, 4});
    assert(!oversized_call_result);
    assert(oversized_call_result.error().code == "scripting.string_return_too_large");

    ScriptHostApiDesc client_only_api = set_voxel_api;
    client_only_api.api_id = "ui.draw";
    client_only_api.stage = ScriptStage::runtime_client;
    client_only_api.required_permissions = {ScriptPermission::client_ui};
    client_only_api.arguments.clear();
    auto client_only_api_call = validate_script_host_api_call(*info, client_only_api);
    assert(!client_only_api_call);
    assert(client_only_api_call.error().code == "scripting.host_api_stage_mismatch");

    assert(!runtime.value()->load_module(module));

    auto limited_runtime =
        create_script_runtime(ScriptRuntimeDesc{ScriptBackend::disabled, 128, 1, 1, 4});
    assert(limited_runtime);
    assert(limited_runtime.value()->load_module(module));
    ScriptModuleDesc second_module = module;
    second_module.module_id = "base:scripts/runtime_server/second";
    auto limited_second_load = limited_runtime.value()->load_module(second_module);
    assert(!limited_second_load);
    assert(limited_second_load.error().code == "scripting.module_limit_reached");

    auto too_many_arguments = limited_runtime.value()->call(
        ScriptCallDesc{module.module_id,
                       "ping",
                       ScriptStage::runtime_server,
                       {ScriptValue::string("a"), ScriptValue::string("b")},
                       100,
                       {ScriptPermission::read_prototypes}});
    assert(!too_many_arguments);
    assert(too_many_arguments.error().code == "scripting.too_many_arguments");

    auto too_large_string =
        limited_runtime.value()->call(ScriptCallDesc{module.module_id,
                                                     "ping",
                                                     ScriptStage::runtime_server,
                                                     {ScriptValue::string("hello")},
                                                     100,
                                                     {ScriptPermission::read_prototypes}});
    assert(!too_large_string);
    assert(too_large_string.error().code == "scripting.string_argument_too_large");

    auto disabled_call = runtime.value()->call(ScriptCallDesc{module.module_id,
                                                              "ping",
                                                              ScriptStage::runtime_server,
                                                              {ScriptValue::string("hello")},
                                                              100,
                                                              {ScriptPermission::read_prototypes}});
    assert(!disabled_call);
    assert(disabled_call.error().code == "scripting.runtime_disabled");

    auto permission_denied_call =
        runtime.value()->call(ScriptCallDesc{module.module_id,
                                             "ping",
                                             ScriptStage::runtime_server,
                                             {ScriptValue::string("hello")},
                                             100,
                                             {ScriptPermission::write_mod_state}});
    assert(!permission_denied_call);
    assert(permission_denied_call.error().code == "scripting.permission_denied");

    auto invalid_number_call = runtime.value()->call(
        ScriptCallDesc{module.module_id,
                       "ping",
                       ScriptStage::runtime_server,
                       {ScriptValue::number(std::numeric_limits<double>::infinity())},
                       100,
                       {}});
    assert(!invalid_number_call);
    assert(invalid_number_call.error().code == "scripting.invalid_number_argument");

    assert(!runtime.value()->call(ScriptCallDesc{
        module.module_id, "bad function", ScriptStage::runtime_server, {}, 100, {}}));

    auto unloaded = runtime.value()->unload_module(module.module_id);
    assert(unloaded);
    assert(runtime.value()->module_count() == 0);
    assert(runtime.value()->find_module(module.module_id) == nullptr);
    assert(!runtime.value()->unload_module(module.module_id));

    ScriptModuleDesc bad_module = module;
    bad_module.module_id = "bad module id";
    assert(!validate_script_module_desc(bad_module, 128));

    bad_module = module;
    bad_module.source_mod_id = "Bad";
    assert(!validate_script_module_desc(bad_module, 128));

    bad_module = module;
    bad_module.source = std::string(129, 'x');
    assert(!validate_script_module_desc(bad_module, 128));

    bad_module = module;
    bad_module.permissions.push_back(ScriptPermission::read_prototypes);
    assert(!validate_script_module_desc(bad_module, 128));

    auto duplicate_permission_call = runtime.value()->call(
        ScriptCallDesc{module.module_id,
                       "ping",
                       ScriptStage::runtime_server,
                       {ScriptValue::string("hello")},
                       100,
                       {ScriptPermission::read_assets, ScriptPermission::read_assets}});
    assert(!duplicate_permission_call);
    assert(duplicate_permission_call.error().code == "scripting.duplicate_permission");

    if (!luau_info.available) {
        auto unavailable = create_script_runtime(ScriptRuntimeDesc{ScriptBackend::luau, 512});
        assert(!unavailable);
        assert(unavailable.error().code == "scripting.backend_unavailable");
        return;
    }

    ScriptRuntimeDesc luau_runtime_desc{ScriptBackend::luau, 512};
    luau_runtime_desc.host_apis = {set_voxel_api};
    auto luau_runtime = create_script_runtime(luau_runtime_desc);
    assert(luau_runtime);
    assert(luau_runtime.value()->backend() == ScriptBackend::luau);
    assert(luau_runtime.value()->backend_name() == "luau");

    ScriptModuleDesc luau_module = module;
    luau_module.source = "return {"
                         " ping = function(value) return value end,"
                         " truth = function() return true end,"
                         " answer = function() return 42 end,"
                         " text = function() return \"ok\" end,"
                         " big_text = function() return \"hello\" end,"
                         " notify = function(chunk, voxel, cell)"
                         "   return emit(\"world.set_voxel\", chunk, voxel, cell)"
                         " end,"
                         " notify_bad_type = function()"
                         "   return emit(\"world.set_voxel\", 1, \"1|2|3\", \"4|0\")"
                         " end,"
                         " notify_missing_args = function() return emit(\"world.set_voxel\") end"
                         "}";
    assert(luau_runtime.value()->load_module(luau_module));
    assert(luau_runtime.value()->module_count() == 1);

    auto limited_luau_runtime =
        create_script_runtime(ScriptRuntimeDesc{ScriptBackend::luau, 512, 1, 1, 4});
    assert(limited_luau_runtime);
    assert(limited_luau_runtime.value()->load_module(luau_module));
    ScriptModuleDesc second_luau_module = luau_module;
    second_luau_module.module_id = "base:scripts/runtime_server/second_luau";
    auto limited_luau_second_load = limited_luau_runtime.value()->load_module(second_luau_module);
    assert(!limited_luau_second_load);
    assert(limited_luau_second_load.error().code == "scripting.module_limit_reached");
    auto limited_luau_large_string =
        limited_luau_runtime.value()->call(ScriptCallDesc{luau_module.module_id,
                                                          "ping",
                                                          ScriptStage::runtime_server,
                                                          {ScriptValue::string("hello")},
                                                          32,
                                                          {ScriptPermission::read_prototypes}});
    assert(!limited_luau_large_string);
    assert(limited_luau_large_string.error().code == "scripting.string_argument_too_large");
    auto limited_luau_large_return = limited_luau_runtime.value()->call(
        ScriptCallDesc{luau_module.module_id, "big_text", ScriptStage::runtime_server, {}, 32, {}});
    assert(!limited_luau_large_return);
    assert(limited_luau_large_return.error().code == "scripting.string_return_too_large");

    auto echo_call = luau_runtime.value()->call(
        ScriptCallDesc{luau_module.module_id,
                       "ping",
                       ScriptStage::runtime_server,
                       {ScriptValue::string("hello")},
                       32,
                       {ScriptPermission::read_prototypes, ScriptPermission::emit_commands}});
    assert(echo_call);
    assert(echo_call.value().return_value.kind == ScriptValueKind::string);
    assert(echo_call.value().return_value.string_value == "hello");
    assert(echo_call.value().consumed_instruction_estimate > 0);

    auto truth_call = luau_runtime.value()->call(
        ScriptCallDesc{luau_module.module_id, "truth", ScriptStage::runtime_server, {}, 32, {}});
    assert(truth_call);
    assert(truth_call.value().return_value.kind == ScriptValueKind::boolean);
    assert(truth_call.value().return_value.boolean_value);

    auto answer_call = luau_runtime.value()->call(
        ScriptCallDesc{luau_module.module_id, "answer", ScriptStage::runtime_server, {}, 32, {}});
    assert(answer_call);
    assert(answer_call.value().return_value.kind == ScriptValueKind::number);
    assert(answer_call.value().return_value.number_value == 42.0);

    auto text_call = luau_runtime.value()->call(
        ScriptCallDesc{luau_module.module_id, "text", ScriptStage::runtime_server, {}, 32, {}});
    assert(text_call);
    assert(text_call.value().return_value.string_value == "ok");

    auto unregistered_luau_runtime =
        create_script_runtime(ScriptRuntimeDesc{ScriptBackend::luau, 512});
    assert(unregistered_luau_runtime);
    assert(unregistered_luau_runtime.value()->load_module(luau_module));
    auto unregistered_event_call = unregistered_luau_runtime.value()->call(ScriptCallDesc{
        luau_module.module_id,
        "notify",
        ScriptStage::runtime_server,
        {ScriptValue::string("0|0|0"), ScriptValue::string("1|2|3"), ScriptValue::string("4|0")},
        32,
        {ScriptPermission::emit_commands}});
    assert(!unregistered_event_call);
    assert(unregistered_event_call.error().code == "scripting.host_api_not_registered");

    ScriptCallDesc notify_call_desc{luau_module.module_id,
                                    "notify",
                                    ScriptStage::runtime_server,
                                    {ScriptValue::string("1099511627776|0|-1099511627776"),
                                     ScriptValue::string("1|2|3"), ScriptValue::string("4|0")},
                                    32,
                                    {ScriptPermission::emit_commands}};
    auto event_call = luau_runtime.value()->call(notify_call_desc);
    assert(event_call);
    assert(event_call.value().return_value.kind == ScriptValueKind::nil);
    assert(event_call.value().emitted_events.size() == 1);
    assert(event_call.value().emitted_events[0].api_id == "world.set_voxel");
    assert(event_call.value().emitted_events[0].arguments.size() == 3);
    assert(event_call.value().emitted_events[0].arguments[0].string_value ==
           "1099511627776|0|-1099511627776");

    const auto* loaded_luau_module = luau_runtime.value()->find_module(luau_module.module_id);
    assert(loaded_luau_module != nullptr);
    ScriptHostEventQueue host_event_queue(42);
    auto event_batch = host_event_queue.enqueue_from_call(*loaded_luau_module, notify_call_desc,
                                                          event_call.value(), luau_runtime_desc);
    assert(event_batch);
    assert(validate_script_host_event_batch(event_batch.value()));
    assert(event_batch.value().first_sequence == 42);
    assert(event_batch.value().last_sequence == 42);
    assert(event_batch.value().events.size() == 1);
    assert(host_event_queue.pending_count() == 1);
    assert(host_event_queue.next_sequence() == 43);
    assert(event_batch.value().events[0].api_id == "world.set_voxel");
    assert(event_batch.value().events[0].module_id == luau_module.module_id);
    assert(event_batch.value().events[0].source_mod_id == "base");
    assert(event_batch.value().events[0].stage == ScriptStage::runtime_server);
    assert(event_batch.value().events[0].function_name == "notify");
    assert(event_batch.value().events[0].module_api_version == luau_module.api_version);
    assert(event_batch.value().events[0].arguments.size() == 3);
    assert(event_batch.value().events[0].arguments[2].string_value == "4|0");

    ScriptCallResult unregistered_result;
    unregistered_result.emitted_events.push_back(ScriptEmittedEvent{"world.missing", {}});
    auto rejected_event_batch = host_event_queue.enqueue_from_call(
        *loaded_luau_module, notify_call_desc, unregistered_result, luau_runtime_desc);
    assert(!rejected_event_batch);
    assert(rejected_event_batch.error().code == "scripting.host_api_not_registered");
    assert(host_event_queue.pending_count() == 1);
    assert(host_event_queue.next_sequence() == 43);

    auto wrong_emit_type = luau_runtime.value()->call(ScriptCallDesc{
        luau_module.module_id, "notify_bad_type", ScriptStage::runtime_server, {}, 32, {}});
    assert(!wrong_emit_type);
    assert(wrong_emit_type.error().code == "scripting.host_api_argument_type_mismatch");

    auto missing_emit_args = luau_runtime.value()->call(ScriptCallDesc{
        luau_module.module_id, "notify_missing_args", ScriptStage::runtime_server, {}, 32, {}});
    assert(!missing_emit_args);
    assert(missing_emit_args.error().code == "scripting.host_api_argument_count_mismatch");

    auto drained_events = host_event_queue.drain();
    assert(drained_events.size() == 1);
    assert(validate_script_host_event(drained_events[0]));
    assert(drained_events[0].sequence == 42);
    assert(host_event_queue.empty());

    ScriptHostApiDesc future_api = set_voxel_api;
    future_api.api_id = "world.future";
    future_api.min_module_api_version = 3;
    ScriptRuntimeDesc future_runtime_desc{ScriptBackend::luau, 512};
    future_runtime_desc.host_apis = {future_api};
    auto future_runtime = create_script_runtime(future_runtime_desc);
    assert(future_runtime);
    ScriptModuleDesc future_module = module;
    future_module.module_id = "base:scripts/runtime_server/future";
    future_module.source =
        "return { notify = function() return emit(\"world.future\", \"0|0|0\", \"1|2|3\", "
        "\"4|0\") end }";
    assert(future_runtime.value()->load_module(future_module));
    auto future_api_call = future_runtime.value()->call(
        ScriptCallDesc{future_module.module_id, "notify", ScriptStage::runtime_server, {}, 32, {}});
    assert(!future_api_call);
    assert(future_api_call.error().code == "scripting.host_api_version_unsupported");

    auto luau_permission_denied =
        luau_runtime.value()->call(ScriptCallDesc{luau_module.module_id,
                                                  "truth",
                                                  ScriptStage::runtime_server,
                                                  {},
                                                  32,
                                                  {ScriptPermission::write_mod_state}});
    assert(!luau_permission_denied);
    assert(luau_permission_denied.error().code == "scripting.permission_denied");

    auto wrong_stage = luau_runtime.value()->call(
        ScriptCallDesc{luau_module.module_id, "truth", ScriptStage::runtime_client, {}, 32, {}});
    assert(!wrong_stage);
    assert(wrong_stage.error().code == "scripting.stage_mismatch");

    auto missing_function = luau_runtime.value()->call(
        ScriptCallDesc{luau_module.module_id, "missing", ScriptStage::runtime_server, {}, 32, {}});
    assert(!missing_function);
    assert(missing_function.error().code == "scripting.function_not_found");

    auto wrong_argument_count = luau_runtime.value()->call(
        ScriptCallDesc{luau_module.module_id, "ping", ScriptStage::runtime_server, {}, 32, {}});
    assert(!wrong_argument_count);
    assert(wrong_argument_count.error().code == "scripting.argument_count_mismatch");

    auto too_little_budget = luau_runtime.value()->call(
        ScriptCallDesc{luau_module.module_id, "truth", ScriptStage::runtime_server, {}, 1, {}});
    assert(!too_little_budget);
    assert(too_little_budget.error().code == "scripting.instruction_budget_exceeded");

    ScriptModuleDesc unsupported_module = module;
    unsupported_module.module_id = "base:scripts/runtime_server/unsupported";
    unsupported_module.source = "return os.execute('bad')";
    auto unsupported_loaded = luau_runtime.value()->load_module(unsupported_module);
    assert(!unsupported_loaded);
    assert(unsupported_loaded.error().code == "scripting.module_initialization_failed");

    ScriptModuleDesc invalid_event_module = module;
    invalid_event_module.module_id = "base:scripts/runtime_server/invalid_event";
    invalid_event_module.source = "return { bad = function() return emit(\".bad\") end }";
    assert(luau_runtime.value()->load_module(invalid_event_module));
    auto invalid_event_call = luau_runtime.value()->call(ScriptCallDesc{
        invalid_event_module.module_id, "bad", ScriptStage::runtime_server, {}, 32, {}});
    assert(!invalid_event_call);
    assert(invalid_event_call.error().code == "scripting.invalid_host_api_id");
    assert(luau_runtime.value()->unload_module(invalid_event_module.module_id));

    ScriptModuleDesc malformed_emit_module = module;
    malformed_emit_module.module_id = "base:scripts/runtime_server/malformed_emit";
    malformed_emit_module.source = "return { bad = function() return emit() end }";
    assert(luau_runtime.value()->load_module(malformed_emit_module));
    auto malformed_emit_call = luau_runtime.value()->call(ScriptCallDesc{
        malformed_emit_module.module_id, "bad", ScriptStage::runtime_server, {}, 32, {}});
    assert(!malformed_emit_call);
    assert(malformed_emit_call.error().code == "scripting.invalid_host_api_id");
    assert(luau_runtime.value()->unload_module(malformed_emit_module.module_id));

    ScriptModuleDesc unauthorized_emit_module = module;
    unauthorized_emit_module.module_id = "base:scripts/runtime_server/unauthorized_emit";
    unauthorized_emit_module.permissions = {ScriptPermission::read_prototypes};
    unauthorized_emit_module.source =
        "return { notify = function() return emit(\"world.set_voxel\", \"0|0|0\", \"1|2|3\", "
        "\"4|0\") end }";
    assert(luau_runtime.value()->load_module(unauthorized_emit_module));
    auto unauthorized_emit_call = luau_runtime.value()->call(ScriptCallDesc{
        unauthorized_emit_module.module_id, "notify", ScriptStage::runtime_server, {}, 32, {}});
    assert(!unauthorized_emit_call);
    assert(unauthorized_emit_call.error().code == "scripting.permission_denied");
    assert(luau_runtime.value()->unload_module(unauthorized_emit_module.module_id));

    ScriptModuleDesc unknown_parameter_module = module;
    unknown_parameter_module.module_id = "base:scripts/runtime_server/unknown_parameter";
    unknown_parameter_module.source = "return { bad = function(";
    auto unknown_parameter_loaded = luau_runtime.value()->load_module(unknown_parameter_module);
    assert(!unknown_parameter_loaded);
    assert(unknown_parameter_loaded.error().code == "scripting.luau_compile_error");

    assert(luau_runtime.value()->unload_module(luau_module.module_id));
    assert(luau_runtime.value()->module_count() == 0);

    assert(script_stage_name(ScriptStage::migration) == "migration");
    assert(script_permission_name(ScriptPermission::client_ui) == "client_ui");
    assert(script_value_kind_name(ScriptValueKind::boolean) == "boolean");
}

void test_script_module_loading_from_mod_lifecycle() {
    const auto root = make_temp_root();
    const auto mods_root = root / "mods";

    write_text(mods_root / "base/mod.toml", "id = \"base\"\n"
                                            "name = \"Heartstead Base\"\n"
                                            "version = \"0.0.1\"\n");
    write_text(mods_root / "base/scripts/runtime_server/tick.luau",
               "-- heartstead.permissions = \"read_prototypes, emit_commands\"\n"
               "-- heartstead.api_version = \"2\"\n"
               "return { tick = function(value) return value end }\n");
    write_text(mods_root / "base/scripts/runtime_client/hud.lua",
               "-- heartstead.permissions = \"read_assets, client_ui\"\n"
               "return { draw = function() return true end }\n");
    write_text(mods_root / "base/migrations/0001_schema.luau",
               "-- heartstead.permissions = \"read_save, write_mod_state\"\n"
               "return { migrate = function() return true end }\n");

    heartstead::modding::ModDiscoverer discoverer;
    auto discovery = discoverer.discover(mods_root);
    assert(!discovery.has_errors());

    auto lifecycle_plan = heartstead::modding::ModLifecyclePlanner::build(discovery.mods);
    assert(!lifecycle_plan.has_errors());

    auto loaded =
        heartstead::scripting::ScriptModuleLoader::load_from_plan(discovery.mods, lifecycle_plan);
    assert(!loaded.has_errors());
    assert(loaded.modules.size() == 3);
    assert(loaded.count_stage(heartstead::scripting::ScriptStage::runtime_server) == 1);
    assert(loaded.count_stage(heartstead::scripting::ScriptStage::runtime_client) == 1);
    assert(loaded.count_stage(heartstead::scripting::ScriptStage::migration) == 1);

    const auto validation = heartstead::modding::ModValidation::validate(mods_root);
    assert(!validation.has_errors());

    if (heartstead::scripting::script_backend_info(heartstead::scripting::ScriptBackend::luau)
            .available) {
        write_text(mods_root / "base/scripts/runtime_server/tick.luau",
                   "return { tick = function(\n");
        const auto malformed_validation = heartstead::modding::ModValidation::validate(mods_root);
        assert(malformed_validation.has_errors());
        assert(std::ranges::any_of(malformed_validation.diagnostics, [](const auto& diagnostic) {
            return diagnostic.code == "mod.scripting.module_invalid";
        }));
        write_text(mods_root / "base/scripts/runtime_server/tick.luau",
                   "-- heartstead.permissions = \"read_prototypes, emit_commands\"\n"
                   "-- heartstead.api_version = \"2\"\n"
                   "return { tick = function(value) return value end }\n");
    }

    const auto bounded = heartstead::scripting::ScriptModuleLoader::load_from_plan(
        discovery.mods, lifecycle_plan, {.max_source_bytes = 16});
    assert(bounded.has_errors());
    assert(std::ranges::any_of(bounded.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "scripting.source_too_large";
    }));
    const auto module_limited = heartstead::scripting::ScriptModuleLoader::load_from_plan(
        discovery.mods, lifecycle_plan, {.max_modules = 1});
    assert(module_limited.has_errors());
    assert(std::ranges::any_of(module_limited.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "scripting.module.module_limit_reached";
    }));
    const auto invalid_limits = heartstead::scripting::ScriptModuleLoader::load_from_plan(
        discovery.mods, lifecycle_plan, {.max_source_bytes = 0});
    assert(invalid_limits.has_errors());
    assert(invalid_limits.diagnostics.front().code == "scripting.module.invalid_loader_limits");

    write_text(root / "outside.luau", "return {}\n");
    heartstead::modding::ModLifecyclePlan forged_plan;
    forged_plan.tasks.push_back({heartstead::modding::ModLifecycleStage::runtime_server,
                                 heartstead::modding::ModLifecycleTaskKind::runtime_server_script,
                                 "base", root / "outside.luau"});
    const auto forged =
        heartstead::scripting::ScriptModuleLoader::load_from_plan(discovery.mods, forged_plan);
    assert(forged.has_errors());
    assert(forged.diagnostics.front().code == "scripting.module.unsafe_source_path");

    const auto tick_module = std::ranges::find(loaded.modules, "base:scripts/runtime_server/tick",
                                               &heartstead::scripting::ScriptModuleDesc::module_id);
    assert(tick_module != loaded.modules.end());
    assert(tick_module->source_mod_id == "base");
    assert(tick_module->stage == heartstead::scripting::ScriptStage::runtime_server);
    assert(tick_module->api_version == 2);
    assert(tick_module->source.find("tick") != std::string::npos);
    assert(tick_module->permissions.size() == 2);
    assert(tick_module->permissions[0] == heartstead::scripting::ScriptPermission::read_prototypes);
    assert(tick_module->permissions[1] == heartstead::scripting::ScriptPermission::emit_commands);
    assert(tick_module->source.find("heartstead.permissions") == std::string::npos);
    assert(tick_module->source.find("heartstead.api_version") == std::string::npos);

    if (heartstead::scripting::script_backend_info(heartstead::scripting::ScriptBackend::luau)
            .available) {
        auto script_runtime =
            heartstead::scripting::create_script_runtime(heartstead::scripting::ScriptRuntimeDesc{
                heartstead::scripting::ScriptBackend::luau, 512});
        assert(script_runtime);
        assert(script_runtime.value()->load_module(*tick_module));
        auto tick_call =
            script_runtime.value()->call({tick_module->module_id,
                                          "tick",
                                          heartstead::scripting::ScriptStage::runtime_server,
                                          {heartstead::scripting::ScriptValue::number(42)},
                                          32,
                                          {}});
        assert(tick_call);
        assert(tick_call.value().return_value.kind ==
               heartstead::scripting::ScriptValueKind::number);
        assert(tick_call.value().return_value.number_value == 42);
    }

    const auto migration_module =
        std::ranges::find(loaded.modules, "base:migrations/0001_schema",
                          &heartstead::scripting::ScriptModuleDesc::module_id);
    assert(migration_module != loaded.modules.end());
    assert(migration_module->stage == heartstead::scripting::ScriptStage::migration);
    assert(migration_module->permissions.size() == 2);

    auto mod_report = heartstead::modding::ModValidation::validate(mods_root);
    assert(!mod_report.has_errors());
    assert(mod_report.script_modules.size() == 3);
    assert(mod_report.script_modules.front().module_id == "base:migrations/0001_schema");

    const auto resource_packs_root = root / "resource_packs";
    std::filesystem::create_directories(resource_packs_root);
    auto content_report =
        heartstead::content::ContentValidation::validate(mods_root, resource_packs_root);
    assert(!content_report.has_errors());
    assert(content_report.script_modules.size() == 3);

    const auto invalid_root = make_temp_root();
    const auto invalid_mods_root = invalid_root / "mods";
    write_text(invalid_mods_root / "base/mod.toml", "id = \"base\"\n"
                                                    "name = \"Heartstead Base\"\n"
                                                    "version = \"0.0.1\"\n");
    write_text(invalid_mods_root / "base/scripts/runtime_server/bad.luau",
               "-- heartstead.permissions = \"read_prototypes, unknown_permission\"\n"
               "return { bad = function() return true end }\n");
    write_text(invalid_mods_root / "base/scripts/runtime_server/too_many.luau",
               "-- heartstead.permissions = "
               "\"read_prototypes, read_assets, emit_commands, read_save, write_mod_state, "
               "client_ui, read_assets\"\n"
               "return {}\n");
    write_text(invalid_mods_root / "base/scripts/runtime_server/duplicate.luau",
               "-- heartstead.api_version = 1\n"
               "-- heartstead.api_version = 2\n"
               "return {}\n");
    write_text(invalid_mods_root / "base/scripts/runtime_server/mismatched.luau",
               "-- heartstead.permissions = \"read_assets\n"
               "return {}\n");
    write_text(invalid_mods_root / "base/migrations/readme.txt", "not a lua migration\n");

    auto invalid_report = heartstead::modding::ModValidation::validate(invalid_mods_root);
    assert(invalid_report.has_errors());
    assert(std::ranges::any_of(invalid_report.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "scripting.module.unknown_permission";
    }));
    assert(std::ranges::any_of(invalid_report.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "scripting.module.unsupported_extension";
    }));
    assert(std::ranges::any_of(invalid_report.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "scripting.module.too_many_permissions";
    }));
    assert(std::ranges::any_of(invalid_report.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "scripting.module.duplicate_api_version_directive";
    }));
    assert(std::ranges::any_of(invalid_report.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "scripting.module.invalid_directive_string";
    }));

    write_text(mods_root / "base/scripts/runtime_server/collision.lua", "return {}\n");
    write_text(mods_root / "base/scripts/runtime_server/collision.luau", "return {}\n");
    const auto collision_plan = heartstead::modding::ModLifecyclePlanner::build(discovery.mods);
    const auto collision_modules =
        heartstead::scripting::ScriptModuleLoader::load_from_plan(discovery.mods, collision_plan);
    assert(collision_modules.has_errors());
    assert(std::ranges::any_of(collision_modules.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "scripting.module.duplicate_id";
    }));
}

void test_job_system() {
    using namespace heartstead::jobs;

    const auto immediate_info = job_backend_info(JobBackend::immediate);
    assert(immediate_info.available);
    assert(immediate_info.name == "immediate");

    const auto thread_pool_info = job_backend_info(JobBackend::thread_pool);
    assert(thread_pool_info.available);
    assert(thread_pool_info.name == "thread_pool");

    assert(!create_job_system(JobSystemDesc{JobBackend::immediate, 0, 16}));
    assert(!create_job_system(JobSystemDesc{JobBackend::immediate, 1, 0}));
    assert(!create_job_system(JobSystemDesc{JobBackend::immediate, 1, 16, 0}));

    constexpr auto maximum_job_id = std::numeric_limits<std::uint64_t>::max();
    auto exhausting_immediate =
        create_job_system(JobSystemDesc{JobBackend::immediate, 1, 2, maximum_job_id});
    assert(exhausting_immediate);
    auto final_immediate_job = exhausting_immediate.value()->submit(
        JobDesc{"id.final.immediate", JobPriority::normal,
                [](const JobContext&) { return heartstead::core::Status::ok(); }});
    assert(final_immediate_job && final_immediate_job.value().value() == maximum_job_id);
    auto exhausted_immediate_job = exhausting_immediate.value()->submit(
        JobDesc{"id.exhausted.immediate", JobPriority::normal,
                [](const JobContext&) { return heartstead::core::Status::ok(); }});
    assert(!exhausted_immediate_job);
    assert(exhausted_immediate_job.error().code == "jobs.id_range_exhausted");

    auto exhausting_pool =
        create_job_system(JobSystemDesc{JobBackend::thread_pool, 1, 2, maximum_job_id});
    assert(exhausting_pool);
    auto final_threaded_job = exhausting_pool.value()->submit(
        JobDesc{"id.final.threaded", JobPriority::normal,
                [](const JobContext&) { return heartstead::core::Status::ok(); }});
    assert(final_threaded_job && final_threaded_job.value().value() == maximum_job_id);
    auto exhausted_threaded_job = exhausting_pool.value()->submit(
        JobDesc{"id.exhausted.threaded", JobPriority::normal,
                [](const JobContext&) { return heartstead::core::Status::ok(); }});
    assert(!exhausted_threaded_job);
    assert(exhausted_threaded_job.error().code == "jobs.id_range_exhausted");
    assert(wait_for_completed_jobs(*exhausting_pool.value(), 1));
    assert(exhausting_pool.value()->drain_completed().size() == 1);

    auto system = create_job_system(JobSystemDesc{JobBackend::immediate, 1, 2});
    assert(system);
    assert(system.value()->backend() == JobBackend::immediate);
    assert(system.value()->backend_name() == "immediate");
    assert(system.value()->pending_count() == 0);
    assert(system.value()->submitted_count() == 0);
    assert(system.value()->completed_count() == 0);

    assert(!system.value()->submit(JobDesc{"", JobPriority::normal, [](const JobContext&) {
                                               return heartstead::core::Status::ok();
                                           }}));
    assert(!system.value()->submit(JobDesc{"missing.work", JobPriority::normal, {}}));

    bool first_ran = false;
    auto first = system.value()->submit(JobDesc{
        "chunk.mesh.rebuild",
        JobPriority::high,
        [&first_ran](const JobContext& context) {
            assert(context.id.is_valid());
            assert(context.name == "chunk.mesh.rebuild");
            assert(context.priority == JobPriority::high);
            first_ran = true;
            return heartstead::core::Status::ok();
        },
    });
    assert(first);
    assert(first.value().value() == 1);
    assert(first_ran);

    auto second = system.value()->submit(JobDesc{
        "asset.cook.fail",
        JobPriority::low,
        [](const JobContext&) {
            return heartstead::core::Status::failure("job_test.expected_failure",
                                                     "intentional failure");
        },
    });
    assert(second);
    assert(second.value().value() == 2);

    assert(system.value()->submitted_count() == 2);
    assert(system.value()->completed_count() == 2);

    auto full = system.value()->submit(JobDesc{
        "queue.full",
        JobPriority::normal,
        [](const JobContext&) { return heartstead::core::Status::ok(); },
    });
    assert(!full);
    assert(full.error().code == "jobs.completed_queue_full");

    auto completed = system.value()->drain_completed();
    assert(completed.size() == 2);
    assert(completed[0].id == first.value());
    assert(completed[0].state == JobState::succeeded);
    assert(completed[0].completion_order == 1);
    assert(completed[1].id == second.value());
    assert(completed[1].state == JobState::failed);
    assert(completed[1].error_code == "job_test.expected_failure");
    assert(system.value()->drain_completed().empty());

    auto after_drain = system.value()->submit(JobDesc{
        "after.drain",
        JobPriority::normal,
        [](const JobContext&) { return heartstead::core::Status::ok(); },
    });
    assert(after_drain);
    assert(after_drain.value().value() == 3);
    auto immediate_exception = system.value()->submit(JobDesc{
        "immediate.exception",
        JobPriority::normal,
        [](const JobContext&) -> heartstead::core::Status {
            throw std::runtime_error("immediate callback failure");
        },
    });
    assert(immediate_exception);
    auto immediate_exception_results = system.value()->drain_completed();
    assert(immediate_exception_results.size() == 2);
    assert(immediate_exception_results[1].id == immediate_exception.value());
    assert(immediate_exception_results[1].state == JobState::failed);
    assert(immediate_exception_results[1].error_code == "jobs.callback_exception");
    assert(immediate_exception_results[1].error_message.find("immediate callback failure") !=
           std::string::npos);

    auto thread_pool = create_job_system(JobSystemDesc{JobBackend::thread_pool, 1, 8});
    assert(thread_pool);
    assert(thread_pool.value()->backend() == JobBackend::thread_pool);
    assert(thread_pool.value()->backend_name() == "thread_pool");
    assert(thread_pool.value()->submitted_count() == 0);
    assert(thread_pool.value()->completed_count() == 0);

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool blocker_started = false;
    bool release_blocker = false;

    auto blocker = thread_pool.value()->submit(JobDesc{
        "thread.blocker",
        JobPriority::normal,
        [&](const JobContext& context) {
            assert(context.id.is_valid());
            assert(context.name == "thread.blocker");

            std::unique_lock lock(gate_mutex);
            blocker_started = true;
            gate_changed.notify_all();
            gate_changed.wait(lock, [&release_blocker] { return release_blocker; });
            return heartstead::core::Status::ok();
        },
    });
    assert(blocker);

    {
        std::unique_lock lock(gate_mutex);
        assert(gate_changed.wait_for(lock, std::chrono::seconds(2),
                                     [&blocker_started] { return blocker_started; }));
    }

    auto low = thread_pool.value()->submit(JobDesc{
        "thread.low",
        JobPriority::low,
        [](const JobContext& context) {
            assert(context.priority == JobPriority::low);
            return heartstead::core::Status::ok();
        },
    });
    assert(low);

    auto high = thread_pool.value()->submit(JobDesc{
        "thread.high.fail",
        JobPriority::high,
        [](const JobContext& context) {
            assert(context.priority == JobPriority::high);
            return heartstead::core::Status::failure("job_test.thread_failure",
                                                     "intentional threaded failure");
        },
    });
    assert(high);
    assert(thread_pool.value()->submitted_count() == 3);
    assert(thread_pool.value()->pending_count() == 3);

    {
        std::lock_guard lock(gate_mutex);
        release_blocker = true;
    }
    gate_changed.notify_all();

    assert(wait_for_completed_jobs(*thread_pool.value(), 3));
    assert(thread_pool.value()->pending_count() == 0);
    auto threaded_results = thread_pool.value()->drain_completed();
    assert(threaded_results.size() == 3);
    assert(threaded_results[0].id == blocker.value());
    assert(threaded_results[0].state == JobState::succeeded);
    assert(threaded_results[0].completion_order == 1);
    assert(threaded_results[1].id == high.value());
    assert(threaded_results[1].state == JobState::failed);
    assert(threaded_results[1].error_code == "job_test.thread_failure");
    assert(threaded_results[1].completion_order == 2);
    assert(threaded_results[2].id == low.value());
    assert(threaded_results[2].state == JobState::succeeded);
    assert(threaded_results[2].completion_order == 3);

    auto threaded_exception = thread_pool.value()->submit(JobDesc{
        "thread.exception",
        JobPriority::normal,
        [](const JobContext&) -> heartstead::core::Status { throw 7; },
    });
    assert(threaded_exception);
    assert(wait_for_completed_jobs(*thread_pool.value(), 4));
    auto threaded_exception_results = thread_pool.value()->drain_completed();
    assert(threaded_exception_results.size() == 1);
    assert(threaded_exception_results.front().id == threaded_exception.value());
    assert(threaded_exception_results.front().state == JobState::failed);
    assert(threaded_exception_results.front().error_code == "jobs.callback_exception");
    assert(threaded_exception_results.front().error_message ==
           "job callback threw a non-standard exception");

    auto capacity_pool = create_job_system(JobSystemDesc{JobBackend::thread_pool, 1, 1});
    assert(capacity_pool);
    auto capacity_first = capacity_pool.value()->submit(JobDesc{
        "thread.capacity.first",
        JobPriority::normal,
        [](const JobContext&) { return heartstead::core::Status::ok(); },
    });
    assert(capacity_first);
    assert(wait_for_completed_jobs(*capacity_pool.value(), 1));
    auto capacity_full = capacity_pool.value()->submit(JobDesc{
        "thread.capacity.full",
        JobPriority::normal,
        [](const JobContext&) { return heartstead::core::Status::ok(); },
    });
    assert(!capacity_full);
    assert(capacity_full.error().code == "jobs.completed_queue_full");
    assert(capacity_pool.value()->drain_completed().size() == 1);
    auto capacity_after_drain = capacity_pool.value()->submit(JobDesc{
        "thread.capacity.after_drain",
        JobPriority::normal,
        [](const JobContext&) { return heartstead::core::Status::ok(); },
    });
    assert(capacity_after_drain);

    assert(job_priority_name(JobPriority::high) == "high");
    assert(job_state_name(JobState::cancelled) == "cancelled");
}

void test_mod_discovery_and_prototypes() {
    const auto root = make_temp_root();
    const auto mods_root = root / "mods";

    write_text(mods_root / "base/mod.toml", "id = \"base\"\n"
                                            "name = \"Heartstead Base\"\n"
                                            "version = \"0.0.1\"\n"
                                            "description = \"Test # base mod\"\n");

    write_text(mods_root / "base/data/items/raw_clay.prototype.toml",
               "kind = \"item\"\n"
               "id = \"base:items/raw_clay\"\n"
               "display_name = \"Raw Clay\"\n"
               "stack_limit = \"64\"\n");
    write_text(mods_root / "tweaks/mod.toml", "id = \"tweaks\"\n"
                                              "name = \"Patch Test Tweaks\"\n"
                                              "version = \"0.0.1\"\n"
                                              "dependencies = \"base\"\n");
    write_text(mods_root / "tweaks/data/items/raw_clay.prototype_patch.toml",
               "target = \"base:items/raw_clay\"\n"
               "set.display_name = \"Packed Raw Clay\"\n"
               "set.stack_limit = \"32\"\n");
    write_text(mods_root / "tweaks/data/items/raw_clay.final_patch.toml",
               "target = \"base:items/raw_clay\"\n"
               "set.display_name = \"Final Raw Clay\"\n");

    heartstead::modding::ModDiscoverer discoverer;
    auto discovery = discoverer.discover(mods_root);
    assert(!discovery.has_errors());
    assert(discovery.mods.size() == 2);
    assert(discovery.mods.front().id == "base");
    assert(discovery.mods.front().description == "Test # base mod");
    assert(discovery.mods.back().id == "tweaks");
    assert(discovery.mods.back().dependencies.size() == 1);
    assert(discovery.mods.back().dependencies.front() == "base");

    heartstead::modding::GenericPrototypeLoader loader;
    auto prototypes = loader.load_from_mods(discovery.mods);
    assert(!prototypes.has_errors());
    assert(prototypes.prototypes.size() == 1);
    assert(prototypes.prototype_patches.size() == 2);
    assert(prototypes.applied_patch_count == 2);
    assert(prototypes.prototypes.front().id.value() == "base:items/raw_clay");
    assert(prototypes.prototypes.front().kind == "item");
    assert(prototypes.prototypes.front().display_name == "Final Raw Clay");
    assert(prototypes.prototypes.front().fields.at("stack_limit") == "32");
    assert(prototypes.prototype_patches.front().source_mod_id == "tweaks");
    assert(prototypes.prototype_patches.front().stage ==
           heartstead::modding::GenericPrototypePatchStage::data_update);
    assert(prototypes.prototype_patches.front().target_id.value() == "base:items/raw_clay");
    assert(prototypes.prototype_patches.back().stage ==
           heartstead::modding::GenericPrototypePatchStage::final_fix);

    const auto invalid_mods_root = root / "invalid_mods";
    write_text(invalid_mods_root / "bad/mod.toml", "id = \"bad\"\n"
                                                   "name = \"Bad\"\n"
                                                   "name = \"Duplicate\"\n"
                                                   "version = \"1\"\n"
                                                   "surprise = true\n");
    const auto invalid_discovery = discoverer.discover(invalid_mods_root);
    assert(invalid_discovery.has_errors());
    assert(invalid_discovery.mods.empty());
    assert(std::ranges::any_of(invalid_discovery.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "mod.manifest.duplicate_key";
    }));
    assert(std::ranges::any_of(invalid_discovery.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "mod.manifest.unknown_field";
    }));

    write_text(mods_root / "base/data/items/raw_clay.prototype.toml",
               "kind = \"item\"\n"
               "id = \"base:items/raw_clay\"\n"
               "display_name = \"Raw Clay\"\n"
               "display_name = \"Duplicate\"\n");
    const auto invalid_prototypes = loader.load_from_mods(discovery.mods);
    assert(invalid_prototypes.has_errors());
    assert(invalid_prototypes.prototypes.empty());
    assert(std::ranges::any_of(invalid_prototypes.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "prototype.duplicate_key";
    }));

    std::error_code prototype_link_error;
    std::filesystem::create_symlink(mods_root / "base/data/items/raw_clay.prototype.toml",
                                    mods_root / "base/data/items/linked.prototype.toml",
                                    prototype_link_error);
    if (!prototype_link_error) {
        const auto linked_prototypes = loader.load_from_mods(discovery.mods);
        assert(linked_prototypes.has_errors());
        assert(std::ranges::any_of(linked_prototypes.diagnostics, [](const auto& diagnostic) {
            return diagnostic.code == "core.directory_symlink_forbidden";
        }));
    }

    const auto linked_manifest_root = root / "linked_manifest_mods";
    const auto external_manifest = root / "external_mod.toml";
    write_text(external_manifest, "id = \"linked\"\n"
                                  "name = \"Linked\"\n"
                                  "version = \"1\"\n");
    std::filesystem::create_directories(linked_manifest_root / "linked");
    std::error_code manifest_link_error;
    std::filesystem::create_symlink(external_manifest, linked_manifest_root / "linked/mod.toml",
                                    manifest_link_error);
    if (!manifest_link_error) {
        const auto linked_manifest_discovery = discoverer.discover(linked_manifest_root);
        assert(linked_manifest_discovery.has_errors());
        assert(
            std::ranges::any_of(linked_manifest_discovery.diagnostics, [](const auto& diagnostic) {
                return diagnostic.code == "mod.manifest.unsafe_file";
            }));
    }

    const auto linked_directory_root = root / "linked_directory_mods";
    std::filesystem::create_directories(linked_directory_root);
    std::error_code directory_link_error;
    std::filesystem::create_directory_symlink(mods_root / "base", linked_directory_root / "linked",
                                              directory_link_error);
    if (!directory_link_error) {
        const auto linked_directory_discovery = discoverer.discover(linked_directory_root);
        assert(linked_directory_discovery.has_errors());
        assert(linked_directory_discovery.diagnostics.front().code ==
               "core.directory_symlink_forbidden");
    }
}

void test_mod_dependency_discovery() {
    const auto root = make_temp_root();
    const auto mods_root = root / "mods";

    write_text(mods_root / "addon/mod.toml", "id = \"addon\"\n"
                                             "name = \"Addon\"\n"
                                             "version = \"1.0.0\"\n"
                                             "dependencies = \"coremod\"\n");
    write_text(mods_root / "coremod/mod.toml", "id = \"coremod\"\n"
                                               "name = \"Core Mod\"\n"
                                               "version = \"1.0.0\"\n");
    write_text(mods_root / "patcher/mod.toml", "id = \"patcher\"\n"
                                               "name = \"Patcher\"\n"
                                               "version = \"1.0.0\"\n"
                                               "dependencies = \"addon, coremod\"\n");

    heartstead::modding::ModDiscoverer discoverer;
    auto discovery = discoverer.discover(mods_root);
    assert(!discovery.has_errors());
    assert(discovery.mods.size() == 3);
    assert(discovery.mods[0].id == "coremod");
    assert(discovery.mods[1].id == "addon");
    assert(discovery.mods[2].id == "patcher");
    assert(discovery.mods[2].dependencies.size() == 2);

    const auto missing_root = make_temp_root();
    const auto missing_mods_root = missing_root / "mods";
    write_text(missing_mods_root / "addon/mod.toml", "id = \"addon\"\n"
                                                     "name = \"Addon\"\n"
                                                     "version = \"1.0.0\"\n"
                                                     "dependencies = \"missing_mod\"\n");
    discovery = discoverer.discover(missing_mods_root);
    assert(discovery.has_errors());
    assert(std::ranges::any_of(discovery.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "mod.manifest.missing_dependency";
    }));

    const auto cycle_root = make_temp_root();
    const auto cycle_mods_root = cycle_root / "mods";
    write_text(cycle_mods_root / "moda/mod.toml", "id = \"moda\"\n"
                                                  "name = \"Mod A\"\n"
                                                  "version = \"1.0.0\"\n"
                                                  "dependencies = \"modb\"\n");
    write_text(cycle_mods_root / "modb/mod.toml", "id = \"modb\"\n"
                                                  "name = \"Mod B\"\n"
                                                  "version = \"1.0.0\"\n"
                                                  "dependencies = \"moda\"\n");
    discovery = discoverer.discover(cycle_mods_root);
    assert(discovery.has_errors());
    assert(std::ranges::any_of(discovery.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "mod.manifest.dependency_cycle";
    }));
}

void test_mod_lifecycle_plan() {
    const auto root = make_temp_root();
    const auto mods_root = root / "mods";

    write_text(mods_root / "base/mod.toml", "id = \"base\"\n"
                                            "name = \"Heartstead Base\"\n"
                                            "version = \"0.0.1\"\n");
    write_text(mods_root / "base/data/items/raw_clay.prototype.toml",
               "kind = \"item\"\n"
               "id = \"base:items/raw_clay\"\n"
               "display_name = \"Raw Clay\"\n"
               "stack_limit = \"64\"\n");
    write_text(mods_root / "base/assets/textures/items/raw_clay.png", "not a real png");
    write_text(mods_root / "base/locale/en_us.toml", "raw_clay = \"Raw Clay\"\n");
    write_text(mods_root / "base/scripts/runtime_server/tick.luau", "return {}\n");
    write_text(mods_root / "base/scripts/runtime_client/hud.luau", "return {}\n");
    write_text(mods_root / "base/scripts/migration/0001_schema.luau", "return {}\n");

    write_text(mods_root / "tweaks/mod.toml", "id = \"tweaks\"\n"
                                              "name = \"Tweaks\"\n"
                                              "version = \"0.0.1\"\n"
                                              "dependencies = \"base\"\n");
    write_text(mods_root / "tweaks/data/items/raw_clay.prototype_patch.toml",
               "target = \"base:items/raw_clay\"\n"
               "set.display_name = \"Packed Raw Clay\"\n");
    write_text(mods_root / "tweaks/data/items/raw_clay.final_patch.toml",
               "target = \"base:items/raw_clay\"\n"
               "set.tags = \"final\"\n");

    heartstead::modding::ModDiscoverer discoverer;
    auto discovery = discoverer.discover(mods_root);
    assert(!discovery.has_errors());

    auto plan = heartstead::modding::ModLifecyclePlanner::build(discovery.mods);
    assert(!plan.has_errors());
    assert(plan.count_stage(heartstead::modding::ModLifecycleStage::settings) == 2);
    assert(plan.count_stage(heartstead::modding::ModLifecycleStage::prototypes) == 1);
    assert(plan.count_stage(heartstead::modding::ModLifecycleStage::data_updates) == 1);
    assert(plan.count_stage(heartstead::modding::ModLifecycleStage::final_fixes) == 1);
    assert(plan.count_stage(heartstead::modding::ModLifecycleStage::assets) == 2);
    assert(plan.count_stage(heartstead::modding::ModLifecycleStage::migration) == 1);
    assert(plan.count_stage(heartstead::modding::ModLifecycleStage::runtime_server) == 1);
    assert(plan.count_stage(heartstead::modding::ModLifecycleStage::runtime_client) == 1);

    const auto settings_tasks =
        plan.tasks_for_stage(heartstead::modding::ModLifecycleStage::settings);
    assert(settings_tasks.size() == 2);
    assert(settings_tasks[0].mod_id == "base");
    assert(settings_tasks[1].mod_id == "tweaks");
    assert(settings_tasks[0].kind == heartstead::modding::ModLifecycleTaskKind::manifest);

    const auto prototype_tasks =
        plan.tasks_for_stage(heartstead::modding::ModLifecycleStage::prototypes);
    assert(prototype_tasks.size() == 1);
    assert(prototype_tasks.front().mod_id == "base");
    assert(prototype_tasks.front().kind ==
           heartstead::modding::ModLifecycleTaskKind::prototype_definition);

    const auto patch_tasks =
        plan.tasks_for_stage(heartstead::modding::ModLifecycleStage::data_updates);
    assert(patch_tasks.size() == 1);
    assert(patch_tasks.front().mod_id == "tweaks");
    assert(patch_tasks.front().kind == heartstead::modding::ModLifecycleTaskKind::prototype_patch);
    const auto final_patch_tasks =
        plan.tasks_for_stage(heartstead::modding::ModLifecycleStage::final_fixes);
    assert(final_patch_tasks.size() == 1);
    assert(final_patch_tasks.front().kind ==
           heartstead::modding::ModLifecycleTaskKind::final_patch);

    const auto runtime_server_tasks =
        plan.tasks_for_stage(heartstead::modding::ModLifecycleStage::runtime_server);
    assert(runtime_server_tasks.size() == 1);
    assert(runtime_server_tasks.front().kind ==
           heartstead::modding::ModLifecycleTaskKind::runtime_server_script);

    auto report = heartstead::modding::ModValidation::validate(mods_root);
    assert(!report.has_errors());
    assert(report.lifecycle_plan.count_stage(heartstead::modding::ModLifecycleStage::settings) ==
           2);
    assert(report.lifecycle_plan.count_stage(
               heartstead::modding::ModLifecycleStage::runtime_client) == 1);

    const auto resource_packs_root = root / "resource_packs";
    std::filesystem::create_directories(resource_packs_root);
    auto content_report =
        heartstead::content::ContentValidation::validate(mods_root, resource_packs_root);
    assert(!content_report.has_errors());
    assert(content_report.lifecycle_plan.count_stage(
               heartstead::modding::ModLifecycleStage::runtime_server) == 1);

    auto lifecycle_inspection = heartstead::debug::Inspector::inspect(report.lifecycle_plan);
    assert(lifecycle_inspection.object_type == "mod_lifecycle_plan");
    assert(lifecycle_inspection.state == "valid");
    assert(lifecycle_inspection.find_field("stage_settings_count")->value == "2");
    assert(lifecycle_inspection.find_field("stage_runtime_server_count")->value == "1");
    assert(lifecycle_inspection.find_field("task_0_stage")->value == "settings");
    assert(lifecycle_inspection.find_field("task_0_kind")->value == "manifest");
    assert(lifecycle_inspection.issues.empty());

    assert(heartstead::modding::mod_lifecycle_stage_name(
               heartstead::modding::ModLifecycleStage::data_updates) == "data_updates");
    assert(heartstead::modding::mod_lifecycle_task_kind_name(
               heartstead::modding::ModLifecycleTaskKind::migration_script) == "migration_script");
    assert(heartstead::modding::generic_prototype_patch_stage_name(
               heartstead::modding::GenericPrototypePatchStage::final_fix) == "final_fix");

    const auto invalid_root = make_temp_root();
    const auto invalid_mods_root = invalid_root / "mods";
    write_text(invalid_mods_root / "base/mod.toml", "id = \"base\"\n"
                                                    "name = \"Heartstead Base\"\n"
                                                    "version = \"0.0.1\"\n");
    write_text(invalid_mods_root / "base/scripts/unknown/place.luau", "return {}\n");
    write_text(invalid_mods_root / "base/other/scripts/runtime_server/hidden.luau", "return {}\n");
    auto invalid_discovery = discoverer.discover(invalid_mods_root);
    assert(!invalid_discovery.has_errors());
    auto invalid_plan = heartstead::modding::ModLifecyclePlanner::build(invalid_discovery.mods);
    assert(invalid_plan.has_errors());
    assert(std::ranges::any_of(invalid_plan.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "mod.lifecycle.unknown_script_stage";
    }));
    assert(std::ranges::any_of(invalid_plan.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "mod.lifecycle.script_outside_stage";
    }));
    assert(invalid_plan.count_stage(heartstead::modding::ModLifecycleStage::runtime_server) == 0);
    lifecycle_inspection = heartstead::debug::Inspector::inspect(invalid_plan);
    assert(lifecycle_inspection.state == "invalid");
    assert(lifecycle_inspection.has_errors());
    assert(std::ranges::any_of(lifecycle_inspection.issues, [](const auto& issue) {
        return issue.code == "mod.lifecycle.unknown_script_stage";
    }));
    auto invalid_report = heartstead::modding::ModValidation::validate(invalid_mods_root);
    assert(invalid_report.has_errors());
    assert(std::ranges::any_of(invalid_report.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "mod.lifecycle.unknown_script_stage";
    }));

    std::error_code link_error;
    std::filesystem::create_symlink(invalid_mods_root / "base/scripts/unknown/place.luau",
                                    invalid_mods_root / "base/scripts/linked.luau", link_error);
    if (!link_error) {
        const auto linked_plan =
            heartstead::modding::ModLifecyclePlanner::build(invalid_discovery.mods);
        assert(linked_plan.has_errors());
        assert(std::ranges::any_of(linked_plan.diagnostics, [](const auto& diagnostic) {
            return diagnostic.code == "core.directory_symlink_forbidden";
        }));
    }
}

void test_mod_prototype_fingerprints() {
    heartstead::modding::ModManifest base;
    base.id = "base";
    base.name = "Heartstead Base";
    base.version = "0.0.1";

    const auto clay_id = heartstead::core::PrototypeId::parse("base:items/raw_clay");
    const auto nail_id = heartstead::core::PrototypeId::parse("base:items/nails");
    assert(clay_id);
    assert(nail_id);

    heartstead::modding::GenericPrototype clay;
    clay.kind = std::string(heartstead::modding::PrototypeKinds::item);
    clay.id = clay_id.value();
    clay.display_name = "Raw Clay";
    clay.fields.emplace("kind", "item");
    clay.fields.emplace("id", "base:items/raw_clay");
    clay.fields.emplace("display_name", "Raw Clay");
    clay.fields.emplace("stack_limit", "64");

    heartstead::modding::GenericPrototype nails;
    nails.kind = std::string(heartstead::modding::PrototypeKinds::item);
    nails.id = nail_id.value();
    nails.display_name = "Nails";
    nails.fields.emplace("kind", "item");
    nails.fields.emplace("id", "base:items/nails");
    nails.fields.emplace("display_name", "Nails");
    nails.fields.emplace("stack_limit", "128");

    auto fingerprints = heartstead::modding::build_mod_prototype_fingerprints(
        {base}, std::vector<heartstead::modding::GenericPrototype>{nails, clay});
    auto fingerprints_reordered = heartstead::modding::build_mod_prototype_fingerprints(
        {base}, std::vector<heartstead::modding::GenericPrototype>{clay, nails});
    assert(fingerprints.size() == 1);
    assert(fingerprints.front().id == "base");
    assert(fingerprints.front().version == "0.0.1");
    assert(fingerprints.front().prototype_count == 2);
    assert(fingerprints.front().patch_count == 0);
    assert(fingerprints.front().script_count == 0);
    assert(!fingerprints.front().prototype_hash.empty());
    assert(fingerprints.front().prototype_hash == fingerprints_reordered.front().prototype_hash);

    nails.fields["stack_limit"] = "64";
    auto changed = heartstead::modding::build_mod_prototype_fingerprints(
        {base}, std::vector<heartstead::modding::GenericPrototype>{clay, nails});
    assert(changed.front().prototype_hash != fingerprints.front().prototype_hash);

    auto saved_mods = heartstead::save::saved_mod_records_from_fingerprints(fingerprints);
    assert(saved_mods.size() == 1);
    assert(saved_mods.front().id == "base");
    assert(saved_mods.front().version == "0.0.1");
    assert(saved_mods.front().prototype_hash == fingerprints.front().prototype_hash);

    heartstead::save::SaveMetadata metadata;
    metadata.game_version = "0.1.0";
    metadata.enabled_mods = std::move(saved_mods);
    assert(metadata.validate());

    metadata.enabled_mods.front().prototype_hash.clear();
    assert(!metadata.validate());

    heartstead::modding::ModManifest tweaks;
    tweaks.id = "tweaks";
    tweaks.name = "Tweaks";
    tweaks.version = "0.0.1";

    heartstead::modding::GenericPrototypePatch patch;
    patch.source_mod_id = "tweaks";
    patch.target_id = clay_id.value();
    patch.set_fields.emplace("stack_limit", "32");

    auto patched_fingerprints = heartstead::modding::build_mod_prototype_fingerprints(
        {base, tweaks}, std::vector<heartstead::modding::GenericPrototype>{clay}, {patch});
    const auto tweaks_fingerprint = std::ranges::find(
        patched_fingerprints, "tweaks", &heartstead::modding::ModPrototypeFingerprint::id);
    assert(tweaks_fingerprint != patched_fingerprints.end());
    assert(tweaks_fingerprint->prototype_count == 0);
    assert(tweaks_fingerprint->patch_count == 1);

    auto final_stage_patch = patch;
    final_stage_patch.stage = heartstead::modding::GenericPrototypePatchStage::final_fix;
    auto final_stage_fingerprints = heartstead::modding::build_mod_prototype_fingerprints(
        {base, tweaks}, std::vector<heartstead::modding::GenericPrototype>{clay},
        {final_stage_patch});
    const auto final_stage_tweaks_fingerprint = std::ranges::find(
        final_stage_fingerprints, "tweaks", &heartstead::modding::ModPrototypeFingerprint::id);
    assert(final_stage_tweaks_fingerprint != final_stage_fingerprints.end());
    assert(final_stage_tweaks_fingerprint->prototype_hash != tweaks_fingerprint->prototype_hash);

    auto staged_fingerprints = heartstead::modding::build_mod_prototype_fingerprints(
        {base, tweaks}, std::vector<heartstead::modding::GenericPrototype>{clay},
        {final_stage_patch, patch});
    auto staged_fingerprints_reordered = heartstead::modding::build_mod_prototype_fingerprints(
        {base, tweaks}, std::vector<heartstead::modding::GenericPrototype>{clay},
        {patch, final_stage_patch});
    const auto staged_tweaks_fingerprint = std::ranges::find(
        staged_fingerprints, "tweaks", &heartstead::modding::ModPrototypeFingerprint::id);
    const auto reordered_tweaks_fingerprint = std::ranges::find(
        staged_fingerprints_reordered, "tweaks", &heartstead::modding::ModPrototypeFingerprint::id);
    assert(staged_tweaks_fingerprint != staged_fingerprints.end());
    assert(reordered_tweaks_fingerprint != staged_fingerprints_reordered.end());
    assert(staged_tweaks_fingerprint->patch_count == 2);
    assert(staged_tweaks_fingerprint->prototype_hash ==
           reordered_tweaks_fingerprint->prototype_hash);

    heartstead::scripting::ScriptModuleDesc server_script;
    server_script.module_id = "base:scripts/runtime_server/neutral";
    server_script.source_mod_id = "base";
    server_script.source_path = "scripts/runtime_server/neutral.luau";
    server_script.source = "return { ping = function() return true end }\n";
    server_script.stage = heartstead::scripting::ScriptStage::runtime_server;
    server_script.permissions = {
        heartstead::scripting::ScriptPermission::read_prototypes,
        heartstead::scripting::ScriptPermission::emit_commands,
    };
    auto client_script = server_script;
    client_script.module_id = "base:scripts/runtime_client/neutral";
    client_script.source_path = "scripts/runtime_client/neutral.luau";
    client_script.stage = heartstead::scripting::ScriptStage::runtime_client;

    auto scripted = heartstead::modding::build_mod_prototype_fingerprints(
        {base}, std::vector<heartstead::modding::GenericPrototype>{clay}, {},
        {server_script, client_script});
    auto scripts_reordered = heartstead::modding::build_mod_prototype_fingerprints(
        {base}, std::vector<heartstead::modding::GenericPrototype>{clay}, {},
        {client_script, server_script});
    assert(scripted.front().script_count == 2);
    assert(scripted.front().prototype_hash == scripts_reordered.front().prototype_hash);

    auto permission_order_changed = server_script;
    std::ranges::reverse(permission_order_changed.permissions);
    auto permissions_reordered = heartstead::modding::build_mod_prototype_fingerprints(
        {base}, std::vector<heartstead::modding::GenericPrototype>{clay}, {},
        {permission_order_changed, client_script});
    assert(scripted.front().prototype_hash == permissions_reordered.front().prototype_hash);

    auto changed_script = server_script;
    changed_script.source = "return { ping = function() return false end }\n";
    auto script_source_changed = heartstead::modding::build_mod_prototype_fingerprints(
        {base}, std::vector<heartstead::modding::GenericPrototype>{clay}, {},
        {changed_script, client_script});
    assert(scripted.front().prototype_hash != script_source_changed.front().prototype_hash);
}

void test_mod_validation_applies_prototype_patches() {
    const auto root = make_temp_root();
    const auto mods_root = root / "mods";

    write_text(mods_root / "base/mod.toml", "id = \"base\"\n"
                                            "name = \"Heartstead Base\"\n"
                                            "version = \"0.0.1\"\n");
    write_text(mods_root / "base/data/items/raw_clay.prototype.toml",
               "kind = \"item\"\n"
               "id = \"base:items/raw_clay\"\n"
               "display_name = \"Raw Clay\"\n"
               "stack_limit = \"64\"\n");
    write_text(mods_root / "tweaks/mod.toml", "id = \"tweaks\"\n"
                                              "name = \"Tweaks\"\n"
                                              "version = \"0.0.1\"\n"
                                              "dependencies = \"base\"\n");
    write_text(mods_root / "tweaks/data/items/raw_clay.prototype_patch.toml",
               "target = \"base:items/raw_clay\"\n"
               "set.stack_limit = \"16\"\n"
               "set.tags = \"patched\"\n");
    write_text(mods_root / "tweaks/data/items/raw_clay.final_patch.toml",
               "target = \"base:items/raw_clay\"\n"
               "set.display_name = \"Final Packed Clay\"\n");

    auto report = heartstead::modding::ModValidation::validate(mods_root);
    assert(!report.has_errors());
    assert(report.mods.size() == 2);
    assert(report.prototypes.size() == 1);
    assert(report.prototype_patches.size() == 2);
    assert(report.applied_patch_count == 2);
    assert(report.mod_fingerprints.size() == 2);

    const auto clay_id = heartstead::core::PrototypeId::parse("base:items/raw_clay");
    assert(clay_id);
    const auto* clay = report.registry.find(clay_id.value());
    assert(clay != nullptr);
    assert(clay->display_name == "Final Packed Clay");
    assert(clay->fields.at("stack_limit") == "16");
    assert(clay->fields.at("tags") == "patched");

    const auto tweaks_fingerprint = std::ranges::find(
        report.mod_fingerprints, "tweaks", &heartstead::modding::ModPrototypeFingerprint::id);
    assert(tweaks_fingerprint != report.mod_fingerprints.end());
    assert(tweaks_fingerprint->prototype_count == 0);
    assert(tweaks_fingerprint->patch_count == 2);

    write_text(mods_root / "tweaks/data/items/raw_clay.prototype_patch.toml",
               "target = \"base:items/raw_clay\"\n"
               "set.stack_limit = \"0\"\n");
    report = heartstead::modding::ModValidation::validate(mods_root);
    assert(report.has_errors());
    assert(std::ranges::any_of(report.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "prototype_semantic.invalid_number";
    }));

    write_text(mods_root / "tweaks/data/items/raw_clay.prototype_patch.toml",
               "target = \"base:items/raw_clay\"\n"
               "set.id = \"base:items/forged\"\n"
               "set.stack_limit = \"16\"\n");
    report = heartstead::modding::ModValidation::validate(mods_root);
    assert(report.has_errors());
    assert(std::ranges::any_of(report.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "prototype_patch.immutable_field";
    }));
    assert(report.applied_patch_count == 1);
    const auto* unmodified_clay = report.registry.find(clay_id.value());
    assert(unmodified_clay != nullptr);
    assert(unmodified_clay->fields.at("stack_limit") == "64");

    write_text(mods_root / "tweaks/data/items/raw_clay.prototype_patch.toml",
               "target = \"base:items/missing\"\n"
               "set.stack_limit = \"16\"\n");
    report = heartstead::modding::ModValidation::validate(mods_root);
    assert(report.has_errors());
    assert(std::ranges::any_of(report.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "prototype_patch.missing_target";
    }));

    write_text(mods_root / "tweaks/mod.toml", "id = \"tweaks\"\n"
                                              "name = \"Tweaks\"\n"
                                              "version = \"0.0.1\"\n");
    write_text(mods_root / "tweaks/data/items/raw_clay.prototype_patch.toml",
               "target = \"base:items/raw_clay\"\n"
               "set.stack_limit = \"16\"\n");
    report = heartstead::modding::ModValidation::validate(mods_root);
    assert(report.has_errors());
    assert(std::ranges::any_of(report.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "prototype_patch.missing_dependency";
    }));
}

void test_mod_validation_report() {
    const auto root = make_temp_root();
    const auto mods_root = root / "mods";
    const auto resource_packs_root = root / "resource_packs";

    write_text(mods_root / "base/mod.toml", "id = \"base\"\n"
                                            "name = \"Heartstead Base\"\n"
                                            "version = \"0.0.1\"\n");
    write_text(mods_root / "base/data/items/raw_clay.prototype.toml",
               "kind = \"item\"\n"
               "id = \"base:items/raw_clay\"\n"
               "display_name = \"Raw Clay\"\n"
               "stack_limit = \"64\"\n"
               "tags = \"clay,crafting\"\n");
    write_text(mods_root / "base/data/cargo/heavy_log.prototype.toml",
               "kind = \"cargo\"\n"
               "id = \"base:cargo/heavy_log\"\n"
               "display_name = \"Heavy Log\"\n"
               "mass_grams = \"90000\"\n"
               "volume_milliliters = \"180000\"\n"
               "transport_modes = \"cart,wagon,animal\"\n"
               "hazard_tags = \"crush\"\n");
    write_text(mods_root / "base/data/entities/hand_cart.prototype.toml",
               "kind = \"entity\"\n"
               "id = \"base:entities/hand_cart\"\n"
               "display_name = \"Hand Cart\"\n"
               "entity_kind = \"cart\"\n"
               "persistent = \"true\"\n");
    write_text(mods_root / "base/data/processes/drying.prototype.toml",
               "kind = \"process\"\n"
               "id = \"base:processes/drying\"\n"
               "display_name = \"Drying\"\n"
               "default_required_work_ms = \"60000\"\n"
               "requires_room = \"true\"\n"
               "requires_power = \"true\"\n"
               "required_power_capacity = \"2\"\n"
               "base_quality_rate_per_mille = \"1100\"\n"
               "tags = \"ambient,timestamped\"\n");
    write_text(mods_root / "base/data/room_descriptors/warm.prototype.toml",
               "kind = \"room_descriptor\"\n"
               "id = \"base:room_descriptors/warm\"\n"
               "display_name = \"Warm\"\n"
               "code = \"warm\"\n"
               "label = \"Warm\"\n"
               "severity = \"positive\"\n"
               "tags = \"comfort,temperature\"\n");
    write_text(mods_root / "base/data/voxels/clay.prototype.toml",
               "kind = \"voxel\"\n"
               "id = \"base:voxels/clay\"\n"
               "display_name = \"Clay\"\n"
               "terrain_material = \"clay\"\n"
               "mining_tool = \"shovel\"\n"
               "tags = \"soil,crafting_source\"\n");
    write_text(mods_root / "base/data/build_pieces/kiln_firebox.prototype.toml",
               "kind = \"build_piece\"\n"
               "id = \"base:build_pieces/kiln_firebox\"\n"
               "display_name = \"Kiln Firebox\"\n"
               "material_tags = \"clay,heat_resistant\"\n"
               "network_ports = \"fuel_input\"\n");
    write_text(mods_root / "base/data/build_pieces/chimney.prototype.toml",
               "kind = \"build_piece\"\n"
               "id = \"base:build_pieces/chimney\"\n"
               "display_name = \"Chimney\"\n"
               "material_tags = \"clay\"\n"
               "network_ports = \"smoke_output\"\n");
    write_text(mods_root / "base/data/assemblies/clay_kiln.prototype.toml",
               "kind = \"assembly\"\n"
               "id = \"base:assemblies/clay_kiln\"\n"
               "display_name = \"Clay Kiln\"\n"
               "required_parts = \"firebox:base:build_pieces/kiln_firebox,"
               "chimney:base:build_pieces/chimney\"\n"
               "required_ports = \"fuel_input,smoke_output\"\n");
    write_text(mods_root / "base/data/workpieces/clay_lump.prototype.toml",
               "kind = \"workpiece\"\n"
               "id = \"base:workpieces/clay_lump\"\n"
               "display_name = \"Clay Lump\"\n"
               "grid = \"8x8x8\"\n"
               "material = \"base:materials/clay\"\n");
    write_text(mods_root / "base/data/scenarios/homestead.prototype.toml",
               "kind = \"scenario\"\n"
               "id = \"base:scenarios/homestead\"\n"
               "display_name = \"Homestead\"\n"
               "start_region = \"temperate_valley\"\n"
               "spawn_mode = \"homestead\"\n"
               "starting_items = \"base:items/raw_clay\"\n"
               "starting_cargo = \"base:cargo/heavy_log\"\n"
               "tags = \"co_op\"\n");
    write_text(mods_root / "base/data/materials/clay.prototype.toml",
               "kind = \"material\"\n"
               "id = \"base:materials/clay\"\n"
               "display_name = \"Clay Material\"\n"
               "domain = \"terrain\"\n"
               "blend_mode = \"opaque\"\n"
               "shader_template = \"base:shaders/templates/terrain.slang\"\n"
               "texture.albedo = \"base:textures/voxels/clay.txt\"\n"
               "scalar.roughness = \"0.85\"\n"
               "color.tint = \"0.55,0.38,0.26,1.0\"\n");
    write_text(mods_root / "base/assets/shaders/templates/terrain.slang", "shader");
    write_text(mods_root / "base/assets/textures/voxels/clay.txt", "base clay texture");
    write_text(resource_packs_root / "hd_pack/resource_pack.toml", "id = \"hd_pack\"\n"
                                                                   "name = \"HD Pack\"\n"
                                                                   "version = \"1.0.0\"\n");
    write_text(resource_packs_root / "hd_pack/assets/textures/voxels/clay.txt",
               "resource pack clay texture");

    auto report = heartstead::modding::ModValidation::validate(mods_root);
    assert(!report.has_errors());
    assert(report.mods.size() == 1);
    assert(report.prototypes.size() == 12);
    assert(report.registry.size() == 12);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::item) == 1);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::cargo) == 1);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::entity) == 1);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::process) == 1);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::room_descriptor) == 1);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::voxel) == 1);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::build_piece) == 2);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::assembly) == 1);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::workpiece) == 1);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::material) == 1);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::scenario) == 1);
    assert(report.mod_fingerprints.size() == 1);
    assert(report.mod_fingerprints.front().id == "base");
    assert(report.mod_fingerprints.front().version == "0.0.1");
    assert(report.mod_fingerprints.front().prototype_count == 12);
    assert(!report.mod_fingerprints.front().prototype_hash.empty());
    assert(report.count_severity(heartstead::modding::DiagnosticSeverity::error) == 0);
    assert(heartstead::modding::diagnostic_severity_name(
               heartstead::modding::DiagnosticSeverity::warning) == "warning");

    auto content_report = heartstead::content::ContentValidation::validate(root);
    assert(!content_report.has_errors());
    assert(content_report.mods.size() == 1);
    assert(content_report.resource_packs.size() == 1);
    assert(content_report.resource_pack_load_plan.size() == 1);
    assert(content_report.resource_pack_load_plan.entries.front().manifest.id == "hd_pack");
    assert(content_report.resource_pack_load_plan.entries.front().asset_priority ==
           heartstead::assets::default_resource_pack_priority_base);
    assert(content_report.prototypes.size() == 12);
    assert(content_report.mod_fingerprints.size() == 1);
    assert(content_report.mod_fingerprints.front().prototype_hash ==
           report.mod_fingerprints.front().prototype_hash);
    auto content_inspection = heartstead::debug::Inspector::inspect(content_report);
    assert(content_inspection.object_type == "content_validation_report");
    assert(content_inspection.state == "ready");
    assert(content_inspection.find_field("mod_count")->value == "1");
    assert(content_inspection.find_field("resource_pack_count")->value == "1");
    assert(content_inspection.find_field("resource_pack_plan_count")->value == "1");
    assert(content_inspection.find_field("prototype_count")->value == "12");
    assert(content_inspection.find_field("active_asset_count")->value == "2");
    assert(content_inspection.find_field("item_prototype_count")->value == "1");
    assert(content_inspection.find_field("room_descriptor_prototype_count")->value == "1");
    assert(content_inspection.find_field("material_asset_override_count")->value == "1");
    assert(content_inspection.find_field("first_resource_pack_id")->value == "hd_pack");
    assert(content_inspection.find_field("first_resource_pack_priority")->value == "1000");
    assert(content_inspection.find_field("warning_count")->value == "0");
    assert(content_inspection.find_field("error_count")->value == "0");
    assert(content_inspection.issues.empty());

    auto new_save_metadata =
        heartstead::content::save_metadata_from_content_report(content_report, "0.1.0", 424242);
    assert(new_save_metadata);
    assert(new_save_metadata.value().schema_version ==
           heartstead::save::current_save_schema_version);
    assert(new_save_metadata.value().game_version == "0.1.0");
    assert(new_save_metadata.value().world_seed == 424242);
    assert(new_save_metadata.value().enabled_mods.size() == 1);
    assert(new_save_metadata.value().enabled_mods.front().id == "base");
    assert(new_save_metadata.value().enabled_mods.front().version == "0.0.1");
    assert(new_save_metadata.value().enabled_mods.front().prototype_hash ==
           content_report.mod_fingerprints.front().prototype_hash);
    assert(new_save_metadata.value().migration_history.empty());

    auto missing_game_version_metadata =
        heartstead::content::save_metadata_from_content_report(content_report, "", 1);
    assert(!missing_game_version_metadata);
    assert(missing_game_version_metadata.error().code == "save.missing_game_version");

    heartstead::content::ContentValidationReport invalid_content_report;
    invalid_content_report.mod_fingerprints = content_report.mod_fingerprints;
    invalid_content_report.diagnostics.push_back({heartstead::modding::DiagnosticSeverity::error,
                                                  root, "test.invalid_content",
                                                  "test invalid content"});
    auto invalid_metadata =
        heartstead::content::save_metadata_from_content_report(invalid_content_report, "0.1.0", 1);
    assert(!invalid_metadata);
    assert(invalid_metadata.error().code == "content.invalid_for_save_metadata");
    auto invalid_content_inspection = heartstead::debug::Inspector::inspect(invalid_content_report);
    assert(invalid_content_inspection.object_type == "content_validation_report");
    assert(invalid_content_inspection.state == "invalid");
    assert(invalid_content_inspection.has_errors());
    assert(invalid_content_inspection.find_field("error_count")->value == "1");
    assert(std::ranges::any_of(invalid_content_inspection.issues, [](const auto& issue) {
        return issue.code == "test.invalid_content";
    }));

    heartstead::content::ContentValidationReport missing_fingerprint_report;
    auto missing_fingerprint_metadata = heartstead::content::save_metadata_from_content_report(
        missing_fingerprint_report, "0.1.0", 1);
    assert(!missing_fingerprint_metadata);
    assert(missing_fingerprint_metadata.error().code == "content.missing_mod_fingerprints");

    assert(content_report.asset_catalog.record_count() == 3);
    assert(content_report.asset_catalog.active_count() == 2);
    assert(content_report.item_definitions.size() == 1);
    assert(content_report.item_definitions.front().stack_limit == 64);
    assert(content_report.item_definitions.front().tags.size() == 2);
    assert(content_report.cargo_definitions.size() == 1);
    assert(content_report.cargo_definitions.front().mass_grams == 90000);
    assert(content_report.cargo_definitions.front().allowed_transport_modes.allows(
        heartstead::cargo::CargoTransportMode::wagon));
    assert(content_report.entity_definitions.size() == 1);
    assert(content_report.entity_definitions.front().kind ==
           heartstead::entities::EntityKind::cart);
    assert(content_report.entity_definitions.front().persistent);
    assert(content_report.voxel_palette.size() == 1);
    const auto clay_voxel_id = heartstead::core::PrototypeId::parse("base:voxels/clay");
    assert(clay_voxel_id);
    assert(content_report.voxel_palette.type_for(clay_voxel_id.value()).value() == 1);
    assert(content_report.material_registry.size() == 1);
    assert(content_report.assembly_definitions.size() == 1);
    assert(content_report.assembly_definitions.front().part_requirements.size() == 2);
    assert(content_report.assembly_definitions.front().required_ports.size() == 2);
    assert(content_report.process_definitions.size() == 1);
    assert(content_report.process_definitions.front().default_required_work_ms == 60000);
    assert(content_report.process_definitions.front().requires_room);
    assert(content_report.process_definitions.front().requires_power);
    assert(content_report.process_definitions.front().required_power_capacity == 2);
    assert(content_report.process_definitions.front().base_quality_rate_per_mille == 1100);
    assert(content_report.process_definitions.front().tags.size() == 2);
    assert(content_report.room_descriptor_definitions.size() == 1);
    assert(content_report.room_descriptor_definitions.front().code == "warm");
    assert(content_report.room_descriptor_definitions.front().label == "Warm");
    assert(content_report.room_descriptor_definitions.front().severity ==
           heartstead::rooms::RoomDescriptorSeverity::positive);
    assert(content_report.room_descriptor_definitions.front().tags.size() == 2);
    assert(content_report.workpiece_definitions.size() == 1);
    assert(content_report.workpiece_definitions.front().grid_shape.width == 8);
    assert(content_report.workpiece_definitions.front().material_prototype_id.value() ==
           "base:materials/clay");
    assert(content_report.scenario_definitions.size() == 1);
    assert(content_report.scenario_definitions.front().start_region == "temperate_valley");
    assert(heartstead::scenarios::scenario_spawn_mode_name(
               content_report.scenario_definitions.front().spawn_mode) == "homestead");
    assert(content_report.scenario_definitions.front().starting_items.size() == 1);
    assert(content_report.scenario_definitions.front().starting_cargo.size() == 1);
    assert(content_report.material_assets.references.size() == 2);
    assert(content_report.material_assets.override_count() == 1);
    assert(content_report.count_kind(heartstead::modding::PrototypeKinds::material) == 1);
    assert(content_report.count_kind(heartstead::modding::PrototypeKinds::room_descriptor) == 1);
    assert(content_report.count_kind(heartstead::modding::PrototypeKinds::scenario) == 1);
    assert(content_report.count_severity(heartstead::modding::DiagnosticSeverity::error) == 0);

    write_text(mods_root / "base/data/room_descriptors/warm.prototype.toml",
               "kind = \"room_descriptor\"\n"
               "id = \"base:room_descriptors/warm\"\n"
               "display_name = \"Warm\"\n"
               "code = \"warm\"\n"
               "label = \"Warm\"\n"
               "severity = \"severe\"\n");
    auto invalid_room_descriptor_report = heartstead::modding::ModValidation::validate(mods_root);
    assert(invalid_room_descriptor_report.has_errors());
    assert(
        std::ranges::any_of(invalid_room_descriptor_report.diagnostics, [](const auto& diagnostic) {
            return diagnostic.code == "prototype_semantic.invalid_enum";
        }));
    write_text(mods_root / "base/data/room_descriptors/warm.prototype.toml",
               "kind = \"room_descriptor\"\n"
               "id = \"base:room_descriptors/warm\"\n"
               "display_name = \"Warm\"\n"
               "code = \"warm\"\n"
               "label = \"Warm\"\n"
               "severity = \"positive\"\n"
               "tags = \"comfort,temperature\"\n");

    write_text(mods_root / "base/data/build_pieces/chimney.prototype.toml",
               "kind = \"build_piece\"\n"
               "id = \"base:build_pieces/chimney\"\n"
               "display_name = \"Chimney\"\n"
               "material_tags = \"clay,clay\"\n"
               "network_ports = \"smoke_output\"\n");
    auto duplicate_token_report = heartstead::modding::ModValidation::validate(mods_root);
    assert(duplicate_token_report.has_errors());
    assert(std::ranges::any_of(duplicate_token_report.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "prototype_semantic.duplicate_token";
    }));

    const auto invalid_root = make_temp_root();
    const auto invalid_mods_root = invalid_root / "mods";
    write_text(invalid_mods_root / "base/mod.toml", "id = \"base\"\n"
                                                    "name = \"Heartstead Base\"\n"
                                                    "version = \"0.0.1\"\n");
    write_text(invalid_mods_root / "base/data/items/strange.prototype.toml",
               "kind = \"strange_kind\"\n"
               "id = \"base:items/strange\"\n"
               "display_name = \"Strange\"\n");
    write_text(invalid_mods_root / "base/data/items/bad_stack.prototype.toml",
               "kind = \"item\"\n"
               "id = \"base:items/bad_stack\"\n"
               "display_name = \"Bad Stack\"\n"
               "stack_limit = \"0\"\n");
    write_text(invalid_mods_root / "base/data/materials/bad_material.prototype.toml",
               "kind = \"material\"\n"
               "id = \"base:materials/bad_material\"\n"
               "display_name = \"Bad Material\"\n"
               "domain = \"terrain\"\n"
               "blend_mode = \"opaque\"\n"
               "shader_template = \"not_a_virtual_path\"\n");
    write_text(invalid_mods_root / "base/data/scenarios/bad_start.prototype.toml",
               "kind = \"scenario\"\n"
               "id = \"base:scenarios/bad_start\"\n"
               "display_name = \"Bad Start\"\n"
               "spawn_mode = \"somewhere\"\n"
               "starting_items = \"base:items/missing\"\n");

    report = heartstead::modding::ModValidation::validate(invalid_mods_root);
    assert(report.has_errors());
    assert(report.count_kind("strange_kind") == 1);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::item) == 1);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::material) == 1);
    assert(report.count_kind(heartstead::modding::PrototypeKinds::scenario) == 1);
    assert(report.count_severity(heartstead::modding::DiagnosticSeverity::error) == 6);
    assert(report.registry.size() == 3);

    const auto missing_asset_root = make_temp_root();
    const auto missing_asset_mods_root = missing_asset_root / "mods";
    write_text(missing_asset_mods_root / "base/mod.toml", "id = \"base\"\n"
                                                          "name = \"Heartstead Base\"\n"
                                                          "version = \"0.0.1\"\n");
    write_text(missing_asset_mods_root / "base/data/materials/missing_texture.prototype.toml",
               "kind = \"material\"\n"
               "id = \"base:materials/missing_texture\"\n"
               "display_name = \"Missing Texture Material\"\n"
               "domain = \"terrain\"\n"
               "blend_mode = \"opaque\"\n"
               "shader_template = \"base:shaders/templates/terrain.slang\"\n"
               "texture.albedo = \"base:textures/voxels/missing.txt\"\n");
    write_text(missing_asset_mods_root / "base/assets/shaders/templates/terrain.slang", "shader");
    std::filesystem::create_directories(missing_asset_root / "resource_packs");
    auto missing_asset_report =
        heartstead::content::ContentValidation::validate(missing_asset_root);
    assert(missing_asset_report.has_errors());
    assert(missing_asset_report.material_registry.size() == 1);
    assert(missing_asset_report.material_assets.references.size() == 1);
    assert(missing_asset_report.count_severity(heartstead::modding::DiagnosticSeverity::error) ==
           1);
    assert(std::ranges::any_of(missing_asset_report.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "material_assets.missing_texture";
    }));
}

void test_prototype_registry() {
    const auto root = make_temp_root();
    const auto mods_root = root / "mods";

    write_text(mods_root / "base/mod.toml", "id = \"base\"\n"
                                            "name = \"Heartstead Base\"\n"
                                            "version = \"0.0.1\"\n");
    write_text(mods_root / "base/data/items/raw_clay.prototype.toml",
               "kind = \"item\"\n"
               "id = \"base:items/raw_clay\"\n"
               "display_name = \"Raw Clay\"\n");
    write_text(mods_root / "base/data/build_pieces/wall_frame.prototype.toml",
               "kind = \"build_piece\"\n"
               "id = \"base:build_pieces/wall_frame\"\n"
               "display_name = \"Wall Frame\"\n");

    heartstead::modding::ModDiscoverer discoverer;
    auto discovery = discoverer.discover(mods_root);
    assert(!discovery.has_errors());

    heartstead::modding::GenericPrototypeLoader loader;
    auto loaded = loader.load_from_mods(discovery.mods);
    assert(!loaded.has_errors());

    heartstead::modding::PrototypeRegistry registry;
    auto build_result = registry.build(std::move(loaded.prototypes));
    assert(!build_result.has_errors());
    assert(registry.size() == 2);
    assert(registry.count_kind(heartstead::modding::PrototypeKinds::item) == 1);

    const auto clay = heartstead::core::PrototypeId::parse("base:items/raw_clay");
    const auto wall = heartstead::core::PrototypeId::parse("base:build_pieces/wall_frame");
    assert(clay);
    assert(wall);
    assert(registry.require_kind(clay.value(), heartstead::modding::PrototypeKinds::item));
    assert(registry.require_kind(wall.value(), heartstead::modding::PrototypeKinds::build_piece));
    assert(!registry.require_kind(clay.value(), heartstead::modding::PrototypeKinds::cargo));
}

void test_voxel_palette() {
    heartstead::modding::GenericPrototype raw_clay;
    raw_clay.kind = std::string(heartstead::modding::PrototypeKinds::item);
    raw_clay.id = heartstead::core::PrototypeId::parse("base:items/raw_clay").value();
    raw_clay.display_name = "Raw Clay";
    raw_clay.fields = {
        {"kind", "item"},
        {"id", "base:items/raw_clay"},
        {"display_name", "Raw Clay"},
        {"stack_limit", "64"},
    };

    heartstead::modding::GenericPrototype clay;
    clay.kind = std::string(heartstead::modding::PrototypeKinds::voxel);
    clay.id = heartstead::core::PrototypeId::parse("base:voxels/clay").value();
    clay.display_name = "Clay";
    clay.fields = {
        {"kind", "voxel"},
        {"id", "base:voxels/clay"},
        {"display_name", "Clay"},
        {"terrain_material", "clay"},
        {"mining_tool", "shovel"},
        {"tags", "soil,crafting_source"},
        {"interaction.break_resource_item", "base:items/raw_clay"},
    };

    heartstead::modding::GenericPrototype stone = clay;
    stone.id = heartstead::core::PrototypeId::parse("base:voxels/stone").value();
    stone.display_name = "Stone";
    stone.fields["id"] = "base:voxels/stone";
    stone.fields["display_name"] = "Stone";
    stone.fields["terrain_material"] = "stone";
    stone.fields["mining_tool"] = "pick";

    heartstead::modding::PrototypeRegistry registry;
    auto build = registry.build({stone, clay, raw_clay});
    assert(!build.has_errors());

    auto palette = heartstead::world::voxel_palette_from_prototypes(registry);
    assert(palette);
    assert(palette.value().size() == 2);

    const auto clay_id = heartstead::core::PrototypeId::parse("base:voxels/clay").value();
    const auto stone_id = heartstead::core::PrototypeId::parse("base:voxels/stone").value();
    assert(palette.value().type_for(clay_id).value() == 1);
    assert(palette.value().type_for(stone_id).value() == 2);
    assert(palette.value().find_by_type(1)->prototype_id == clay_id);
    assert(palette.value().find_by_type(1)->interaction.break_resource_item == raw_clay.id);
    assert(palette.value().find_by_prototype(stone_id)->terrain_material == "stone");
    auto clay_cell = palette.value().cell_for(clay_id, 3);
    assert(clay_cell);
    assert(clay_cell.value().type == 1);
    assert(clay_cell.value().light == 3);

    auto missing = heartstead::core::PrototypeId::parse("base:voxels/missing").value();
    assert(!palette.value().cell_for(missing));

    heartstead::world::VoxelDefinition reserved;
    reserved.type = heartstead::world::VoxelPalette::air_type;
    reserved.prototype_id = clay_id;
    reserved.display_name = "Air Collision";
    reserved.terrain_material = "air";
    reserved.mining_tool = "none";
    assert(!reserved.validate());

    heartstead::world::VoxelPalette manual;
    auto clay_definition = heartstead::world::voxel_definition_from_prototype(clay, 1);
    assert(clay_definition);
    assert(manual.add(clay_definition.value()));
    assert(!manual.add(clay_definition.value()));
}

void test_world_voxel_chunk() {
    heartstead::world::VoxelChunk chunk({2, 0, -1});
    const auto initial_revision = chunk.content_revision();
    assert(initial_revision == 1);
    assert(!chunk.identity().is_valid());
    auto initial = chunk.get({0, 0, 0});
    assert(initial);
    assert(initial.value().is_air());

    auto set_status = chunk.set({3, 4, 5}, heartstead::world::VoxelCell{42, 9});
    assert(set_status);
    assert(chunk.content_revision() == initial_revision + 1);
    assert(chunk.set({3, 4, 5}, heartstead::world::VoxelCell{42, 9}));
    assert(chunk.content_revision() == initial_revision + 1);

    auto edited = chunk.get({3, 4, 5});
    assert(edited);
    assert(edited.value().type == 42);
    assert(chunk.dirty().contains(heartstead::world::ChunkDirtyFlag::mesh));
    assert(chunk.dirty().contains(heartstead::world::ChunkDirtyFlag::save));

    chunk.clear_all_dirty();
    assert(!chunk.dirty().contains(heartstead::world::ChunkDirtyFlag::mesh));

    auto loaded_status = chunk.apply_saved_cell({4, 5, 6}, heartstead::world::VoxelCell{43, 10});
    assert(loaded_status);
    assert(chunk.content_revision() == initial_revision + 2);
    auto loaded_cell = chunk.get({4, 5, 6});
    assert(loaded_cell);
    assert(loaded_cell.value().type == 43);
    assert(chunk.dirty().contains(heartstead::world::ChunkDirtyFlag::mesh));
    assert(!chunk.dirty().contains(heartstead::world::ChunkDirtyFlag::save));
    assert(!chunk.dirty().contains(heartstead::world::ChunkDirtyFlag::replication));

    chunk.fill(heartstead::world::VoxelCell{6, 2});
    assert(chunk.content_revision() == initial_revision + 3);
    chunk.fill(heartstead::world::VoxelCell{6, 2});
    assert(chunk.content_revision() == initial_revision + 3);

    std::vector<heartstead::world::VoxelCell> generated(heartstead::world::VoxelChunk::total_cells,
                                                        heartstead::world::VoxelCell::air());
    assert(chunk.load_generated_cells(std::move(generated)));
    assert(chunk.content_revision() == initial_revision + 4);

    auto outside = chunk.get({heartstead::world::VoxelChunk::edge_length, 0, 0});
    assert(!outside);

    constexpr std::int64_t far_coord = 1LL << 40;
    heartstead::world::VoxelChunk far_chunk({far_coord, -far_coord, far_coord + 7});
    assert(far_chunk.coord().x == far_coord);
    assert(far_chunk.coord().y == -far_coord);
    assert(far_chunk.coord().z == far_coord + 7);
}

void test_chunk_database() {
    heartstead::world::ChunkDatabase database;
    auto& origin = database.get_or_create({0, 0, 0});
    auto& neighbor = database.get_or_create({1, 0, 0});
    const heartstead::world::ChunkCoord expected_origin{0, 0, 0};
    const heartstead::world::ChunkCoord expected_neighbor{1, 0, 0};
    assert(origin.coord() == expected_origin);
    assert(neighbor.coord() == expected_neighbor);
    assert(database.chunk_count() == 2);
    assert(origin.identity().is_valid());
    assert(neighbor.identity().is_valid());
    assert(origin.identity() != neighbor.identity());
    const auto old_neighbor_identity = neighbor.identity();

    database.clear_all_dirty();
    assert(database.stats().dirty_mesh_count == 0);

    auto set_status = database.set({0, 0, 0}, {3, 4, 5}, heartstead::world::VoxelCell{7, 1});
    assert(set_status);
    assert(database.edit_log().size() == 1);
    assert(database.edit_log().front().previous.is_air());
    assert(database.edit_log().front().next.type == 7);

    auto edited = database.get({0, 0, 0}, {3, 4, 5});
    assert(edited);
    assert(edited.value().type == 7);
    assert(database.stats().dirty_mesh_count == 1);
    assert(database.erase({1, 0, 0}));
    assert(!database.contains({1, 0, 0}));
    assert(!database.erase({1, 0, 0}));

    database.clear_all_dirty();
    heartstead::dirty::DirtyRegionTracker dirty_regions;
    auto& neighbor_after_erase = database.get_or_create({1, 0, 0});
    assert(neighbor_after_erase.coord() == expected_neighbor);
    assert(neighbor_after_erase.identity().coordinate == old_neighbor_identity.coordinate);
    assert(neighbor_after_erase.identity().load_generation > old_neighbor_identity.load_generation);
    const auto identities = database.identities();
    assert(identities.size() == 2);
    auto boundary_edit =
        database.set({0, 0, 0}, {heartstead::world::VoxelChunk::edge_length - 1, 4, 5},
                     heartstead::world::VoxelCell{8, 0}, dirty_regions);
    assert(boundary_edit);
    const auto* neighbor_chunk = database.find({1, 0, 0});
    assert(neighbor_chunk != nullptr);
    assert(neighbor_chunk->dirty().contains(heartstead::world::ChunkDirtyFlag::mesh));
    assert(!neighbor_chunk->dirty().contains(heartstead::world::ChunkDirtyFlag::save));
    assert(database.stats().dirty_mesh_count == 2);
    assert(database.stats().dirty_save_count == 1);
    assert(dirty_regions.count(heartstead::dirty::DirtyRegionKind::chunk_mesh) == 1);
    assert(dirty_regions.count(heartstead::dirty::DirtyRegionKind::chunk_collision) == 1);
    assert(dirty_regions.count(heartstead::dirty::DirtyRegionKind::chunk_lighting) == 1);
    auto mesh_regions = dirty_regions.consume_kind(heartstead::dirty::DirtyRegionKind::chunk_mesh);
    assert(mesh_regions.size() == 1);
    assert(mesh_regions.front().bounds.min.x == 0);
    assert(mesh_regions.front().bounds.max.x == 1);

    auto missing = database.get({99, 0, 0}, {0, 0, 0});
    assert(!missing);

    database.clear_edit_log();
    assert(database.edit_log().empty());

    constexpr std::int64_t far_coord = 1LL << 40;
    heartstead::world::ChunkDatabase far_database;
    auto& far_origin = far_database.get_or_create({far_coord, -far_coord, far_coord});
    auto& far_neighbor = far_database.get_or_create({far_coord + 1, -far_coord, far_coord});
    assert(far_origin.coord().x == far_coord);
    assert(far_neighbor.coord().x == far_coord + 1);
    far_database.clear_all_dirty();

    heartstead::dirty::DirtyRegionTracker far_dirty_regions;
    auto far_boundary_edit = far_database.set(
        {far_coord, -far_coord, far_coord}, {heartstead::world::VoxelChunk::edge_length - 1, 0, 0},
        heartstead::world::VoxelCell{9, 0}, far_dirty_regions);
    assert(far_boundary_edit);
    const auto* far_neighbor_chunk = far_database.find({far_coord + 1, -far_coord, far_coord});
    assert(far_neighbor_chunk != nullptr);
    assert(far_neighbor_chunk->dirty().contains(heartstead::world::ChunkDirtyFlag::mesh));
    assert(far_database.edit_log().front().chunk_coord.x == far_coord);
    assert(far_dirty_regions.count(heartstead::dirty::DirtyRegionKind::chunk_mesh) == 1);
    auto far_mesh_regions =
        far_dirty_regions.consume_kind(heartstead::dirty::DirtyRegionKind::chunk_mesh);
    assert(far_mesh_regions.size() == 1);
    assert(far_mesh_regions.front().bounds.max.x == far_coord + 1);

    heartstead::world::ChunkDatabase loaded_database;
    heartstead::dirty::DirtyRegionTracker loaded_dirty_regions;
    const std::vector<heartstead::world::VoxelEditRecord> saved_edits{
        {{7, 0, 0},
         {1, 2, 3},
         heartstead::world::VoxelCell::air(),
         heartstead::world::VoxelCell{11, 4}},
        {{7, 0, 0},
         {heartstead::world::VoxelChunk::edge_length - 1, 2, 3},
         heartstead::world::VoxelCell::air(),
         heartstead::world::VoxelCell{12, 5}},
    };
    assert(loaded_database.apply_saved_edits(saved_edits, loaded_dirty_regions));
    assert(loaded_database.edit_log().size() == 2);
    auto saved_cell = loaded_database.get({7, 0, 0}, {1, 2, 3});
    assert(saved_cell);
    assert(saved_cell.value().type == 11);
    assert(loaded_database.stats().dirty_mesh_count == 1);
    assert(loaded_database.stats().dirty_save_count == 0);
    assert(loaded_database.stats().dirty_replication_count == 0);
    assert(loaded_dirty_regions.count(heartstead::dirty::DirtyRegionKind::chunk_mesh) == 1);
}

void test_dirty_region_tracker() {
    using namespace heartstead::dirty;

    DirtyRegionTracker tracker;
    assert(tracker.empty());

    auto marked = tracker.mark(DirtyRegionKind::room_graph, {{0, 0, 0}, {2, 2, 2}}, "dig");
    assert(marked);
    marked = tracker.mark(DirtyRegionKind::room_graph, {{3, 0, 0}, {4, 2, 2}}, "place");
    assert(marked);
    marked = tracker.mark_single(DirtyRegionKind::power_network, {10, 0, 0}, "port");
    assert(marked);
    assert(tracker.size() == 2);
    assert(tracker.count(DirtyRegionKind::room_graph) == 1);
    assert(tracker.count(DirtyRegionKind::power_network) == 1);

    auto dirty_inspection = heartstead::debug::Inspector::inspect(tracker);
    assert(dirty_inspection.object_type == "dirty_region_tracker");
    assert(dirty_inspection.state == "dirty");
    assert(dirty_inspection.find_field("region_count")->value == "2");
    assert(dirty_inspection.find_field("dirty_region_room_graph_count")->value == "1");
    assert(dirty_inspection.find_field("dirty_region_power_network_count")->value == "1");
    assert(dirty_inspection.find_field("dirty_region_chunk_mesh_count")->value == "0");
    assert(dirty_inspection.find_field("first_sequence")->value == "1");
    assert(dirty_inspection.find_field("last_sequence")->value == "2");
    assert(dirty_inspection.find_field("first_region_kind")->value == "room_graph");
    assert(dirty_inspection.find_field("first_region_bounds")->value == "0|0|0..4|2|2");
    assert(dirty_inspection.find_field("first_region_reason")->value == "dig");

    DirtyRegionTracker transitive;
    assert(transitive.mark_single(DirtyRegionKind::chunk_mesh, {0, 0, 0}, "left"));
    assert(transitive.mark_single(DirtyRegionKind::chunk_mesh, {4, 0, 0}, "right"));
    assert(transitive.mark(DirtyRegionKind::chunk_mesh, {{1, 0, 0}, {3, 0, 0}}, "bridge"));
    assert(transitive.size() == 1);
    assert((transitive.regions().front().bounds.min == DirtyRegionCoord{0, 0, 0}));
    assert((transitive.regions().front().bounds.max == DirtyRegionCoord{4, 0, 0}));

    const auto expanded = tracker.regions().front().bounds.expanded(1);
    assert(expanded);
    assert(expanded.value().min.x == -1);
    assert(expanded.value().max.x == 5);

    auto invalid = tracker.mark(DirtyRegionKind::room_graph, {{5, 0, 0}, {4, 0, 0}}, "bad");
    assert(!invalid);
    assert(dirty_region_kind_name(DirtyRegionKind::smoke_ventilation_network) ==
           "smoke_ventilation_network");

    auto rooms = tracker.consume_kind(DirtyRegionKind::room_graph);
    assert(rooms.size() == 1);
    assert(rooms.front().bounds.min.x == 0);
    assert(rooms.front().bounds.max.x == 4);
    assert(tracker.count(DirtyRegionKind::room_graph) == 0);
    assert(tracker.size() == 1);

    auto remaining = tracker.consume_all();
    assert(remaining.size() == 1);
    assert(tracker.empty());
    dirty_inspection = heartstead::debug::Inspector::inspect(tracker);
    assert(dirty_inspection.state == "clean");
    assert(dirty_inspection.find_field("region_count")->value == "0");
}

void test_workpiece_grid() {
    auto grid_result = heartstead::workpieces::WorkpieceGrid::create({4, 4, 4});
    assert(grid_result);
    auto grid = std::move(grid_result).value();

    auto add_status = grid.apply(heartstead::workpieces::WorkpieceOperation{
        heartstead::workpieces::WorkpieceOperationKind::add_cell,
        {1, 2, 3},
        heartstead::workpieces::WorkpieceCell::solid(5),
    });
    assert(add_status);
    assert(grid.occupied_count() == 1);

    auto duplicate_add = grid.apply(heartstead::workpieces::WorkpieceOperation{
        heartstead::workpieces::WorkpieceOperationKind::add_cell,
        {1, 2, 3},
        heartstead::workpieces::WorkpieceCell::solid(5),
    });
    assert(!duplicate_add);
    assert(grid.occupied_count() == 1);

    auto remove_status = grid.apply(heartstead::workpieces::WorkpieceOperation{
        heartstead::workpieces::WorkpieceOperationKind::remove_cell,
        {1, 2, 3},
        heartstead::workpieces::WorkpieceCell::empty(),
    });
    assert(remove_status);
    assert(grid.occupied_count() == 0);
    assert(grid.history().size() == 2);
}

void test_workpiece_prototype_materialization() {
    const auto workpiece_id = heartstead::core::PrototypeId::parse("base:workpieces/clay_lump");
    const auto material_id = heartstead::core::PrototypeId::parse("base:materials/clay");
    assert(workpiece_id);
    assert(material_id);

    heartstead::modding::GenericPrototype prototype;
    prototype.kind = std::string(heartstead::modding::PrototypeKinds::workpiece);
    prototype.id = workpiece_id.value();
    prototype.display_name = "Clay Lump";
    prototype.fields.emplace("grid", "8x8x8");
    prototype.fields.emplace("material", "base:materials/clay");

    auto definition = heartstead::workpieces::workpiece_definition_from_prototype(prototype);
    assert(definition);
    assert(definition.value().prototype_id == workpiece_id.value());
    assert(definition.value().material_prototype_id == material_id.value());
    assert(definition.value().grid_shape.width == 8);
    assert(definition.value().grid_shape.height == 8);
    assert(definition.value().grid_shape.depth == 8);

    auto grid = definition.value().create_grid();
    assert(grid);
    assert(grid.value().shape() == definition.value().grid_shape);
    assert(grid.value().occupied_count() == 0);

    prototype.fields["grid"] = "0x8x8";
    auto invalid_grid = heartstead::workpieces::workpiece_definition_from_prototype(prototype);
    assert(!invalid_grid);

    prototype.fields["grid"] = "8x8x8";
    prototype.fields["material"] = "not-a-prototype";
    auto invalid_material = heartstead::workpieces::workpiece_definition_from_prototype(prototype);
    assert(!invalid_material);
}

void test_workpiece_template_and_codec() {
    auto grid_result = heartstead::workpieces::WorkpieceGrid::create({3, 3, 3});
    assert(grid_result);
    auto grid = std::move(grid_result).value();

    assert(grid.apply(heartstead::workpieces::WorkpieceOperation{
        heartstead::workpieces::WorkpieceOperationKind::add_cell,
        {1, 1, 1},
        heartstead::workpieces::WorkpieceCell::solid(9),
    }));
    assert(grid.apply(heartstead::workpieces::WorkpieceOperation{
        heartstead::workpieces::WorkpieceOperationKind::add_cell,
        {1, 1, 2},
        heartstead::workpieces::WorkpieceCell::solid(9),
    }));

    heartstead::workpieces::WorkpieceTemplate templ;
    templ.id = "test_template";
    templ.shape = grid.shape();
    templ.required_cells.push_back({{1, 1, 1}, heartstead::workpieces::WorkpieceCell::solid(9)});
    templ.required_cells.push_back({{1, 1, 2}, heartstead::workpieces::WorkpieceCell::solid(9)});
    templ.forbidden_cells.push_back({0, 0, 0});
    templ.strict = true;
    assert(templ.validate_definition());

    auto validation = heartstead::workpieces::WorkpieceTemplateMatcher::match(grid, templ);
    assert(validation.valid);

    auto encoded = heartstead::workpieces::WorkpieceGridTextCodec::encode(grid);
    auto decoded = heartstead::workpieces::WorkpieceGridTextCodec::decode(encoded);
    assert(decoded);
    assert(decoded.value().shape() == grid.shape());
    assert(decoded.value().occupied_count() == 2);
    auto decoded_cell = decoded.value().get({1, 1, 2});
    assert(decoded_cell);
    assert(decoded_cell.value().material == 9);

    auto decoded_validation =
        heartstead::workpieces::WorkpieceTemplateMatcher::match(decoded.value(), templ);
    assert(decoded_validation.valid);

    auto wrong_material_template = templ;
    wrong_material_template.required_cells.front().cell =
        heartstead::workpieces::WorkpieceCell::solid(3);
    validation =
        heartstead::workpieces::WorkpieceTemplateMatcher::match(grid, wrong_material_template);
    assert(!validation.valid);
    assert(validation.mismatched_required_cells.size() == 1);

    auto forbidden_status = grid.apply(heartstead::workpieces::WorkpieceOperation{
        heartstead::workpieces::WorkpieceOperationKind::add_cell,
        {0, 0, 0},
        heartstead::workpieces::WorkpieceCell::solid(9),
    });
    assert(forbidden_status);
    validation = heartstead::workpieces::WorkpieceTemplateMatcher::match(grid, templ);
    assert(!validation.valid);
    assert(validation.forbidden_occupied_cells.size() == 1);

    auto inspection = heartstead::debug::Inspector::inspect(grid);
    assert(inspection.object_type == "workpiece_grid");
    assert(inspection.state == "occupied");
    const auto* occupied_cells = inspection.find_field("occupied_cells");
    assert(occupied_cells != nullptr);
    assert(occupied_cells->value == "3");
    const auto* material_counts = inspection.find_field("material_counts");
    assert(material_counts != nullptr);
    assert(material_counts->value == "9=3");

    auto invalid_decode = heartstead::workpieces::WorkpieceGridTextCodec::decode("bad\n");
    assert(!invalid_decode);
}

void test_save_metadata_and_ids() {
    heartstead::save::SaveMetadata metadata;
    metadata.game_version = "0.1.0";
    metadata.world_seed = 1234;
    metadata.enabled_mods.push_back({"base", "0.0.1", "hash-placeholder"});
    assert(metadata.validate());

    auto duplicate_metadata = metadata;
    duplicate_metadata.enabled_mods.push_back({"base", "0.0.2", "hash-other"});
    auto duplicate_status = duplicate_metadata.validate();
    assert(!duplicate_status);
    assert(duplicate_status.error().code == "save.duplicate_mod");

    auto duplicate_migration_metadata = metadata;
    duplicate_migration_metadata.migration_history.push_back("0001-add-foundation");
    duplicate_migration_metadata.migration_history.push_back("0001-add-foundation");
    auto duplicate_migration_status = duplicate_migration_metadata.validate();
    assert(!duplicate_migration_status);
    assert(duplicate_migration_status.error().code == "save.duplicate_migration_history");

    auto empty_migration_metadata = metadata;
    empty_migration_metadata.migration_history.push_back("");
    auto empty_migration_status = empty_migration_metadata.validate();
    assert(!empty_migration_status);
    assert(empty_migration_status.error().code == "save.empty_migration_history");

    auto invalid_migration_metadata = metadata;
    invalid_migration_metadata.migration_history.push_back("migration with spaces");
    auto invalid_migration_status = invalid_migration_metadata.validate();
    assert(!invalid_migration_status);
    assert(invalid_migration_status.error().code == "save.invalid_migration_history");

    metadata.enabled_mods.push_back({"BadMod", "0.0.1", ""});
    assert(!metadata.validate());

    heartstead::save::SaveIdAllocator allocator(42);
    auto first = allocator.reserve();
    auto second = allocator.reserve();
    assert(first);
    assert(second);
    assert(first.value().value() == 42);
    assert(second.value().value() == 43);
    assert(allocator.peek_next().value() == 44);
}

void test_save_compatibility_checker() {
    heartstead::save::SaveMetadata metadata;
    metadata.game_version = "0.1.0";
    metadata.enabled_mods.push_back({"base", "0.0.1", "hash-a"});

    std::vector<heartstead::modding::ModPrototypeFingerprint> active;
    active.push_back({"base", "0.0.1", "hash-a", 2});

    auto report = heartstead::save::SaveCompatibilityChecker::compare(metadata, active);
    assert(!report.has_errors());
    assert(!report.has_warnings());
    assert(report.matched_mod_count == 1);
    assert(report.missing_mod_count == 0);

    active.front().prototype_hash = "hash-b";
    report = heartstead::save::SaveCompatibilityChecker::compare(metadata, active);
    assert(report.has_errors());
    assert(report.prototype_hash_mismatch_count == 1);
    assert(report.issues.front().code == "save_compatibility.prototype_hash_mismatch");
    assert(report.issues.front().severity == heartstead::save::SaveCompatibilitySeverity::error);

    active.front().prototype_hash = "hash-a";
    active.front().version = "0.0.2";
    report = heartstead::save::SaveCompatibilityChecker::compare(metadata, active);
    assert(!report.has_errors());
    assert(report.has_warnings());
    assert(report.version_mismatch_count == 1);

    active.clear();
    report = heartstead::save::SaveCompatibilityChecker::compare(metadata, active);
    assert(report.has_errors());
    assert(report.missing_mod_count == 1);
    assert(report.issues.front().code == "save_compatibility.missing_mod");

    active.push_back({"base", "0.0.1", "hash-a", 2});
    active.push_back({"expansion", "1.0.0", "hash-extra", 1});
    report = heartstead::save::SaveCompatibilityChecker::compare(metadata, active);
    assert(report.has_errors());
    assert(report.extra_active_mod_count == 1);

    metadata.enabled_mods.push_back({"base", "0.0.1", "hash-a"});
    report = heartstead::save::SaveCompatibilityChecker::compare(metadata, active);
    assert(report.has_errors());
    assert(std::ranges::any_of(report.issues, [](const auto& issue) {
        return issue.code == "save_compatibility.duplicate_saved_mod";
    }));
    assert(heartstead::save::save_compatibility_severity_name(
               heartstead::save::SaveCompatibilitySeverity::warning) == "warning");
}

void test_save_text_codec() {
    heartstead::save::SaveMetadata metadata;
    metadata.schema_version = heartstead::save::current_save_schema_version;
    metadata.game_version = "0.1.0 dev";
    metadata.world_seed = 982451653;
    metadata.enabled_mods.push_back({"base", "0.0.1", "hash|with=%escaping"});
    metadata.migration_history.push_back("0001-language-and-build-system");
    metadata.migration_history.push_back("0002-percent-marker");

    const auto encoded = heartstead::save::SaveTextCodec::encode_metadata(metadata);
    auto decoded = heartstead::save::SaveTextCodec::decode_metadata(encoded);
    assert(decoded);
    assert(decoded.value().schema_version == metadata.schema_version);
    assert(decoded.value().game_version == metadata.game_version);
    assert(decoded.value().world_seed == metadata.world_seed);
    assert(decoded.value().enabled_mods.size() == 1);
    assert(decoded.value().enabled_mods.front().prototype_hash == "hash|with=%escaping");
    assert(decoded.value().migration_history.size() == 2);

    auto invalid = heartstead::save::SaveTextCodec::decode_metadata("not-heartstead\n");
    assert(!invalid);

    auto duplicate_field_text = encoded;
    duplicate_field_text.insert(duplicate_field_text.find('\n') + 1U, "schema_version=2\n");
    auto duplicate_field = heartstead::save::SaveTextCodec::decode_metadata(duplicate_field_text);
    assert(!duplicate_field && duplicate_field.error().code == "save_text.duplicate_field");

    auto trailing_metadata = heartstead::save::SaveTextCodec::decode_metadata(encoded + "junk");
    assert(!trailing_metadata && trailing_metadata.error().code == "save_text.trailing_data");

    auto oversized_metadata = heartstead::save::SaveTextCodec::decode_metadata(
        std::string(16U * 1024U * 1024U + 1U, 'x'));
    assert(!oversized_metadata && oversized_metadata.error().code == "save_text.file_too_large");
}

void test_save_migration_registry() {
    heartstead::save::SaveSnapshot snapshot;
    snapshot.metadata.schema_version = 1;
    snapshot.metadata.game_version = "0.1.0";
    snapshot.metadata.world_seed = 1234;
    snapshot.metadata.enabled_mods.push_back({"base", "0.0.1", "hash"});

    heartstead::save::SaveMigrationRegistry registry;

    heartstead::save::SaveMigrationStep first;
    first.id = "0001-add-base-state";
    first.from_schema_version = 1;
    first.to_schema_version = 2;
    first.description = "Add base mod state placeholder";
    first.apply = [](heartstead::save::SaveSnapshot& active_snapshot) {
        active_snapshot.mod_states.push_back({"base", "migration_state", "created"});
        return heartstead::core::Status::ok();
    };

    heartstead::save::SaveMigrationStep second;
    second.id = "0002-rewrite-state";
    second.from_schema_version = 2;
    second.to_schema_version = 3;
    second.description = "Rewrite base mod state placeholder";
    second.apply = [](heartstead::save::SaveSnapshot& active_snapshot) {
        if (active_snapshot.mod_states.empty()) {
            return heartstead::core::Status::failure("test.missing_state",
                                                     "expected prior migration state");
        }
        active_snapshot.mod_states.front().encoded_state = "rewritten";
        return heartstead::core::Status::ok();
    };

    assert(registry.register_migration(first));
    assert(registry.register_migration(second));
    assert(registry.migration_count() == 2);
    assert(registry.find_by_id("0001-add-base-state") != nullptr);
    assert(registry.find_from_schema(2) != nullptr);
    assert(registry.validate_path(1, 3));

    auto duplicate = registry.register_migration(first);
    assert(!duplicate);

    auto migrated = heartstead::save::SaveMigrationRunner::migrate(snapshot, registry, 3);
    assert(migrated);
    assert(migrated.value().previous_schema_version == 1);
    assert(migrated.value().final_schema_version == 3);
    assert(migrated.value().applied_migrations.size() == 2);
    assert(snapshot.metadata.schema_version == 3);
    assert(snapshot.metadata.migration_history.size() == 2);
    assert(heartstead::save::has_migration_history_entry(snapshot.metadata, "0002-rewrite-state"));
    assert(snapshot.mod_states.size() == 1);
    assert(snapshot.mod_states.front().encoded_state == "rewritten");

    auto no_op = heartstead::save::SaveMigrationRunner::migrate(snapshot, registry, 3);
    assert(no_op);
    assert(no_op.value().applied_migrations.empty());

    heartstead::save::SaveSnapshot missing_path_snapshot;
    missing_path_snapshot.metadata.schema_version = 1;
    missing_path_snapshot.metadata.game_version = "0.1.0";
    missing_path_snapshot.metadata.world_seed = 99;
    heartstead::save::SaveMigrationRegistry empty_registry;
    auto missing_path =
        heartstead::save::SaveMigrationRunner::migrate(missing_path_snapshot, empty_registry, 2);
    assert(!missing_path);

    heartstead::save::SaveMigrationStep invalid;
    invalid.id = "bad migration id";
    invalid.from_schema_version = 1;
    invalid.to_schema_version = 1;
    invalid.description = "Invalid";
    invalid.apply = [](heartstead::save::SaveSnapshot&) { return heartstead::core::Status::ok(); };
    assert(!empty_registry.register_migration(invalid));

    heartstead::save::SaveSnapshot conflict_snapshot;
    conflict_snapshot.metadata.schema_version = 1;
    conflict_snapshot.metadata.game_version = "0.1.0";
    conflict_snapshot.metadata.world_seed = 77;
    conflict_snapshot.metadata.migration_history.push_back("0001-add-base-state");
    auto conflict = heartstead::save::SaveMigrationRunner::migrate(conflict_snapshot, registry, 2);
    assert(!conflict);

    heartstead::save::SaveSnapshot failure_snapshot;
    failure_snapshot.metadata.schema_version = 1;
    failure_snapshot.metadata.game_version = "0.1.0";
    failure_snapshot.metadata.world_seed = 88;
    const auto before_failed_migration =
        heartstead::save::SaveTextCodec::encode_snapshot(failure_snapshot);
    heartstead::save::SaveMigrationRegistry failing_registry;
    auto succeeding_step = first;
    auto failing_step = second;
    failing_step.apply = [](heartstead::save::SaveSnapshot& active_snapshot) {
        active_snapshot.metadata.world_seed = 999;
        active_snapshot.mod_states.push_back({"base", "partial_failure", "must-not-commit"});
        return heartstead::core::Status::failure("test.migration_failed",
                                                 "intentional migration failure");
    };
    assert(failing_registry.register_migration(std::move(succeeding_step)));
    assert(failing_registry.register_migration(std::move(failing_step)));
    auto failed =
        heartstead::save::SaveMigrationRunner::migrate(failure_snapshot, failing_registry, 3);
    assert(!failed && failed.error().code == "test.migration_failed");
    assert(heartstead::save::SaveTextCodec::encode_snapshot(failure_snapshot) ==
           before_failed_migration);

    heartstead::save::SaveSnapshot changed_schema_snapshot;
    changed_schema_snapshot.metadata.schema_version = 1;
    changed_schema_snapshot.metadata.game_version = "0.1.0";
    const auto before_changed_schema =
        heartstead::save::SaveTextCodec::encode_snapshot(changed_schema_snapshot);
    heartstead::save::SaveMigrationRegistry changed_schema_registry;
    auto changed_schema_step = first;
    changed_schema_step.apply = [](heartstead::save::SaveSnapshot& active_snapshot) {
        active_snapshot.metadata.schema_version = 99;
        return heartstead::core::Status::ok();
    };
    assert(changed_schema_registry.register_migration(std::move(changed_schema_step)));
    auto changed_schema = heartstead::save::SaveMigrationRunner::migrate(
        changed_schema_snapshot, changed_schema_registry, 2);
    assert(!changed_schema &&
           changed_schema.error().code == "save_migration.callback_changed_schema");
    assert(heartstead::save::SaveTextCodec::encode_snapshot(changed_schema_snapshot) ==
           before_changed_schema);

    heartstead::save::SaveSnapshot invalid_metadata_snapshot;
    invalid_metadata_snapshot.metadata.schema_version = 1;
    invalid_metadata_snapshot.metadata.game_version = "0.1.0";
    const auto before_invalid_metadata =
        heartstead::save::SaveTextCodec::encode_snapshot(invalid_metadata_snapshot);
    heartstead::save::SaveMigrationRegistry invalid_metadata_registry;
    auto invalid_metadata_step = first;
    invalid_metadata_step.apply = [](heartstead::save::SaveSnapshot& active_snapshot) {
        active_snapshot.metadata.migration_history.push_back("invalid migration id");
        return heartstead::core::Status::ok();
    };
    assert(invalid_metadata_registry.register_migration(std::move(invalid_metadata_step)));
    auto invalid_metadata = heartstead::save::SaveMigrationRunner::migrate(
        invalid_metadata_snapshot, invalid_metadata_registry, 2);
    assert(!invalid_metadata && invalid_metadata.error().code == "save.invalid_migration_history");
    assert(heartstead::save::SaveTextCodec::encode_snapshot(invalid_metadata_snapshot) ==
           before_invalid_metadata);
}

heartstead::modding::PrototypeRegistry build_snapshot_test_registry() {
    using heartstead::modding::GenericPrototype;
    using heartstead::modding::PrototypeKinds;

    auto make_prototype = [](std::string_view kind, std::string_view id,
                             std::string_view display_name) {
        auto parsed = heartstead::core::PrototypeId::parse(id);
        assert(parsed);
        GenericPrototype prototype;
        prototype.kind = std::string(kind);
        prototype.id = parsed.value();
        prototype.display_name = std::string(display_name);
        return prototype;
    };

    std::vector<GenericPrototype> prototypes;
    prototypes.push_back(make_prototype(PrototypeKinds::item, "base:items/raw_clay", "Raw Clay"));
    prototypes.push_back(
        make_prototype(PrototypeKinds::cargo, "base:cargo/heavy_log", "Heavy Log"));
    prototypes.push_back(
        make_prototype(PrototypeKinds::entity, "base:entities/hand_cart", "Hand Cart"));
    prototypes.push_back(
        make_prototype(PrototypeKinds::build_piece, "base:build_pieces/wall_frame", "Wall Frame"));
    prototypes.push_back(make_prototype(PrototypeKinds::build_piece,
                                        "base:build_pieces/kiln_firebox", "Kiln Firebox"));
    prototypes.push_back(
        make_prototype(PrototypeKinds::build_piece, "base:build_pieces/chimney", "Chimney"));
    prototypes.push_back(
        make_prototype(PrototypeKinds::assembly, "base:assemblies/clay_kiln", "Clay Kiln"));
    prototypes.push_back(
        make_prototype(PrototypeKinds::workpiece, "base:workpieces/clay_lump", "Clay Lump"));
    prototypes.push_back(
        make_prototype(PrototypeKinds::process, "base:processes/drying", "Drying"));

    heartstead::modding::PrototypeRegistry registry;
    auto result = registry.build(std::move(prototypes));
    assert(!result.has_errors());
    return registry;
}

void test_save_snapshot_validation() {
    auto registry = build_snapshot_test_registry();

    const auto raw_clay = heartstead::core::PrototypeId::parse("base:items/raw_clay");
    const auto heavy_log = heartstead::core::PrototypeId::parse("base:cargo/heavy_log");
    const auto cart = heartstead::core::PrototypeId::parse("base:entities/hand_cart");
    const auto wall = heartstead::core::PrototypeId::parse("base:build_pieces/wall_frame");
    const auto firebox = heartstead::core::PrototypeId::parse("base:build_pieces/kiln_firebox");
    const auto chimney = heartstead::core::PrototypeId::parse("base:build_pieces/chimney");
    const auto kiln = heartstead::core::PrototypeId::parse("base:assemblies/clay_kiln");
    const auto clay_lump = heartstead::core::PrototypeId::parse("base:workpieces/clay_lump");
    const auto drying = heartstead::core::PrototypeId::parse("base:processes/drying");
    assert(raw_clay && heavy_log && cart && wall && firebox && chimney && kiln && clay_lump &&
           drying);

    heartstead::save::SaveSnapshot snapshot;
    snapshot.metadata.game_version = "0.1.0";
    snapshot.metadata.world_seed = 456;
    snapshot.metadata.enabled_mods.push_back({"base", "0.0.1", "hash"});
    constexpr std::int64_t far_chunk_coord = 1LL << 40;
    const heartstead::world::VoxelEditRecord chunk_edit{
        {far_chunk_coord, -far_chunk_coord, far_chunk_coord + 3},
        {1, 2, 3},
        heartstead::world::VoxelCell::air(),
        {7, 2},
    };
    const std::vector<const heartstead::world::VoxelEditRecord*> chunk_edits{&chunk_edit};
    const auto encoded_chunk_edit_delta =
        heartstead::world::ChunkEditDeltaTextCodec::encode(chunk_edit.chunk_coord, chunk_edits);
    snapshot.chunk_edits.push_back({chunk_edit.chunk_coord, encoded_chunk_edit_delta});

    heartstead::build::BuildPieceRecord wall_record;
    wall_record.object_id = heartstead::core::SaveId::from_value(800);
    wall_record.prototype_id = wall.value();
    wall_record.construction_state = heartstead::build::ConstructionState::complete;
    wall_record.room_contribution_tags.push_back("wall");
    snapshot.build_pieces.push_back(wall_record);

    heartstead::build::BuildPieceRecord firebox_record;
    firebox_record.object_id = heartstead::core::SaveId::from_value(801);
    firebox_record.prototype_id = firebox.value();
    firebox_record.construction_state = heartstead::build::ConstructionState::complete;
    snapshot.build_pieces.push_back(firebox_record);

    heartstead::build::BuildPieceRecord chimney_record;
    chimney_record.object_id = heartstead::core::SaveId::from_value(802);
    chimney_record.prototype_id = chimney.value();
    chimney_record.construction_state = heartstead::build::ConstructionState::complete;
    snapshot.build_pieces.push_back(chimney_record);

    heartstead::save::EntitySaveRecord entity_record;
    entity_record.save_id = heartstead::core::SaveId::from_value(700);
    entity_record.prototype_id = cart.value();
    entity_record.kind = heartstead::entities::EntityKind::cart;
    entity_record.sleeping = true;
    entity_record.encoded_state = "entity-state";
    entity_record.transform.position = {12.5, 0.0, -4.0};
    entity_record.transform.rotation_degrees = {0.0, 45.0, 0.0};
    snapshot.entities.push_back(entity_record);

    auto stack = heartstead::items::ItemStack::create(raw_clay.value(), 12, 64);
    assert(stack);
    snapshot.inventories.push_back({wall_record.object_id, {stack.value()}});

    heartstead::cargo::CargoRecord cargo_record;
    cargo_record.cargo_id = heartstead::core::SaveId::from_value(500);
    cargo_record.prototype_id = heavy_log.value();
    cargo_record.position = {7.25, 0.0, -3.5};
    cargo_record.mass_grams = 90000;
    cargo_record.volume_milliliters = 180000;
    cargo_record.allowed_transport_modes =
        heartstead::cargo::CargoTransportModes::of({heartstead::cargo::CargoTransportMode::cart});
    snapshot.cargo_records.push_back(cargo_record);

    auto saved_workpiece_grid = heartstead::workpieces::WorkpieceGrid::create({4, 4, 4});
    assert(saved_workpiece_grid);
    const auto encoded_workpiece_cells =
        heartstead::workpieces::WorkpieceGridTextCodec::encode(saved_workpiece_grid.value());
    snapshot.workpieces.push_back({heartstead::core::WorkpieceId::from_value(12),
                                   clay_lump.value(),
                                   {4, 4, 4},
                                   encoded_workpiece_cells});

    heartstead::assemblies::AssemblyRecord assembly;
    assembly.assembly_id = heartstead::core::SaveId::from_value(900);
    assembly.root_build_piece_id = firebox_record.object_id;
    assembly.prototype_id = kiln.value();
    assembly.parts.push_back({"firebox", firebox_record.object_id, firebox.value()});
    assembly.parts.push_back({"chimney", chimney_record.object_id, chimney.value()});
    assembly.ports.push_back(
        {"fuel_input", heartstead::networks::NetworkKind::logistics, firebox_record.object_id, 2});
    assembly.ports.push_back({"smoke_output", heartstead::networks::NetworkKind::smoke_ventilation,
                              chimney_record.object_id, 1});
    assembly.process_slots.push_back(heartstead::core::ProcessId::from_value(300));
    snapshot.assemblies.push_back(assembly);

    auto process = heartstead::processes::ProcessRuntime::create(
        heartstead::core::ProcessId::from_value(300), assembly.assembly_id, drying.value(), 0,
        60000);
    assert(process);
    process.value().input_slots.push_back({raw_clay.value(), 4});
    snapshot.processes.push_back(std::move(process).value());

    snapshot.mod_states.push_back({"base", "test_state", "opaque mod state"});

    auto validation = heartstead::save::SaveSnapshotValidator::validate(snapshot, registry);
    assert(validation.valid());

    const auto encoded_snapshot = heartstead::save::SaveTextCodec::encode_snapshot(snapshot);
    auto decoded_snapshot = heartstead::save::SaveTextCodec::decode_snapshot(encoded_snapshot);
    assert(decoded_snapshot);
    validation =
        heartstead::save::SaveSnapshotValidator::validate(decoded_snapshot.value(), registry);
    assert(validation.valid());
    assert(decoded_snapshot.value().metadata.game_version == snapshot.metadata.game_version);
    assert(decoded_snapshot.value().chunk_edits.size() == 1);
    assert(decoded_snapshot.value().chunk_edits.front().coord.x == far_chunk_coord);
    assert(decoded_snapshot.value().chunk_edits.front().coord.y == -far_chunk_coord);
    assert(decoded_snapshot.value().chunk_edits.front().coord.z == far_chunk_coord + 3);
    assert(decoded_snapshot.value().build_pieces.size() == 3);
    assert(decoded_snapshot.value().entities.size() == 1);
    assert(decoded_snapshot.value().entities.front().transform.position.approximate_global().x ==
           12.5);
    assert(decoded_snapshot.value().entities.front().transform.rotation_degrees.y == 45.0);
    assert(decoded_snapshot.value().inventories.size() == 1);
    assert(decoded_snapshot.value().cargo_records.size() == 1);
    assert(decoded_snapshot.value().workpieces.size() == 1);
    assert(decoded_snapshot.value().assemblies.size() == 1);
    assert(decoded_snapshot.value().assemblies.front().ports.size() == 2);
    assert(decoded_snapshot.value().assemblies.front().ports.front().source_build_piece_id ==
           firebox_record.object_id);
    assert(decoded_snapshot.value().assemblies.front().ports.front().capacity == 2);
    assert(decoded_snapshot.value().processes.size() == 1);
    assert(decoded_snapshot.value().mod_states.size() == 1);
    assert(decoded_snapshot.value().chunk_edits.front().encoded_edit_delta ==
           encoded_chunk_edit_delta);
    assert(decoded_snapshot.value().build_pieces.front().object_id == wall_record.object_id);
    assert(decoded_snapshot.value().cargo_records.front().mass_grams == 90000);
    assert(decoded_snapshot.value().cargo_records.front().position.approximate_global().x == 7.25);
    assert(decoded_snapshot.value().cargo_records.front().position.approximate_global().z == -3.5);
    assert(decoded_snapshot.value().processes.front().input_slots.front().count == 4);
    assert(decoded_snapshot.value().mod_states.front().encoded_state == "opaque mod state");

    auto duplicate_snapshot_field_text = encoded_snapshot;
    duplicate_snapshot_field_text.insert(duplicate_snapshot_field_text.find('\n') + 1U,
                                         "schema_version=2\n");
    auto duplicate_snapshot_field =
        heartstead::save::SaveTextCodec::decode_snapshot(duplicate_snapshot_field_text);
    assert(!duplicate_snapshot_field &&
           duplicate_snapshot_field.error().code == "save_text.duplicate_field");

    auto trailing_snapshot =
        heartstead::save::SaveTextCodec::decode_snapshot(encoded_snapshot + "junk");
    assert(!trailing_snapshot && trailing_snapshot.error().code == "save_text.trailing_data");

    std::string collection_amplification = "heartstead.save_snapshot_text.v2\n"
                                           "schema_version=2\n"
                                           "game_version=0.1.0\n"
                                           "world_seed=1\n"
                                           "world_time=0\n"
                                           "mod_state=base|state|";
    collection_amplification.append(1'000'000U, ',');
    auto excessive_collection =
        heartstead::save::SaveTextCodec::decode_snapshot(collection_amplification);
    assert(!excessive_collection &&
           excessive_collection.error().code == "save_text.limit_exceeded");

    std::string field_amplification = "heartstead.save_snapshot_text.v2\n"
                                      "schema_version=2\n"
                                      "game_version=0.1.0\n"
                                      "world_seed=1\n"
                                      "world_time=0\n"
                                      "mod_state=";
    field_amplification.append(1'000'000U, '|');
    field_amplification += "\nend\n";
    auto excessive_fields = heartstead::save::SaveTextCodec::decode_snapshot(field_amplification);
    assert(!excessive_fields && excessive_fields.error().code == "save_text.invalid_mod_state");

    const auto binary_snapshot = heartstead::save::SaveBinaryCodec::encode_snapshot(snapshot);
    assert(binary_snapshot && !binary_snapshot.value().empty());
    auto decoded_binary =
        heartstead::save::SaveBinaryCodec::decode_snapshot(binary_snapshot.value());
    assert(decoded_binary);
    validation =
        heartstead::save::SaveSnapshotValidator::validate(decoded_binary.value(), registry);
    assert(validation.valid());
    assert(decoded_binary.value().metadata.game_version == snapshot.metadata.game_version);
    assert(decoded_binary.value().metadata.enabled_mods.front().id == "base");
    assert(decoded_binary.value().chunk_edits.front().coord.x == far_chunk_coord);
    assert(decoded_binary.value().chunk_edits.front().coord.y == -far_chunk_coord);
    assert(decoded_binary.value().chunk_edits.front().coord.z == far_chunk_coord + 3);
    assert(decoded_binary.value().chunk_edits.front().encoded_edit_delta ==
           encoded_chunk_edit_delta);
    assert(decoded_binary.value().build_pieces.size() == 3);
    assert(decoded_binary.value().build_pieces.front().room_contribution_tags.front() == "wall");
    assert(decoded_binary.value().entities.front().sleeping);
    assert(decoded_binary.value().entities.front().transform.position.approximate_global().x ==
           12.5);
    assert(decoded_binary.value().entities.front().transform.rotation_degrees.y == 45.0);
    assert(decoded_binary.value().inventories.front().stacks.front().count == 12);
    assert(decoded_binary.value().cargo_records.front().mass_grams == 90000);
    assert(decoded_binary.value().cargo_records.front().position.approximate_global().x == 7.25);
    assert(decoded_binary.value().cargo_records.front().position.approximate_global().z == -3.5);
    assert(decoded_binary.value().workpieces.front().shape.width == 4);
    assert(decoded_binary.value().assemblies.front().parts.size() == 2);
    assert(decoded_binary.value().assemblies.front().ports.front().source_build_piece_id ==
           firebox_record.object_id);
    assert(decoded_binary.value().assemblies.front().ports.front().capacity == 2);
    assert(decoded_binary.value().processes.front().input_slots.front().count == 4);
    assert(decoded_binary.value().mod_states.front().encoded_state == "opaque mod state");

    heartstead::save::SaveSnapshot oversized_binary_snapshot;
    oversized_binary_snapshot.metadata.game_version = "codec-limit-test";
    oversized_binary_snapshot.mod_states.push_back(
        {"base", "oversized", std::string(16U * 1024U * 1024U + 1U, 'x')});
    const auto oversized_binary =
        heartstead::save::SaveBinaryCodec::encode_snapshot(oversized_binary_snapshot);
    assert(!oversized_binary);
    assert(oversized_binary.error().code == "save_binary.string_too_large");

    const std::string legacy_entity_snapshot =
        "heartstead.save_snapshot_text.v1\n"
        "schema_version=1\n"
        "game_version=0.1.0\n"
        "world_seed=456\n"
        "entity=700|base:entities/hand_cart|cart|1|legacy-state\n"
        "end\n";
    auto decoded_legacy_entity =
        heartstead::save::SaveTextCodec::decode_snapshot(legacy_entity_snapshot);
    assert(decoded_legacy_entity);
    assert(decoded_legacy_entity.value().entities.size() == 1);
    assert(
        decoded_legacy_entity.value().entities.front().transform.position.approximate_global().x ==
        0.0);
    assert(decoded_legacy_entity.value().entities.front().transform.scale.x == 1.0);

    const std::string legacy_cargo_snapshot =
        "heartstead.save_snapshot_text.v1\n"
        "schema_version=1\n"
        "game_version=0.1.0\n"
        "world_seed=456\n"
        "cargo=500|base:cargo/heavy_log|90000|180000|1000|2|crush\n"
        "end\n";
    auto decoded_legacy_cargo =
        heartstead::save::SaveTextCodec::decode_snapshot(legacy_cargo_snapshot);
    assert(decoded_legacy_cargo);
    assert(decoded_legacy_cargo.value().cargo_records.size() == 1);
    assert(decoded_legacy_cargo.value().cargo_records.front().position.approximate_global().x ==
           0.0);
    assert(decoded_legacy_cargo.value().cargo_records.front().mass_grams == 90000);

    auto unknown_transport_snapshot = snapshot;
    const auto unknown_transport_bits =
        static_cast<std::uint32_t>(heartstead::cargo::CargoTransportMode::cart) | (1u << 10u);
    unknown_transport_snapshot.cargo_records.front().allowed_transport_modes =
        heartstead::cargo::CargoTransportModes::from_bits(unknown_transport_bits);
    auto decoded_unknown_text = heartstead::save::SaveTextCodec::decode_snapshot(
        heartstead::save::SaveTextCodec::encode_snapshot(unknown_transport_snapshot));
    assert(decoded_unknown_text);
    assert(decoded_unknown_text.value().cargo_records.front().allowed_transport_modes.bits() ==
           unknown_transport_bits);
    validation =
        heartstead::save::SaveSnapshotValidator::validate(decoded_unknown_text.value(), registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "cargo.invalid_transport_mode";
    }));

    auto encoded_unknown_binary =
        heartstead::save::SaveBinaryCodec::encode_snapshot(unknown_transport_snapshot);
    assert(encoded_unknown_binary);
    auto decoded_unknown_binary =
        heartstead::save::SaveBinaryCodec::decode_snapshot(encoded_unknown_binary.value());
    assert(decoded_unknown_binary);
    assert(decoded_unknown_binary.value().cargo_records.front().allowed_transport_modes.bits() ==
           unknown_transport_bits);
    validation =
        heartstead::save::SaveSnapshotValidator::validate(decoded_unknown_binary.value(), registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "cargo.invalid_transport_mode";
    }));

    auto truncated_binary = binary_snapshot.value();
    truncated_binary.pop_back();
    auto invalid_binary = heartstead::save::SaveBinaryCodec::decode_snapshot(truncated_binary);
    assert(!invalid_binary);

    auto invalid_snapshot_text =
        heartstead::save::SaveTextCodec::decode_snapshot("heartstead.save_snapshot_text.v1\nbad\n");
    assert(!invalid_snapshot_text);

    auto duplicate_snapshot = snapshot;
    duplicate_snapshot.cargo_records.front().cargo_id = wall_record.object_id;
    validation = heartstead::save::SaveSnapshotValidator::validate(duplicate_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.duplicate_save_id";
    }));

    auto invalid_cargo_hazard_snapshot = snapshot;
    invalid_cargo_hazard_snapshot.cargo_records.front().hazard_tags = {"bad tag"};
    validation =
        heartstead::save::SaveSnapshotValidator::validate(invalid_cargo_hazard_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "cargo.invalid_hazard_tag";
    }));

    auto duplicate_chunk_snapshot = snapshot;
    duplicate_chunk_snapshot.chunk_edits.push_back(snapshot.chunk_edits.front());
    validation =
        heartstead::save::SaveSnapshotValidator::validate(duplicate_chunk_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.duplicate_chunk_edit";
    }));

    auto invalid_chunk_payload_snapshot = snapshot;
    invalid_chunk_payload_snapshot.chunk_edits.front().encoded_edit_delta = "bad\n";
    validation =
        heartstead::save::SaveSnapshotValidator::validate(invalid_chunk_payload_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "world_snapshot.invalid_chunk_delta_magic";
    }));

    auto mismatched_chunk_coord_snapshot = snapshot;
    mismatched_chunk_coord_snapshot.chunk_edits.front().coord = {1, 0, 0};
    validation = heartstead::save::SaveSnapshotValidator::validate(mismatched_chunk_coord_snapshot,
                                                                   registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "world_snapshot.chunk_delta_coord_mismatch";
    }));

    auto invalid_build_piece_snapshot = snapshot;
    invalid_build_piece_snapshot.build_pieces.front().material_tags.push_back("bad tag");
    validation =
        heartstead::save::SaveSnapshotValidator::validate(invalid_build_piece_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "build_piece.invalid_material_tag";
    }));

    auto invalid_entity_snapshot = snapshot;
    invalid_entity_snapshot.entities.front().kind =
        static_cast<heartstead::entities::EntityKind>(99);
    validation =
        heartstead::save::SaveSnapshotValidator::validate(invalid_entity_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.invalid_entity_kind";
    }));

    auto invalid_entity_transform_snapshot = snapshot;
    invalid_entity_transform_snapshot.entities.front().transform.scale.y = 0.0;
    validation = heartstead::save::SaveSnapshotValidator::validate(
        invalid_entity_transform_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.invalid_entity_transform_scale";
    }));

    auto duplicate_inventory_snapshot = snapshot;
    duplicate_inventory_snapshot.inventories.push_back(snapshot.inventories.front());
    validation =
        heartstead::save::SaveSnapshotValidator::validate(duplicate_inventory_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.duplicate_inventory_owner";
    }));

    auto invalid_inventory_stack_snapshot = snapshot;
    invalid_inventory_stack_snapshot.inventories.front().stacks.front().count =
        invalid_inventory_stack_snapshot.inventories.front().stacks.front().max_count + 1;
    validation = heartstead::save::SaveSnapshotValidator::validate(invalid_inventory_stack_snapshot,
                                                                   registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.invalid_item_stack_count";
    }));

    invalid_inventory_stack_snapshot = snapshot;
    invalid_inventory_stack_snapshot.inventories.front().stacks.front().max_count = 0;
    validation = heartstead::save::SaveSnapshotValidator::validate(invalid_inventory_stack_snapshot,
                                                                   registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.invalid_item_stack_count";
    }));

    auto duplicate_process_snapshot = snapshot;
    duplicate_process_snapshot.processes.push_back(snapshot.processes.front());
    validation =
        heartstead::save::SaveSnapshotValidator::validate(duplicate_process_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.duplicate_process_id";
    }));

    auto missing_assembly_process_snapshot = snapshot;
    missing_assembly_process_snapshot.assemblies.front().process_slots.front() =
        heartstead::core::ProcessId::from_value(9999);
    validation = heartstead::save::SaveSnapshotValidator::validate(
        missing_assembly_process_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.missing_assembly_process";
    }));

    auto foreign_assembly_process_snapshot = snapshot;
    foreign_assembly_process_snapshot.processes.front().owner_id = wall_record.object_id;
    validation = heartstead::save::SaveSnapshotValidator::validate(
        foreign_assembly_process_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.assembly_process_owner_mismatch";
    }));

    auto invalid_workpiece_payload_snapshot = snapshot;
    invalid_workpiece_payload_snapshot.workpieces.front().encoded_cells = "bad\n";
    validation = heartstead::save::SaveSnapshotValidator::validate(
        invalid_workpiece_payload_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "workpiece_codec.invalid_magic";
    }));

    auto mismatched_workpiece_shape_snapshot = snapshot;
    mismatched_workpiece_shape_snapshot.workpieces.front().shape = {3, 4, 4};
    validation = heartstead::save::SaveSnapshotValidator::validate(
        mismatched_workpiece_shape_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.workpiece_shape_mismatch";
    }));

    auto duplicate_mod_state_snapshot = snapshot;
    duplicate_mod_state_snapshot.mod_states.push_back(snapshot.mod_states.front());
    validation =
        heartstead::save::SaveSnapshotValidator::validate(duplicate_mod_state_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.duplicate_mod_state";
    }));

    auto disabled_mod_state_snapshot = snapshot;
    disabled_mod_state_snapshot.mod_states.push_back({"disabled", "state", "opaque"});
    validation =
        heartstead::save::SaveSnapshotValidator::validate(disabled_mod_state_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.disabled_mod_state";
    }));

    auto engine_mod_state_snapshot = snapshot;
    engine_mod_state_snapshot.mod_states.front().mod_id = "engine";
    validation =
        heartstead::save::SaveSnapshotValidator::validate(engine_mod_state_snapshot, registry);
    assert(validation.valid());

    auto duplicate_assembly_part_snapshot = snapshot;
    duplicate_assembly_part_snapshot.assemblies.front().parts.push_back(
        {"firebox", chimney_record.object_id, chimney.value()});
    validation = heartstead::save::SaveSnapshotValidator::validate(duplicate_assembly_part_snapshot,
                                                                   registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "assembly.duplicate_part";
    }));

    auto missing_assembly_port_source_snapshot = snapshot;
    missing_assembly_port_source_snapshot.assemblies.front().ports.front().source_build_piece_id =
        heartstead::core::SaveId::from_value(9999);
    validation = heartstead::save::SaveSnapshotValidator::validate(
        missing_assembly_port_source_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.missing_assembly_port_source";
    }));

    auto invalid_process_snapshot = snapshot;
    invalid_process_snapshot.processes.front().accumulated_effective_work_ms =
        invalid_process_snapshot.processes.front().required_effective_work_ms + 1;
    invalid_process_snapshot.processes.front().started_at = 1;
    invalid_process_snapshot.processes.front().last_eval = 0;
    validation =
        heartstead::save::SaveSnapshotValidator::validate(invalid_process_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "process.invalid_accumulated_work";
    }));

    auto invalid_process_time_snapshot = snapshot;
    invalid_process_time_snapshot.processes.front().started_at = 1;
    invalid_process_time_snapshot.processes.front().last_eval = 0;
    validation =
        heartstead::save::SaveSnapshotValidator::validate(invalid_process_time_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(
        validation.issues, [](const auto& issue) { return issue.code == "process.invalid_time"; }));

    auto invalid_process_state_snapshot = snapshot;
    invalid_process_state_snapshot.processes.front().state =
        static_cast<heartstead::processes::ProcessState>(99);
    validation =
        heartstead::save::SaveSnapshotValidator::validate(invalid_process_state_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "process.invalid_state";
    }));

    auto invalid_complete_process_snapshot = snapshot;
    invalid_complete_process_snapshot.processes.front().state =
        heartstead::processes::ProcessState::complete;
    validation = heartstead::save::SaveSnapshotValidator::validate(
        invalid_complete_process_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "process.invalid_complete_work";
    }));

    auto invalid_process_slot_snapshot = snapshot;
    invalid_process_slot_snapshot.processes.front().input_slots.front().count = 0;
    validation =
        heartstead::save::SaveSnapshotValidator::validate(invalid_process_slot_snapshot, registry);
    assert(!validation.valid());
    assert(std::ranges::any_of(validation.issues, [](const auto& issue) {
        return issue.code == "process_slot.invalid_count";
    }));

    auto wrong_kind_snapshot = snapshot;
    wrong_kind_snapshot.cargo_records.front().prototype_id = raw_clay.value();
    validation = heartstead::save::SaveSnapshotValidator::validate(wrong_kind_snapshot, registry);
    assert(!validation.valid());
}

void test_file_save_database() {
    const auto root = make_temp_root() / "save_database";
    heartstead::save::FileSaveDatabase database(root);

    heartstead::save::SaveSnapshot snapshot;
    snapshot.metadata.game_version = "0.1.0";
    snapshot.metadata.world_seed = 9001;
    snapshot.metadata.enabled_mods.push_back({"base", "0.0.1", "hash"});
    snapshot.chunk_edits.push_back({{1, -2, 3}, "delta-a"});
    snapshot.chunk_edits.push_back({{2, -2, 3}, "delta-b"});
    snapshot.mod_states.push_back({"base", "database_test", "opaque"});

    auto status = database.write_snapshot(snapshot);
    assert(status);

    auto stats = database.stats();
    assert(stats);
    assert(stats.value().has_snapshot);
    assert(stats.value().snapshot_bytes > 0);
    assert(stats.value().chunk_delta_count == 2);
    assert(stats.value().chunk_delta_bytes == 14);
    assert(stats.value().uses_generation_manifest);
    assert(stats.value().active_generation == "generation_1");
    assert(stats.value().committed_generation_count == 1);
    assert(stats.value().staged_generation_count == 0);
    assert(stats.value().stale_generation_count == 0);

    auto stats_inspection = heartstead::debug::Inspector::inspect(stats.value());
    assert(stats_inspection.object_type == "save_database_stats");
    assert(stats_inspection.state == "generation");
    assert(stats_inspection.find_field("layout")->value == "generation");
    assert(stats_inspection.find_field("active_generation")->value == "generation_1");
    assert(stats_inspection.find_field("chunk_delta_count")->value == "2");

    auto loaded = database.read_snapshot();
    assert(loaded);
    assert(loaded.value().metadata.world_seed == 9001);
    assert(loaded.value().chunk_edits.size() == 2);
    assert(loaded.value().chunk_edits.front().encoded_edit_delta == "delta-a");
    assert(loaded.value().mod_states.front().encoded_state == "opaque");
    assert(std::filesystem::exists(root / "current.txt"));
    assert(std::filesystem::exists(root / "generations" / "generation_1" / "snapshot.hssb"));

    const auto active_generation_before_failed_commit = read_text(root / "current.txt");
    auto invalid_snapshot = snapshot;
    invalid_snapshot.metadata.world_seed = 77;
    invalid_snapshot.chunk_edits.push_back({{9, 9, 9}, ""});
    status = database.write_snapshot(invalid_snapshot);
    assert(!status);
    assert(status.error().code == "save_database.empty_chunk_delta");
    assert(read_text(root / "current.txt") == active_generation_before_failed_commit);
    stats = database.stats();
    assert(stats);
    assert(stats.value().active_generation == "generation_1");
    assert(stats.value().committed_generation_count == 1);
    assert(stats.value().staged_generation_count == 0);
    loaded = database.read_snapshot();
    assert(loaded);
    assert(loaded.value().metadata.world_seed == 9001);
    assert(loaded.value().chunk_edits.size() == 2);

    status = database.write_chunk_delta({{-5, 0, 2}, "streamed-delta"});
    assert(status);
    auto streamed = database.read_chunk_delta({-5, 0, 2});
    assert(streamed);
    assert(streamed.value().encoded_edit_delta == "streamed-delta");
    assert(!std::filesystem::exists(root / "chunks" / "c_-5_0_2.delta"));
    assert(std::filesystem::exists(root / "generations" / "generation_1" / "chunks" /
                                   "c_-5_0_2.delta"));

    constexpr std::int64_t far_chunk_coord = 1LL << 40;
    status = database.write_chunk_delta(
        {{far_chunk_coord, -far_chunk_coord, far_chunk_coord + 2}, "far-delta"});
    assert(status);
    auto far_streamed =
        database.read_chunk_delta({far_chunk_coord, -far_chunk_coord, far_chunk_coord + 2});
    assert(far_streamed);
    assert(far_streamed.value().coord.x == far_chunk_coord);
    assert(far_streamed.value().encoded_edit_delta == "far-delta");
    assert(std::filesystem::exists(root / "generations" / "generation_1" / "chunks" /
                                   ("c_" + std::to_string(far_chunk_coord) + "_" +
                                    std::to_string(-far_chunk_coord) + "_" +
                                    std::to_string(far_chunk_coord + 2) + ".delta")));

    loaded = database.read_snapshot();
    assert(loaded);
    assert(loaded.value().chunk_edits.size() == 4);

    const std::vector<heartstead::save::ChunkEditSaveRecord> replacement{
        {{far_chunk_coord + 4, 0, 0}, "only"}};
    status = database.write_chunk_deltas(replacement);
    assert(status);
    auto deltas = database.read_chunk_deltas();
    assert(deltas);
    assert(deltas.value().size() == 1);
    assert(deltas.value().front().coord.x == far_chunk_coord + 4);
    assert(deltas.value().front().encoded_edit_delta == "only");

    const std::vector<heartstead::save::ChunkEditSaveRecord> invalid_replacement{
        {{far_chunk_coord + 5, 0, 0}, "would-replace"}, {{far_chunk_coord + 6, 0, 0}, ""}};
    status = database.write_chunk_deltas(invalid_replacement);
    assert(!status && status.error().code == "save_database.empty_chunk_delta");
    deltas = database.read_chunk_deltas();
    assert(deltas && deltas.value().size() == 1);
    assert(deltas.value().front().coord.x == far_chunk_coord + 4);
    assert(deltas.value().front().encoded_edit_delta == "only");

    const std::vector<heartstead::save::ChunkEditSaveRecord> duplicate_replacement{
        {{far_chunk_coord + 5, 0, 0}, "first"}, {{far_chunk_coord + 5, 0, 0}, "second"}};
    status = database.write_chunk_deltas(duplicate_replacement);
    assert(!status && status.error().code == "save_database.duplicate_chunk_delta");
    deltas = database.read_chunk_deltas();
    assert(deltas && deltas.value().size() == 1);
    assert(deltas.value().front().coord.x == far_chunk_coord + 4);

    status = database.write_chunk_deltas({});
    assert(status);
    deltas = database.read_chunk_deltas();
    assert(deltas && deltas.value().empty());
    loaded = database.read_snapshot();
    assert(loaded && loaded.value().chunk_edits.empty());

    auto missing = database.read_chunk_delta({99, 0, 0});
    assert(!missing);
    assert(!database.write_chunk_delta({{0, 0, 0}, ""}));

    auto second_snapshot = snapshot;
    second_snapshot.metadata.world_seed = 9002;
    second_snapshot.chunk_edits = {{{3, 0, 0}, "delta-c"}};
    status = database.write_snapshot(second_snapshot);
    assert(status);
    stats = database.stats();
    assert(stats);
    assert(stats.value().active_generation == "generation_2");
    assert(stats.value().committed_generation_count == 2);
    assert(stats.value().staged_generation_count == 0);
    assert(stats.value().stale_generation_count == 1);
    assert(stats.value().chunk_delta_count == 1);

    stats_inspection = heartstead::debug::Inspector::inspect(stats.value());
    assert(stats_inspection.state == "generation");
    assert(stats_inspection.find_field("active_generation")->value == "generation_2");
    assert(stats_inspection.find_field("stale_generation_count")->value == "1");

    loaded = database.read_snapshot();
    assert(loaded);
    assert(loaded.value().metadata.world_seed == 9002);
    assert(loaded.value().chunk_edits.size() == 1);
    assert(loaded.value().chunk_edits.front().encoded_edit_delta == "delta-c");

    auto third_snapshot = snapshot;
    third_snapshot.metadata.world_seed = 9003;
    third_snapshot.chunk_edits = {{{4, 0, 0}, "delta-d"}};
    status = database.write_snapshot(third_snapshot);
    assert(status);
    std::filesystem::create_directories(root / "generations" / "generation_99.tmp");
    stats = database.stats();
    assert(stats);
    assert(stats.value().active_generation == "generation_3");
    assert(stats.value().committed_generation_count == 3);
    assert(stats.value().staged_generation_count == 1);
    assert(stats.value().stale_generation_count == 2);

    status = database.prune_stale_generations(1);
    assert(status);
    stats = database.stats();
    assert(stats);
    assert(stats.value().active_generation == "generation_3");
    assert(stats.value().committed_generation_count == 2);
    assert(stats.value().staged_generation_count == 1);
    assert(stats.value().stale_generation_count == 1);
    assert(!std::filesystem::exists(root / "generations" / "generation_1"));
    assert(std::filesystem::exists(root / "generations" / "generation_2"));
    assert(std::filesystem::exists(root / "generations" / "generation_3"));
    assert(std::filesystem::exists(root / "generations" / "generation_99.tmp"));

    status = database.prune_stale_generations(0);
    assert(status);
    stats = database.stats();
    assert(stats);
    assert(stats.value().committed_generation_count == 1);
    assert(stats.value().stale_generation_count == 0);
    assert(!std::filesystem::exists(root / "generations" / "generation_2"));
    assert(std::filesystem::exists(root / "generations" / "generation_3"));
    assert(std::filesystem::exists(root / "generations" / "generation_99.tmp"));

    auto recovered_staged_generations = database.recover_staged_generations();
    assert(recovered_staged_generations);
    assert(recovered_staged_generations.value() == 1);
    stats = database.stats();
    assert(stats);
    assert(stats.value().active_generation == "generation_3");
    assert(stats.value().staged_generation_count == 0);
    assert(!std::filesystem::exists(root / "generations" / "generation_99.tmp"));

    recovered_staged_generations = database.recover_staged_generations();
    assert(recovered_staged_generations);
    assert(recovered_staged_generations.value() == 0);

    loaded = database.read_snapshot();
    assert(loaded);
    assert(loaded.value().metadata.world_seed == 9003);
    assert(loaded.value().chunk_edits.size() == 1);
    assert(loaded.value().chunk_edits.front().encoded_edit_delta == "delta-d");

    write_text(root / "generations" / "generation_3" / "chunks" / "orphan.delta", "orphan");
    write_text(root / "generations" / "generation_3" / "chunks" / "note.txt", "keep");
    auto compacted_chunk_deltas = database.compact_chunk_deltas();
    assert(compacted_chunk_deltas);
    assert(compacted_chunk_deltas.value() == 1);
    assert(!std::filesystem::exists(root / "generations" / "generation_3" / "chunks" /
                                    "orphan.delta"));
    assert(std::filesystem::exists(root / "generations" / "generation_3" / "chunks" / "note.txt"));
    assert(std::filesystem::exists(root / "generations" / "generation_3" / "chunks" /
                                   "c_4_0_0.delta"));

    compacted_chunk_deltas = database.compact_chunk_deltas();
    assert(compacted_chunk_deltas);
    assert(compacted_chunk_deltas.value() == 0);

    loaded = database.read_snapshot();
    assert(loaded);
    assert(loaded.value().metadata.world_seed == 9003);
    assert(loaded.value().chunk_edits.size() == 1);
    assert(loaded.value().chunk_edits.front().encoded_edit_delta == "delta-d");

    auto fourth_snapshot = snapshot;
    fourth_snapshot.metadata.world_seed = 9004;
    fourth_snapshot.chunk_edits = {{{5, 0, 0}, "delta-e"}};
    status = database.write_snapshot(fourth_snapshot);
    assert(status);
    auto fifth_snapshot = snapshot;
    fifth_snapshot.metadata.world_seed = 9005;
    fifth_snapshot.chunk_edits = {{{6, 0, 0}, "delta-f"}};
    status = database.write_snapshot(fifth_snapshot);
    assert(status);
    std::filesystem::create_directories(root / "generations" / "generation_100.tmp");
    write_text(root / "generations" / "generation_5" / "chunks" / "orphan.delta", "orphan");

    heartstead::save::SaveDatabaseMaintenancePolicy maintenance_policy;
    maintenance_policy.recover_staged_generations = true;
    maintenance_policy.prune_stale_generations = true;
    maintenance_policy.keep_stale_generations = 1;
    maintenance_policy.compact_chunk_deltas = true;
    auto maintenance = database.maintain(maintenance_policy);
    assert(maintenance);
    assert(maintenance.value().changed());
    assert(maintenance.value().before.active_generation == "generation_5");
    assert(maintenance.value().before.committed_generation_count == 3);
    assert(maintenance.value().before.stale_generation_count == 2);
    assert(maintenance.value().before.staged_generation_count == 1);
    assert(maintenance.value().recovered_staged_generation_count == 1);
    assert(maintenance.value().pruned_stale_generation_count == 1);
    assert(maintenance.value().compacted_chunk_delta_count == 1);
    assert(maintenance.value().after.active_generation == "generation_5");
    assert(maintenance.value().after.committed_generation_count == 2);
    assert(maintenance.value().after.stale_generation_count == 1);
    assert(maintenance.value().after.staged_generation_count == 0);
    assert(!std::filesystem::exists(root / "generations" / "generation_3"));
    assert(std::filesystem::exists(root / "generations" / "generation_4"));
    assert(std::filesystem::exists(root / "generations" / "generation_5"));
    assert(!std::filesystem::exists(root / "generations" / "generation_100.tmp"));
    assert(!std::filesystem::exists(root / "generations" / "generation_5" / "chunks" /
                                    "orphan.delta"));

    auto maintenance_inspection = heartstead::debug::Inspector::inspect(maintenance.value());
    assert(maintenance_inspection.object_type == "save_database_maintenance");
    assert(maintenance_inspection.state == "changed");
    assert(maintenance_inspection.find_field("changed")->value == "true");
    assert(maintenance_inspection.find_field("recovered_staged_generation_count")->value == "1");
    assert(maintenance_inspection.find_field("pruned_stale_generation_count")->value == "1");
    assert(maintenance_inspection.find_field("compacted_chunk_delta_count")->value == "1");
    assert(maintenance_inspection.find_field("after_staged_generation_count")->value == "0");
    assert(!maintenance_inspection.has_errors());

    auto unchanged_maintenance = database.maintain(maintenance_policy);
    assert(unchanged_maintenance);
    assert(!unchanged_maintenance.value().changed());
    maintenance_inspection = heartstead::debug::Inspector::inspect(unchanged_maintenance.value());
    assert(maintenance_inspection.state == "unchanged");

    loaded = database.read_snapshot();
    assert(loaded);
    assert(loaded.value().metadata.world_seed == 9005);
    assert(loaded.value().chunk_edits.size() == 1);
    assert(loaded.value().chunk_edits.front().encoded_edit_delta == "delta-f");

    auto registry = build_snapshot_test_registry();
    const auto wall = heartstead::core::PrototypeId::parse("base:build_pieces/wall_frame");
    const auto raw_clay = heartstead::core::PrototypeId::parse("base:items/raw_clay");
    assert(wall && raw_clay);

    heartstead::save::SaveSnapshot validated_snapshot;
    validated_snapshot.metadata.game_version = "0.1.0";
    validated_snapshot.metadata.world_seed = 9100;
    validated_snapshot.metadata.enabled_mods.push_back({"base", "0.0.1", "hash"});
    heartstead::build::BuildPieceRecord wall_record;
    wall_record.object_id = heartstead::core::SaveId::from_value(910);
    wall_record.prototype_id = wall.value();
    wall_record.construction_state = heartstead::build::ConstructionState::complete;
    validated_snapshot.build_pieces.push_back(wall_record);

    const auto validated_root = make_temp_root() / "validated_save_database";
    heartstead::save::FileSaveDatabase validated_database(validated_root);
    status = validated_database.write_snapshot(validated_snapshot);
    assert(status);
    auto validated_loaded = validated_database.read_validated_snapshot(registry);
    assert(validated_loaded);
    assert(validated_loaded.value().metadata.world_seed == 9100);
    assert(validated_loaded.value().build_pieces.size() == 1);

    auto wrong_kind_snapshot = validated_snapshot;
    wrong_kind_snapshot.build_pieces.front().prototype_id = raw_clay.value();
    status = validated_database.write_snapshot(wrong_kind_snapshot);
    assert(status);
    auto raw_wrong_kind_loaded = validated_database.read_snapshot();
    assert(raw_wrong_kind_loaded);
    auto validated_wrong_kind_loaded = validated_database.read_validated_snapshot(registry);
    assert(!validated_wrong_kind_loaded);
    assert(validated_wrong_kind_loaded.error().code == "prototype_registry.kind_mismatch");
}

void test_file_save_database_safety() {
    constexpr std::string_view chunk_index_magic = "heartstead.save_database_chunks.v1";

    heartstead::save::SaveSnapshot snapshot;
    snapshot.metadata.game_version = "0.1.0";
    snapshot.metadata.world_seed = 42;
    snapshot.metadata.enabled_mods.push_back({"base", "0.0.1", "hash"});
    snapshot.chunk_edits = {{{1, 0, 0}, "first"}, {{2, 0, 0}, "second"}};

    const auto transaction_root = make_temp_root() / "transactional_chunks";
    heartstead::save::FileSaveDatabase transaction_database(transaction_root);
    assert(transaction_database.write_snapshot(snapshot));
    const auto active_chunks = transaction_root / "generations" / "generation_1" / "chunks";
    write_text(active_chunks / "orphan.delta", "not indexed");

    assert(transaction_database.write_chunk_delta({{1, 0, 0}, "updated"}));
    auto deltas = transaction_database.read_chunk_deltas();
    assert(deltas && deltas.value().size() == 2);
    assert(deltas.value()[0].coord == (heartstead::world::ChunkCoord{1, 0, 0}));
    assert(deltas.value()[0].encoded_edit_delta == "updated");
    assert(deltas.value()[1].coord == (heartstead::world::ChunkCoord{2, 0, 0}));
    assert(deltas.value()[1].encoded_edit_delta == "second");
    assert(!std::filesystem::exists(active_chunks / "orphan.delta"));

    const auto index_path = active_chunks / "index.txt";
    const auto expect_invalid_index = [&](std::string text, std::string_view error_code) {
        write_text(index_path, text);
        const auto result = transaction_database.read_chunk_deltas();
        assert(!result);
        assert(result.error().code == error_code);
    };
    expect_invalid_index(std::string(chunk_index_magic) + "\nchunk|1|0|0|renamed.delta\nend\n",
                         "save_database.noncanonical_chunk_filename");
    expect_invalid_index(std::string(chunk_index_magic) + "\nchunk|1|0|0|c_1_0_0.delta\n"
                                                          "chunk|1|0|0|different.delta\nend\n",
                         "save_database.duplicate_chunk_coordinate");
    expect_invalid_index(std::string(chunk_index_magic) + "\nchunk|1|0|0|c_1_0_0.delta\n"
                                                          "chunk|2|0|0|c_1_0_0.delta\nend\n",
                         "save_database.duplicate_chunk_filename");
    expect_invalid_index(std::string(chunk_index_magic) +
                             "\nchunk|1|0|0|c_1_0_0.delta\nend\ntrailing\n",
                         "save_database.invalid_chunk_index");
    write_text(index_path, std::string(chunk_index_magic) + "\nchunk|2|0|0|c_2_0_0.delta\n"
                                                            "chunk|1|0|0|c_1_0_0.delta\nend\n");
    deltas = transaction_database.read_chunk_deltas();
    assert(deltas && deltas.value().size() == 2);
    assert(deltas.value().front().coord == (heartstead::world::ChunkCoord{1, 0, 0}));

    write_text(active_chunks / "c_1_0_0.delta", "");
    const auto empty_delta = transaction_database.read_chunk_deltas();
    assert(!empty_delta);
    assert(empty_delta.error().code == "save_database.empty_chunk_delta");
    write_text(active_chunks / "c_1_0_0.delta", "updated");

    const auto backup_chunks = active_chunks.parent_path() / "chunks.backup";
    std::filesystem::rename(active_chunks, backup_chunks);
    deltas = transaction_database.read_chunk_deltas();
    assert(deltas && deltas.value().size() == 2);
    assert(transaction_database.write_chunk_delta({{2, 0, 0}, "recovered"}));
    assert(std::filesystem::is_directory(active_chunks));
    assert(!std::filesystem::exists(backup_chunks));
    const auto recovered = transaction_database.read_chunk_delta({2, 0, 0});
    assert(recovered && recovered.value().encoded_edit_delta == "recovered");

    const auto legacy_root = make_temp_root() / "legacy_chunks";
    auto legacy_snapshot_bytes = heartstead::save::SaveBinaryCodec::encode_snapshot(snapshot);
    assert(legacy_snapshot_bytes);
    write_bytes(legacy_root / "snapshot.hssb", legacy_snapshot_bytes.value());
    heartstead::save::FileSaveDatabase legacy_database(legacy_root);
    assert(legacy_database.write_chunk_delta({{3, 0, 0}, "third"}));
    const auto legacy_loaded = legacy_database.read_snapshot();
    assert(legacy_loaded && legacy_loaded.value().chunk_edits.size() == 3);

    const auto oversized_snapshot_root = make_temp_root() / "oversized_snapshot";
    const auto oversized_snapshot_path = oversized_snapshot_root / "snapshot.hssb";
    write_text(oversized_snapshot_path, "x");
    std::filesystem::resize_file(oversized_snapshot_path,
                                 static_cast<std::uintmax_t>(512U) * 1024U * 1024U + 1U);
    heartstead::save::FileSaveDatabase oversized_snapshot_database(oversized_snapshot_root);
    const auto oversized_snapshot = oversized_snapshot_database.read_snapshot();
    assert(!oversized_snapshot);
    assert(oversized_snapshot.error().code == "save_database.snapshot_too_large");

    const auto oversized_index_root = make_temp_root() / "oversized_index";
    const auto oversized_index_path = oversized_index_root / "chunks" / "index.txt";
    write_text(oversized_index_path, "x");
    std::filesystem::resize_file(oversized_index_path,
                                 static_cast<std::uintmax_t>(64U) * 1024U * 1024U + 1U);
    heartstead::save::FileSaveDatabase oversized_index_database(oversized_index_root);
    const auto oversized_index = oversized_index_database.read_chunk_deltas();
    assert(!oversized_index);
    assert(oversized_index.error().code == "save_database.chunk_index_too_large");

    const auto oversized_delta_root = make_temp_root() / "oversized_delta";
    const auto oversized_delta_path = oversized_delta_root / "chunks" / "c_1_0_0.delta";
    write_text(oversized_delta_root / "chunks" / "index.txt",
               std::string(chunk_index_magic) + "\nchunk|1|0|0|c_1_0_0.delta\nend\n");
    write_text(oversized_delta_path, "x");
    std::filesystem::resize_file(oversized_delta_path,
                                 static_cast<std::uintmax_t>(16U) * 1024U * 1024U + 1U);
    heartstead::save::FileSaveDatabase oversized_delta_database(oversized_delta_root);
    const auto oversized_delta = oversized_delta_database.read_chunk_deltas();
    assert(!oversized_delta);
    assert(oversized_delta.error().code == "save_database.chunk_delta_too_large");

    const auto oversized_manifest_root = make_temp_root() / "oversized_manifest";
    const auto oversized_manifest_path = oversized_manifest_root / "current.txt";
    write_text(oversized_manifest_path, "x");
    std::filesystem::resize_file(oversized_manifest_path,
                                 static_cast<std::uintmax_t>(64U) * 1024U + 1U);
    heartstead::save::FileSaveDatabase oversized_manifest_database(oversized_manifest_root);
    const auto oversized_manifest = oversized_manifest_database.stats();
    assert(!oversized_manifest);
    assert(oversized_manifest.error().code == "save_database.generation_manifest_too_large");

    const auto exhausted_root = make_temp_root() / "exhausted_generation";
    std::filesystem::create_directories(exhausted_root / "generations" /
                                        "generation_18446744073709551615");
    heartstead::save::FileSaveDatabase exhausted_database(exhausted_root);
    const auto exhausted = exhausted_database.write_snapshot(snapshot);
    assert(!exhausted);
    assert(exhausted.error().code == "save_database.generation_exhausted");
    assert(!std::filesystem::exists(exhausted_root / "generations" / "generation_0"));
}

void test_file_save_database_migration() {
    const auto root = make_temp_root() / "save_database_migration";
    heartstead::save::FileSaveDatabase database(root);

    heartstead::save::SaveSnapshot snapshot;
    snapshot.metadata.schema_version = 1;
    snapshot.metadata.game_version = "0.1.0";
    snapshot.metadata.world_seed = 7001;
    snapshot.metadata.enabled_mods.push_back({"base", "0.0.1", "hash"});
    snapshot.chunk_edits.push_back({{1, 0, 0}, "migration-delta"});

    auto status = database.write_snapshot(snapshot);
    assert(status);

    heartstead::save::SaveMigrationRegistry registry;
    heartstead::save::SaveMigrationStep add_mod_state;
    add_mod_state.id = "0001-add-db-state";
    add_mod_state.from_schema_version = 1;
    add_mod_state.to_schema_version = 2;
    add_mod_state.description = "Add database migration state";
    add_mod_state.apply = [](heartstead::save::SaveSnapshot& active_snapshot) {
        active_snapshot.mod_states.push_back({"base", "database_migration", "created"});
        return heartstead::core::Status::ok();
    };
    heartstead::save::SaveMigrationStep rewrite_mod_state;
    rewrite_mod_state.id = "0002-rewrite-db-state";
    rewrite_mod_state.from_schema_version = 2;
    rewrite_mod_state.to_schema_version = 3;
    rewrite_mod_state.description = "Rewrite database migration state";
    rewrite_mod_state.apply = [](heartstead::save::SaveSnapshot& active_snapshot) {
        if (active_snapshot.mod_states.empty()) {
            return heartstead::core::Status::failure("test.missing_state",
                                                     "expected database migration state");
        }
        active_snapshot.mod_states.front().encoded_state = "rewritten";
        return heartstead::core::Status::ok();
    };
    assert(registry.register_migration(add_mod_state));
    assert(registry.register_migration(rewrite_mod_state));

    auto migrated = database.migrate_to_schema(registry, 3);
    assert(migrated);
    assert(migrated.value().changed());
    assert(migrated.value().wrote_snapshot);
    assert(migrated.value().migration.previous_schema_version == 1);
    assert(migrated.value().migration.final_schema_version == 3);
    assert(migrated.value().migration.applied_migrations.size() == 2);
    assert(migrated.value().before.active_generation == "generation_1");
    assert(migrated.value().after.active_generation == "generation_2");
    assert(migrated.value().after.committed_generation_count == 2);
    assert(migrated.value().after.stale_generation_count == 1);

    auto migration_inspection = heartstead::debug::Inspector::inspect(migrated.value());
    assert(migration_inspection.object_type == "save_database_migration");
    assert(migration_inspection.state == "migrated");
    assert(migration_inspection.find_field("changed")->value == "true");
    assert(migration_inspection.find_field("wrote_snapshot")->value == "true");
    assert(migration_inspection.find_field("previous_schema_version")->value == "1");
    assert(migration_inspection.find_field("final_schema_version")->value == "3");
    assert(migration_inspection.find_field("applied_migration_count")->value == "2");
    assert(!migration_inspection.has_errors());

    auto loaded = database.read_snapshot();
    assert(loaded);
    assert(loaded.value().metadata.schema_version == 3);
    assert(loaded.value().metadata.migration_history.size() == 2);
    assert(heartstead::save::has_migration_history_entry(loaded.value().metadata,
                                                         "0002-rewrite-db-state"));
    assert(loaded.value().mod_states.size() == 1);
    assert(loaded.value().mod_states.front().encoded_state == "rewritten");
    assert(loaded.value().chunk_edits.size() == 1);
    assert(loaded.value().chunk_edits.front().encoded_edit_delta == "migration-delta");

    auto no_op = database.migrate_to_schema(registry, 3);
    assert(no_op);
    assert(!no_op.value().changed());
    assert(!no_op.value().wrote_snapshot);
    assert(no_op.value().migration.applied_migrations.empty());
    assert(no_op.value().before.active_generation == "generation_2");
    assert(no_op.value().after.active_generation == "generation_2");
    migration_inspection = heartstead::debug::Inspector::inspect(no_op.value());
    assert(migration_inspection.state == "unchanged");

    auto downgrade = database.migrate_to_schema(registry, 2);
    assert(!downgrade);
    assert(downgrade.error().code == "save_migration.downgrade_unsupported");
    loaded = database.read_snapshot();
    assert(loaded);
    assert(loaded.value().metadata.schema_version == 3);
}

void test_file_save_slot_catalog() {
    const auto root = make_temp_root() / "save_slots";
    heartstead::save::FileSaveSlotCatalog catalog(root);

    auto listed = catalog.list_slots();
    assert(listed);
    assert(listed.value().empty());
    auto empty_catalog_summary = catalog.summary();
    assert(empty_catalog_summary);
    auto empty_catalog_inspection =
        heartstead::debug::Inspector::inspect(empty_catalog_summary.value());
    assert(empty_catalog_inspection.object_type == "save_slot_catalog");
    assert(empty_catalog_inspection.state == "empty");
    assert(empty_catalog_inspection.find_field("slot_count")->value == "0");
    assert(!empty_catalog_inspection.issues.empty());

    auto status = catalog.create_slot("BadSlot");
    assert(!status);
    assert(status.error().code == "save_slot.invalid_id");
    auto invalid_database = catalog.database("../bad");
    assert(!invalid_database);
    assert(invalid_database.error().code == "save_slot.invalid_id");
    auto missing_database = catalog.database("missing");
    assert(!missing_database);
    assert(missing_database.error().code == "save_slot.not_found");
    auto missing_metadata = catalog.read_metadata("missing");
    assert(!missing_metadata);
    assert(missing_metadata.error().code == "save_slot.not_found");

    status = catalog.create_slot("settlement_a");
    assert(status);
    status = catalog.create_slot("winter-2");
    assert(status);

    write_text(root / "winter-2" / "slot.txt", std::string(64U * 1024U + 1U, 'x'));
    auto oversized_slot_metadata = catalog.read_metadata("winter-2");
    assert(!oversized_slot_metadata);
    assert(oversized_slot_metadata.error().code == "save_slot.metadata_too_large");
    write_text(root / "winter-2" / "slot.txt",
               "heartstead.save_slot.v1\nslot_id|winter-2\n"
               "display_name|Winter 2\ncreated_at_ms|0\nlast_saved_at_ms|0\nend\n"
               "unexpected|data\n");
    auto trailing_slot_metadata = catalog.read_metadata("winter-2");
    assert(!trailing_slot_metadata);
    assert(trailing_slot_metadata.error().code == "save_slot.trailing_data");
    write_text(root / "winter-2" / "slot.txt",
               "heartstead.save_slot.v1\nslot_id|winter-2\n"
               "display_name|Winter 2\ncreated_at_ms|10\nlast_saved_at_ms|20\nend\n");
    auto legacy_slot_metadata = catalog.read_metadata("winter-2");
    assert(legacy_slot_metadata);
    assert(legacy_slot_metadata.value().last_played_at_ms == 20);

    auto settlement_metadata = catalog.read_metadata("settlement_a");
    assert(settlement_metadata);
    assert(settlement_metadata.value().slot_id == "settlement_a");
    assert(settlement_metadata.value().display_name == "settlement_a");

    heartstead::save::SaveSlotMetadata renamed_metadata;
    renamed_metadata.slot_id = "settlement_a";
    renamed_metadata.display_name = "Settlement A";
    renamed_metadata.created_at_ms = 100;
    renamed_metadata.last_saved_at_ms = 250;
    status = catalog.write_metadata(renamed_metadata);
    assert(status);

    heartstead::save::SaveSlotMetadata invalid_name_metadata = renamed_metadata;
    invalid_name_metadata.display_name.clear();
    status = catalog.write_metadata(invalid_name_metadata);
    assert(!status);
    assert(status.error().code == "save_slot.invalid_display_name");

    heartstead::save::SaveSlotMetadata invalid_time_metadata = renamed_metadata;
    invalid_time_metadata.created_at_ms = 500;
    invalid_time_metadata.last_saved_at_ms = 250;
    status = catalog.write_metadata(invalid_time_metadata);
    assert(!status);
    assert(status.error().code == "save_slot.invalid_timestamps");

    std::filesystem::remove(root / "winter-2" / "slot.txt");
    auto fallback_metadata = catalog.read_metadata("winter-2");
    assert(fallback_metadata);
    assert(fallback_metadata.value().slot_id == "winter-2");
    assert(fallback_metadata.value().display_name == "winter-2");

    const auto outside_metadata = make_temp_root() / "outside-slot-metadata.txt";
    write_text(outside_metadata, "outside");
    std::filesystem::create_symlink(outside_metadata, root / "winter-2" / "slot.txt");
    auto symlinked_metadata = catalog.read_metadata("winter-2");
    assert(!symlinked_metadata);
    assert(symlinked_metadata.error().code == "save_slot.unsafe_symlink");
    std::filesystem::remove(root / "winter-2" / "slot.txt");

    const auto outside_slot = make_temp_root() / "outside-slot";
    std::filesystem::create_directories(outside_slot);
    std::filesystem::create_directory_symlink(outside_slot, root / "linked");
    auto symlinked_database = catalog.database("linked");
    assert(!symlinked_database);
    assert(symlinked_database.error().code == "save_slot.unsafe_symlink");
    auto symlinked_slots = catalog.list_slots();
    assert(!symlinked_slots);
    assert(symlinked_slots.error().code == "save_slot.unsafe_symlink");
    std::filesystem::remove(root / "linked");

    std::filesystem::create_directories(root / "UpperCase");
    write_text(root / "notes.txt", "not a save slot");

    auto slot_database = catalog.database("settlement_a");
    assert(slot_database);

    heartstead::save::SaveSnapshot snapshot;
    snapshot.metadata.game_version = "0.1.0";
    snapshot.metadata.world_seed = 42;
    snapshot.metadata.enabled_mods.push_back({"base", "0.0.1", "hash"});
    snapshot.chunk_edits.push_back({{1, 0, 0}, "slot-delta"});

    status = catalog.write_snapshot("settlement_a", snapshot, 0);
    assert(!status);
    assert(status.error().code == "save_slot.invalid_timestamps");
    status = catalog.write_snapshot("settlement_a", snapshot, 50);
    assert(status);
    auto clock_adjusted_metadata = catalog.read_metadata("settlement_a");
    assert(clock_adjusted_metadata);
    assert(clock_adjusted_metadata.value().created_at_ms == 100);
    assert(clock_adjusted_metadata.value().last_saved_at_ms == 250);
    status = catalog.write_snapshot("settlement_a", snapshot, 400);
    assert(status);
    status = catalog.mark_played("settlement_a", 350);
    assert(status);
    status = catalog.mark_played("settlement_a", 150);
    assert(status);
    status = catalog.mark_played("settlement_a", 0);
    assert(!status && status.error().code == "save_slot.invalid_timestamps");

    listed = catalog.list_slots();
    assert(listed);
    assert(listed.value().size() == 2);
    assert(listed.value()[0].slot_id == "settlement_a");
    assert(listed.value()[1].slot_id == "winter-2");
    assert(listed.value()[0].metadata.display_name == "Settlement A");
    assert(listed.value()[0].metadata.created_at_ms == 100);
    assert(listed.value()[0].metadata.last_played_at_ms == 350);
    assert(listed.value()[0].metadata.last_saved_at_ms == 400);
    assert(listed.value()[1].metadata.display_name == "winter-2");
    assert(listed.value()[0].database_stats.has_snapshot);
    assert(listed.value()[0].database_stats.uses_generation_manifest);
    assert(listed.value()[0].database_stats.active_generation == "generation_2");
    assert(listed.value()[0].database_stats.chunk_delta_count == 1);
    assert(!listed.value()[1].database_stats.has_snapshot);

    auto catalog_summary = catalog.summary();
    assert(catalog_summary);
    assert(catalog_summary.value().root == root);
    assert(catalog_summary.value().slots.size() == 2);
    auto catalog_inspection = heartstead::debug::Inspector::inspect(catalog_summary.value());
    assert(catalog_inspection.object_type == "save_slot_catalog");
    assert(catalog_inspection.state == "loaded");
    assert(catalog_inspection.find_field("slot_count")->value == "2");
    assert(catalog_inspection.find_field("empty_slot_count")->value == "1");
    assert(catalog_inspection.find_field("generation_slot_count")->value == "1");
    assert(catalog_inspection.find_field("legacy_slot_count")->value == "0");
    assert(catalog_inspection.find_field("invalid_slot_count")->value == "0");
    assert(catalog_inspection.find_field("chunk_delta_count")->value == "1");
    assert(!catalog_inspection.has_errors());

    auto slot_inspection = heartstead::debug::Inspector::inspect(listed.value()[0]);
    assert(slot_inspection.object_type == "save_slot");
    assert(slot_inspection.state == "generation");
    assert(slot_inspection.display_name == "Settlement A");
    assert(slot_inspection.find_field("slot_id")->value == "settlement_a");
    assert(slot_inspection.find_field("display_name")->value == "Settlement A");
    assert(slot_inspection.find_field("created_at_ms")->value == "100");
    assert(slot_inspection.find_field("last_played_at_ms")->value == "350");
    assert(slot_inspection.find_field("last_saved_at_ms")->value == "400");
    assert(slot_inspection.find_field("layout")->value == "generation");
    assert(slot_inspection.find_field("chunk_delta_count")->value == "1");
    assert(!slot_inspection.has_errors());

    auto empty_slot_inspection = heartstead::debug::Inspector::inspect(listed.value()[1]);
    assert(empty_slot_inspection.state == "empty");
    assert(!empty_slot_inspection.issues.empty());

    auto loaded = slot_database.value().read_snapshot();
    assert(loaded);
    assert(loaded.value().metadata.world_seed == 42);
    assert(loaded.value().chunk_edits.size() == 1);
    assert(loaded.value().chunk_edits.front().encoded_edit_delta == "slot-delta");

    write_text(root / "winter-2" / "slot.txt",
               "heartstead.save_slot.v1\nslot_id|other\n"
               "display_name|Other\ncreated_at_ms|0\nlast_saved_at_ms|0\nend\n");
    auto mismatched_metadata_slots = catalog.list_slots();
    assert(mismatched_metadata_slots);
    assert(mismatched_metadata_slots.value().size() == 2);
    const auto mismatched_slot = std::ranges::find(mismatched_metadata_slots.value(), "winter-2",
                                                   &heartstead::save::SaveSlotSummary::slot_id);
    assert(mismatched_slot != mismatched_metadata_slots.value().end());
    assert(mismatched_slot->validation_error.has_value());
    assert(mismatched_slot->validation_error->code == "save_slot.metadata_mismatch");
    auto mismatched_metadata_summary = catalog.summary();
    assert(mismatched_metadata_summary);
    auto mismatched_catalog_inspection =
        heartstead::debug::Inspector::inspect(mismatched_metadata_summary.value());
    assert(mismatched_catalog_inspection.state == "invalid");
    assert(mismatched_catalog_inspection.find_field("invalid_slot_count")->value == "1");
    assert(mismatched_catalog_inspection.has_errors());
}

void test_debug_inspection() {
    auto platform_inspection =
        heartstead::debug::Inspector::inspect(heartstead::platform::platform_backend_capabilities(
            heartstead::platform::PlatformBackend::native));
    assert(platform_inspection.object_type == "platform_backend_capabilities");
    const auto inspected_native_info =
        heartstead::platform::platform_backend_info(heartstead::platform::PlatformBackend::native);
    assert(platform_inspection.state ==
           std::string(inspected_native_info.available ? "available" : "unavailable"));
    assert(platform_inspection.issues.empty() == inspected_native_info.available);
    const auto* platform_window_system = platform_inspection.find_field("window_system");
    assert(platform_window_system != nullptr);
    assert(platform_window_system->value == "x11" ||
           platform_window_system->value == "sdl3_or_equivalent");
    const auto* platform_mouse_input = platform_inspection.find_field("supports_mouse_input");
    assert(platform_mouse_input != nullptr);
    assert(platform_mouse_input->value == "true" || platform_mouse_input->value == "false");
    const auto* platform_display_metadata =
        platform_inspection.find_field("supports_display_metadata");
    assert(platform_display_metadata != nullptr);
    assert(platform_display_metadata->value == "true" ||
           platform_display_metadata->value == "false");
    const auto* platform_clipboard = platform_inspection.find_field("supports_clipboard");
    assert(platform_clipboard != nullptr);
    assert(platform_clipboard->value == "true" || platform_clipboard->value == "false");

    auto renderer_inspection = heartstead::debug::Inspector::inspect(
        heartstead::renderer::rhi::render_backend_capabilities(
            heartstead::renderer::rhi::RenderBackend::vulkan));
    assert(renderer_inspection.object_type == "render_backend_capabilities");
    const auto inspected_vulkan_info = heartstead::renderer::rhi::renderer_backend_info(
        heartstead::renderer::rhi::RenderBackend::vulkan);
    assert(renderer_inspection.state ==
           std::string(inspected_vulkan_info.available ? "available" : "unavailable"));
    const auto* renderer_api = renderer_inspection.find_field("graphics_api");
    assert(renderer_api != nullptr);
    assert(renderer_api->value == "vulkan");
    const auto* renderer_upload = renderer_inspection.find_field("supports_buffer_upload");
    assert(renderer_upload != nullptr);
    assert(renderer_upload->value == "true");
    const auto* renderer_image_upload = renderer_inspection.find_field("supports_image_upload");
    assert(renderer_image_upload != nullptr);
    assert(renderer_image_upload->value == "true");
    const auto* renderer_shader_modules = renderer_inspection.find_field("supports_shader_modules");
    assert(renderer_shader_modules != nullptr);
    assert(renderer_shader_modules->value == "true");
    const auto* renderer_pipeline = renderer_inspection.find_field("supports_pipeline_layout");
    assert(renderer_pipeline != nullptr);
    assert(renderer_pipeline->value == "true");
    const auto* renderer_compute_pipeline =
        renderer_inspection.find_field("supports_compute_pipelines");
    assert(renderer_compute_pipeline != nullptr);
    assert(renderer_compute_pipeline->value == "true");
    const auto* renderer_graphics_pipeline =
        renderer_inspection.find_field("supports_graphics_pipelines");
    assert(renderer_graphics_pipeline != nullptr);
    assert(renderer_graphics_pipeline->value == "true");
    const auto* renderer_descriptor_writes =
        renderer_inspection.find_field("supports_descriptor_writes");
    assert(renderer_descriptor_writes != nullptr);
    assert(renderer_descriptor_writes->value == "true");
    const auto* renderer_draw_binding = renderer_inspection.find_field("supports_draw_binding");
    assert(renderer_draw_binding != nullptr);
    assert(renderer_draw_binding->value == "true");

    auto physics_inspection =
        heartstead::debug::Inspector::inspect(heartstead::physics::physics_backend_capabilities(
            heartstead::physics::PhysicsBackend::jolt));
    assert(physics_inspection.object_type == "physics_backend_capabilities");
    const auto jolt_available =
        heartstead::physics::physics_backend_info(heartstead::physics::PhysicsBackend::jolt)
            .available;
    assert(physics_inspection.state == (jolt_available ? "available" : "unavailable"));
    const auto* collision_response = physics_inspection.find_field("supports_collision_response");
    assert(collision_response != nullptr);
    assert(collision_response->value == "true");

    auto asset_backend_inspection =
        heartstead::debug::Inspector::inspect(heartstead::assets::asset_cook_backend_info(
            heartstead::assets::AssetCookBackend::production_converters));
    assert(asset_backend_inspection.object_type == "asset_cook_backend");
    assert(asset_backend_inspection.state == "available");
    assert(asset_backend_inspection.issues.empty());

    auto asset_model_pipeline_inspection =
        heartstead::debug::Inspector::inspect(heartstead::assets::asset_cook_pipeline_info(
            heartstead::assets::AssetKind::model,
            heartstead::assets::AssetCookBackend::production_converters));
    assert(asset_model_pipeline_inspection.object_type == "asset_cook_pipeline");
    assert(asset_model_pipeline_inspection.state == "available");
    assert(asset_model_pipeline_inspection.find_field("pipeline")->value ==
           "model_gltf_runtime_converter_v5");
    assert(asset_model_pipeline_inspection.find_field("converts_source_format")->value == "true");
    assert(asset_model_pipeline_inspection.issues.empty());

    auto asset_pipeline_inspection =
        heartstead::debug::Inspector::inspect(heartstead::assets::asset_cook_pipeline_info(
            heartstead::assets::AssetKind::texture,
            heartstead::assets::AssetCookBackend::production_converters));
    assert(asset_pipeline_inspection.object_type == "asset_cook_pipeline");
    assert(asset_pipeline_inspection.state == "available");
    assert(asset_pipeline_inspection.find_field("pipeline")->value ==
           "texture_gpu_runtime_converter_v3");
    assert(asset_pipeline_inspection.find_field("converts_source_format")->value == "true");
    assert(asset_pipeline_inspection.issues.empty());
    auto asset_shader_pipeline_inspection =
        heartstead::debug::Inspector::inspect(heartstead::assets::asset_cook_pipeline_info(
            heartstead::assets::AssetKind::shader,
            heartstead::assets::AssetCookBackend::production_converters));
    assert(asset_shader_pipeline_inspection.object_type == "asset_cook_pipeline");
    assert(asset_shader_pipeline_inspection.state == "available");
    assert(asset_shader_pipeline_inspection.find_field("pipeline")->value ==
           "shader_spirv_runtime_passthrough_v1");
    assert(asset_shader_pipeline_inspection.find_field("converts_source_format")->value == "true");
    assert(asset_shader_pipeline_inspection.issues.empty());
    auto asset_audio_pipeline_inspection =
        heartstead::debug::Inspector::inspect(heartstead::assets::asset_cook_pipeline_info(
            heartstead::assets::AssetKind::sound,
            heartstead::assets::AssetCookBackend::production_converters));
    assert(asset_audio_pipeline_inspection.object_type == "asset_cook_pipeline");
    assert(asset_audio_pipeline_inspection.state == "available");
    assert(asset_audio_pipeline_inspection.find_field("pipeline")->value ==
           "audio_runtime_converter_v3");
    assert(asset_audio_pipeline_inspection.find_field("converts_source_format")->value == "true");
    assert(asset_audio_pipeline_inspection.issues.empty());
    auto asset_font_pipeline_inspection =
        heartstead::debug::Inspector::inspect(heartstead::assets::asset_cook_pipeline_info(
            heartstead::assets::AssetKind::font,
            heartstead::assets::AssetCookBackend::production_converters));
    assert(asset_font_pipeline_inspection.object_type == "asset_cook_pipeline");
    assert(asset_font_pipeline_inspection.state == "available");
    assert(asset_font_pipeline_inspection.find_field("pipeline")->value ==
           "font_runtime_converter_v1");
    assert(asset_font_pipeline_inspection.find_field("converts_source_format")->value == "true");
    assert(asset_font_pipeline_inspection.issues.empty());

    const auto asset_inspection_root = make_temp_root();
    const auto debug_mod_assets = asset_inspection_root / "mods/base/assets";
    const auto debug_pack_assets = asset_inspection_root / "resource_packs/hd_pack/assets";
    write_text(debug_mod_assets / "textures/items/raw_clay.txt", "mod texture");
    write_text(debug_mod_assets / "models/building/wall.glb", "model placeholder");
    write_text(debug_pack_assets / "textures/items/raw_clay.txt", "resource pack texture");

    heartstead::assets::VirtualFileSystem inspected_vfs;
    auto inspected_mount = inspected_vfs.mount("base", debug_mod_assets);
    assert(inspected_mount);
    inspected_mount = inspected_vfs.mount("base", debug_pack_assets);
    assert(inspected_mount);

    auto vfs_inspection = heartstead::debug::Inspector::inspect(inspected_vfs);
    assert(vfs_inspection.object_type == "virtual_file_system");
    assert(vfs_inspection.state == "mounted");
    assert(vfs_inspection.find_field("mount_count")->value == "2");
    assert(vfs_inspection.find_field("namespace_count")->value == "1");
    assert(vfs_inspection.find_field("namespaces")->value == "base");

    auto listed_vfs_assets = inspected_vfs.list_files("base:textures");
    assert(listed_vfs_assets);
    assert(!listed_vfs_assets.value().empty());
    const auto listed_override =
        std::ranges::find_if(listed_vfs_assets.value(), [](const auto& entry) {
            return entry.virtual_path.to_string() == "base:textures/items/raw_clay.txt";
        });
    assert(listed_override != listed_vfs_assets.value().end());
    auto vfs_entry_inspection = heartstead::debug::Inspector::inspect(*listed_override);
    assert(vfs_entry_inspection.object_type == "vfs_file_entry");
    assert(vfs_entry_inspection.find_field("namespace_id")->value == "base");
    assert(vfs_entry_inspection.find_field("mount_index")->value == "1");

    heartstead::assets::AssetCatalog inspected_catalog;
    auto inspected_mod_index = heartstead::assets::AssetCatalogBuilder::index_directory(
        inspected_catalog, debug_mod_assets, "base", heartstead::assets::AssetSourceKind::mod,
        "base", 0);
    auto inspected_pack_index = heartstead::assets::AssetCatalogBuilder::index_directory(
        inspected_catalog, debug_pack_assets, "base",
        heartstead::assets::AssetSourceKind::resource_pack, "hd_pack", 1000);
    assert(!inspected_mod_index.has_errors());
    assert(!inspected_pack_index.has_errors());

    auto catalog_inspection = heartstead::debug::Inspector::inspect(inspected_catalog);
    assert(catalog_inspection.object_type == "asset_catalog");
    assert(catalog_inspection.state == "overridden");
    assert(catalog_inspection.find_field("record_count")->value == "3");
    assert(catalog_inspection.find_field("active_count")->value == "2");
    assert(catalog_inspection.find_field("override_logical_id_count")->value == "1");
    assert(catalog_inspection.find_field("texture_count")->value == "2");
    assert(catalog_inspection.find_field("model_count")->value == "1");

    heartstead::assets::ResourcePackManifest inspected_pack;
    inspected_pack.id = "hd_pack";
    inspected_pack.name = "HD Pack";
    inspected_pack.version = "1.0.0";
    inspected_pack.root = asset_inspection_root / "resource_packs/hd_pack";
    auto inspected_pack_plan = heartstead::assets::ResourcePackLoadPlanner::plan({inspected_pack});
    assert(inspected_pack_plan);
    auto pack_plan_inspection = heartstead::debug::Inspector::inspect(inspected_pack_plan.value());
    assert(pack_plan_inspection.object_type == "resource_pack_load_plan");
    assert(pack_plan_inspection.state == "planned");
    assert(pack_plan_inspection.find_field("pack_count")->value == "1");
    assert(pack_plan_inspection.find_field("first_pack_id")->value == "hd_pack");
    assert(pack_plan_inspection.find_field("first_pack_priority")->value == "1000");
    assert(pack_plan_inspection.find_field("priority_order_valid")->value == "true");
    assert(pack_plan_inspection.issues.empty());

    const auto* inspected_asset = inspected_catalog.find_active("base:textures/items/raw_clay.txt");
    assert(inspected_asset != nullptr);
    auto asset_record_inspection = heartstead::debug::Inspector::inspect(*inspected_asset);
    assert(asset_record_inspection.object_type == "asset_record");
    assert(asset_record_inspection.state == "source");
    assert(asset_record_inspection.find_field("logical_id")->value ==
           "base:textures/items/raw_clay.txt");
    assert(asset_record_inspection.find_field("source_kind")->value == "resource_pack");
    assert(asset_record_inspection.find_field("source_id")->value == "hd_pack");
    assert(asset_record_inspection.find_field("content_hash")->value.size() == 16);

    auto inspected_cooked_manifest =
        heartstead::assets::CookedAssetManifestBuilder::build(inspected_catalog);
    assert(inspected_cooked_manifest);
    auto cooked_manifest_inspection =
        heartstead::debug::Inspector::inspect(inspected_cooked_manifest.value());
    assert(cooked_manifest_inspection.object_type == "cooked_asset_manifest");
    assert(cooked_manifest_inspection.state == "ready");
    assert(cooked_manifest_inspection.find_field("profile")->value == "development");
    assert(cooked_manifest_inspection.find_field("record_count")->value == "2");
    assert(cooked_manifest_inspection.find_field("active_count")->value == "2");
    assert(cooked_manifest_inspection.find_field("texture_count")->value == "1");
    assert(cooked_manifest_inspection.find_field("model_count")->value == "1");
    assert(cooked_manifest_inspection.find_field("dependency_issue_count")->value == "0");

    const auto* inspected_cooked_asset =
        inspected_cooked_manifest.value().find("base:textures/items/raw_clay.txt");
    assert(inspected_cooked_asset != nullptr);
    auto cooked_record_inspection = heartstead::debug::Inspector::inspect(*inspected_cooked_asset);
    assert(cooked_record_inspection.object_type == "cooked_asset_record");
    assert(cooked_record_inspection.state == "active");
    assert(cooked_record_inspection.find_field("source_id")->value == "hd_pack");
    assert(cooked_record_inspection.find_field("cooked_hash")->value.size() == 16);
    assert(cooked_record_inspection.find_field("cooked_relative_path")->value ==
           "hd_pack/development/texture/base/textures/items/raw_clay.txt.cooked");

    auto invalid_cooked_record = *inspected_cooked_asset;
    invalid_cooked_record.cooked_hash.clear();
    auto invalid_cooked_record_inspection =
        heartstead::debug::Inspector::inspect(invalid_cooked_record);
    assert(invalid_cooked_record_inspection.state == "invalid");
    assert(invalid_cooked_record_inspection.has_errors());
    assert(invalid_cooked_record_inspection.issues.front().code ==
           "cooked_asset_record.missing_cooked_hash");

    auto unresolved_dependency_manifest = inspected_cooked_manifest.value();
    auto missing_dependency =
        heartstead::assets::VirtualPath::parse("base:textures/items/missing_icon.txt");
    assert(missing_dependency);
    unresolved_dependency_manifest.records.front().dependencies.push_back(
        missing_dependency.value());
    auto unresolved_dependency_inspection =
        heartstead::debug::Inspector::inspect(unresolved_dependency_manifest);
    assert(unresolved_dependency_inspection.state == "invalid");
    assert(unresolved_dependency_inspection.find_field("dependency_issue_count")->value == "1");
    assert(unresolved_dependency_inspection.find_field("first_dependency_issue_path")->value ==
           "base:textures/items/missing_icon.txt");

    heartstead::assets::AssetCookConfig inspected_cook_config;
    inspected_cook_config.output_root = asset_inspection_root / "cooked_debug_assets";
    auto inspected_cook_result =
        heartstead::assets::AssetCooker::cook(inspected_catalog, inspected_cook_config);
    assert(inspected_cook_result);
    auto inspected_store =
        heartstead::assets::CookedAssetStore::load(inspected_cook_config.output_root);
    assert(inspected_store);
    auto cooked_store_inspection = heartstead::debug::Inspector::inspect(inspected_store.value());
    assert(cooked_store_inspection.object_type == "cooked_asset_store");
    assert(cooked_store_inspection.state == "loaded");
    assert(cooked_store_inspection.find_field("root")->value ==
           inspected_cook_config.output_root.generic_string());
    assert(cooked_store_inspection.find_field("manifest_record_count")->value == "2");
    assert(cooked_store_inspection.find_field("manifest_active_count")->value == "2");

    const auto inspected_script_backend =
        heartstead::scripting::script_backend_info(heartstead::scripting::ScriptBackend::luau);
    auto script_backend_inspection =
        heartstead::debug::Inspector::inspect(inspected_script_backend);
    assert(script_backend_inspection.object_type == "script_backend");
    assert(script_backend_inspection.state ==
           (inspected_script_backend.available ? "available" : "unavailable"));
    assert(script_backend_inspection.find_field("backend")->value == "luau");

    heartstead::scripting::ScriptModuleDesc script_module;
    script_module.module_id = "base:scripts/runtime_server/debug_tick";
    script_module.source_mod_id = "base";
    script_module.source_path = "mods/base/scripts/runtime_server/debug_tick.luau";
    script_module.source = "return { tick = function() return true end }";
    script_module.stage = heartstead::scripting::ScriptStage::runtime_server;
    script_module.permissions = {heartstead::scripting::ScriptPermission::read_prototypes,
                                 heartstead::scripting::ScriptPermission::emit_commands};
    auto script_module_inspection = heartstead::debug::Inspector::inspect(script_module);
    assert(script_module_inspection.object_type == "script_module");
    assert(script_module_inspection.state == "runtime_server");
    assert(script_module_inspection.runtime_id == script_module.module_id);
    assert(script_module_inspection.find_field("permission_count")->value == "2");
    assert(script_module_inspection.find_field("permissions")->value ==
           "read_prototypes,emit_commands");
    assert(script_module_inspection.issues.empty());

    heartstead::scripting::ScriptHostApiDesc script_host_api;
    script_host_api.api_id = "world.set_voxel";
    script_host_api.stage = heartstead::scripting::ScriptStage::runtime_server;
    script_host_api.min_module_api_version = 1;
    script_host_api.required_permissions = {heartstead::scripting::ScriptPermission::emit_commands};
    script_host_api.arguments = {
        heartstead::scripting::ScriptHostApiArgument{
            "chunk", heartstead::scripting::ScriptValueKind::string, false},
        heartstead::scripting::ScriptHostApiArgument{
            "voxel", heartstead::scripting::ScriptValueKind::string, false},
        heartstead::scripting::ScriptHostApiArgument{
            "cell", heartstead::scripting::ScriptValueKind::string, false},
    };
    auto script_host_api_inspection = heartstead::debug::Inspector::inspect(script_host_api);
    assert(script_host_api_inspection.object_type == "script_host_api");
    assert(script_host_api_inspection.state == "runtime_server");
    assert(script_host_api_inspection.runtime_id == "world.set_voxel");
    assert(script_host_api_inspection.find_field("required_permission_count")->value == "1");
    assert(script_host_api_inspection.find_field("required_permissions")->value == "emit_commands");
    assert(script_host_api_inspection.find_field("argument_count")->value == "3");
    assert(script_host_api_inspection.find_field("arguments")->value ==
           "chunk:string,voxel:string,cell:string");
    assert(script_host_api_inspection.issues.empty());

    heartstead::scripting::ScriptHostEvent script_host_event;
    script_host_event.sequence = 11;
    script_host_event.api_id = "world.set_voxel";
    script_host_event.module_id = script_module.module_id;
    script_host_event.source_mod_id = script_module.source_mod_id;
    script_host_event.source_path = script_module.source_path;
    script_host_event.stage = heartstead::scripting::ScriptStage::runtime_server;
    script_host_event.function_name = "tick";
    script_host_event.module_api_version = script_module.api_version;
    script_host_event.consumed_instruction_estimate = 12;
    script_host_event.arguments = {heartstead::scripting::ScriptValue::string("payload")};
    auto script_host_event_inspection = heartstead::debug::Inspector::inspect(script_host_event);
    assert(script_host_event_inspection.object_type == "script_host_event");
    assert(script_host_event_inspection.state == "runtime_server");
    assert(script_host_event_inspection.runtime_id == "11");
    assert(script_host_event_inspection.find_field("api_id")->value == "world.set_voxel");
    assert(script_host_event_inspection.find_field("argument_count")->value == "1");
    assert(script_host_event_inspection.issues.empty());

    heartstead::scripting::ScriptHostEventBatch script_host_event_batch;
    script_host_event_batch.first_sequence = 11;
    script_host_event_batch.last_sequence = 11;
    script_host_event_batch.events = {script_host_event};
    auto script_host_event_batch_inspection =
        heartstead::debug::Inspector::inspect(script_host_event_batch);
    assert(script_host_event_batch_inspection.object_type == "script_host_event_batch");
    assert(script_host_event_batch_inspection.state == "queued");
    assert(script_host_event_batch_inspection.find_field("event_count")->value == "1");
    assert(script_host_event_batch_inspection.issues.empty());

    heartstead::simulation::SimulationLodPolicy lod_policy;
    lod_policy.full_radius = 16;
    lod_policy.simplified_radius = 64;
    lod_policy.full_tick_interval_ms = 16;
    lod_policy.simplified_tick_interval_ms = 1000;
    lod_policy.sleeping_tick_interval_ms = 5000;
    auto lod_policy_inspection = heartstead::debug::Inspector::inspect(lod_policy);
    assert(lod_policy_inspection.object_type == "simulation_lod_policy");
    assert(lod_policy_inspection.state == "valid");
    assert(lod_policy_inspection.find_field("full_radius")->value == "16");
    assert(lod_policy_inspection.find_field("simplified_radius")->value == "64");
    assert(lod_policy_inspection.issues.empty());

    const auto subject_prototype = heartstead::core::PrototypeId::parse("base:entities/hand_cart");
    assert(subject_prototype);
    heartstead::simulation::SimulationSubject lod_subject;
    lod_subject.save_id = heartstead::core::SaveId::from_value(43);
    lod_subject.runtime_handle = heartstead::core::RuntimeHandle::from_value(100);
    lod_subject.prototype_id = subject_prototype.value();
    lod_subject.kind = heartstead::simulation::SimulationSubjectKind::entity;
    lod_subject.coord = {12, 0, -4};
    lod_subject.last_update_time_ms = 900;
    lod_subject.sleeping = true;
    lod_subject.label = "cart subject";
    auto lod_subject_inspection = heartstead::debug::Inspector::inspect(lod_subject);
    assert(lod_subject_inspection.object_type == "simulation_subject");
    assert(lod_subject_inspection.display_name == "cart subject");
    assert(lod_subject_inspection.prototype_id == "base:entities/hand_cart");
    assert(lod_subject_inspection.save_id == "43");
    assert(lod_subject_inspection.runtime_id == "100");
    assert(lod_subject_inspection.state == "sleeping");
    assert(lod_subject_inspection.find_field("kind")->value == "entity");
    assert(lod_subject_inspection.find_field("process_id")->value.empty());
    assert(lod_subject_inspection.find_field("coord_x")->value == "12");
    assert(lod_subject_inspection.find_field("coord_z")->value == "-4");
    assert(lod_subject_inspection.find_field("last_update_time_ms")->value == "900");
    assert(lod_subject_inspection.find_field("persistent")->value == "true");
    assert(lod_subject_inspection.find_field("sleeping")->value == "true");
    assert(lod_subject_inspection.find_field("forced_lod")->value.empty());
    assert(lod_subject_inspection.issues.empty());

    lod_subject.forced_lod = heartstead::simulation::SimulationLod::simplified;
    auto forced_lod_subject_inspection = heartstead::debug::Inspector::inspect(lod_subject);
    assert(forced_lod_subject_inspection.state == "forced_simplified");
    assert(forced_lod_subject_inspection.find_field("forced_lod")->value == "simplified");

    lod_subject.save_id = {};
    lod_subject.forced_lod.reset();
    auto invalid_lod_subject_inspection = heartstead::debug::Inspector::inspect(lod_subject);
    assert(invalid_lod_subject_inspection.state == "invalid");
    assert(invalid_lod_subject_inspection.has_errors());

    lod_subject.save_id = heartstead::core::SaveId::from_value(43);
    lod_subject.kind = heartstead::simulation::SimulationSubjectKind::process_owner;
    auto missing_process_subject_inspection = heartstead::debug::Inspector::inspect(lod_subject);
    assert(missing_process_subject_inspection.state == "invalid");
    assert(missing_process_subject_inspection.has_errors());

    heartstead::simulation::SimulationLodDecision lod_decision;
    lod_decision.save_id = heartstead::core::SaveId::from_value(44);
    lod_decision.runtime_handle = heartstead::core::RuntimeHandle::from_value(101);
    lod_decision.kind = heartstead::simulation::SimulationSubjectKind::entity;
    lod_decision.lod = heartstead::simulation::SimulationLod::simplified;
    lod_decision.nearest_viewer_distance_squared = 2304;
    lod_decision.elapsed_since_update_ms = 1200;
    lod_decision.due_for_tick = true;
    auto lod_decision_inspection = heartstead::debug::Inspector::inspect(lod_decision);
    assert(lod_decision_inspection.object_type == "simulation_lod_decision");
    assert(lod_decision_inspection.save_id == "44");
    assert(lod_decision_inspection.runtime_id == "101");
    assert(lod_decision_inspection.state == "simplified");
    assert(lod_decision_inspection.find_field("kind")->value == "entity");
    assert(lod_decision_inspection.find_field("process_id")->value.empty());
    assert(lod_decision_inspection.find_field("due_for_tick")->value == "true");
    assert(lod_decision_inspection.issues.empty());

    auto unloaded_lod_decision = lod_decision;
    unloaded_lod_decision.save_id = heartstead::core::SaveId::from_value(45);
    unloaded_lod_decision.runtime_handle = {};
    unloaded_lod_decision.lod = heartstead::simulation::SimulationLod::unloaded;
    unloaded_lod_decision.nearest_viewer_distance_squared =
        std::numeric_limits<std::uint64_t>::max();
    unloaded_lod_decision.elapsed_since_update_ms = 8000;
    unloaded_lod_decision.offline_delta_ms = 8000;
    unloaded_lod_decision.due_for_tick = false;

    heartstead::simulation::SimulationFramePlan lod_frame_plan;
    lod_frame_plan.decisions = {lod_decision, unloaded_lod_decision};
    lod_frame_plan.simplified_count = 1;
    lod_frame_plan.unloaded_count = 1;
    lod_frame_plan.due_tick_count = 1;
    auto lod_frame_plan_inspection = heartstead::debug::Inspector::inspect(lod_frame_plan);
    assert(lod_frame_plan_inspection.object_type == "simulation_frame_plan");
    assert(lod_frame_plan_inspection.state == "active");
    assert(lod_frame_plan_inspection.find_field("decision_count")->value == "2");
    assert(lod_frame_plan_inspection.find_field("simplified_count")->value == "1");
    assert(lod_frame_plan_inspection.find_field("unloaded_count")->value == "1");
    assert(lod_frame_plan_inspection.find_field("due_tick_count")->value == "1");
    assert(lod_frame_plan_inspection.find_field("total_offline_delta_ms")->value == "8000");
    assert(lod_frame_plan_inspection.find_field("first_decision_lod")->value == "simplified");
    assert(lod_frame_plan_inspection.issues.empty());

    lod_frame_plan.full_count = 1;
    auto invalid_lod_frame_plan_inspection = heartstead::debug::Inspector::inspect(lod_frame_plan);
    assert(invalid_lod_frame_plan_inspection.state == "invalid");
    assert(invalid_lod_frame_plan_inspection.has_errors());

    auto transport_backend_inspection =
        heartstead::debug::Inspector::inspect(heartstead::net::transport_backend_info(
            heartstead::net::TransportBackend::external_library));
    assert(transport_backend_inspection.object_type == "transport_backend");
    const auto external_transport_available =
        heartstead::net::transport_backend_info(heartstead::net::TransportBackend::external_library)
            .available;
    assert(transport_backend_inspection.state ==
           (external_transport_available ? "available" : "unavailable"));
    assert(transport_backend_inspection.issues.empty() == external_transport_available);

    const auto transport_capabilities =
        heartstead::net::transport_host_capabilities(heartstead::net::InMemoryTransportHostConfig{
            heartstead::core::NetId::from_value(7), 2048, 4});
    auto transport_capabilities_inspection =
        heartstead::debug::Inspector::inspect(transport_capabilities);
    assert(transport_capabilities_inspection.object_type == "transport_capabilities");
    assert(transport_capabilities_inspection.state == "host_capable");
    assert(transport_capabilities_inspection.find_field("max_payload_bytes")->value == "2048");
    assert(transport_capabilities_inspection.find_field("max_clients")->value == "4");
    assert(transport_capabilities_inspection.issues.empty());

    heartstead::net::TransportServerWelcome welcome{
        heartstead::net::transport_control_protocol_version,
        heartstead::core::NetId::from_value(7),
        heartstead::core::NetId::from_value(8),
        2048,
        4,
        true,
        true,
    };
    auto welcome_inspection = heartstead::debug::Inspector::inspect(welcome);
    assert(welcome_inspection.object_type == "transport_server_welcome");
    assert(welcome_inspection.state == "current");
    assert(welcome_inspection.runtime_id == "8");
    assert(welcome_inspection.find_field("server_id")->value == "7");
    assert(welcome_inspection.find_field("assigned_client_id")->value == "8");
    assert(welcome_inspection.issues.empty());

    heartstead::net::TransportClientSession client_session{
        heartstead::net::transport_control_protocol_version,
        heartstead::core::NetId::from_value(7),
        heartstead::core::NetId::from_value(8),
        2048,
        4,
        true,
        true,
        1234,
    };
    auto client_session_inspection = heartstead::debug::Inspector::inspect(client_session);
    assert(client_session_inspection.object_type == "transport_client_session");
    assert(client_session_inspection.state == "connected");
    assert(client_session_inspection.runtime_id == "8");
    assert(client_session_inspection.find_field("server_id")->value == "7");
    assert(client_session_inspection.find_field("client_id")->value == "8");
    assert(client_session_inspection.find_field("established_at_ms")->value == "1234");
    assert(client_session_inspection.issues.empty());

    welcome.protocol_version = 99;
    auto invalid_welcome_inspection = heartstead::debug::Inspector::inspect(welcome);
    assert(invalid_welcome_inspection.state == "unsupported");
    assert(invalid_welcome_inspection.has_errors());

    client_session.protocol_version = 99;
    auto invalid_client_session_inspection = heartstead::debug::Inspector::inspect(client_session);
    assert(invalid_client_session_inspection.state == "unsupported");
    assert(invalid_client_session_inspection.has_errors());

    heartstead::net::CommandOperationTrace operation_trace;
    operation_trace.stages = {
        heartstead::world::OperationStage::begun, heartstead::world::OperationStage::validated,
        heartstead::world::OperationStage::mutated, heartstead::world::OperationStage::committed};
    operation_trace.mutations = {"place build piece"};
    operation_trace.derived_updates = {"RoomGraph"};
    operation_trace.replication_dirty = true;
    operation_trace.save_dirty = true;
    auto operation_trace_inspection = heartstead::debug::Inspector::inspect(operation_trace);
    assert(operation_trace_inspection.object_type == "command_operation_trace");
    assert(operation_trace_inspection.state == "committed");
    assert(operation_trace_inspection.find_field("stage_count")->value == "4");
    assert(operation_trace_inspection.find_field("last_stage")->value == "committed");
    assert(operation_trace_inspection.find_field("mutation_count")->value == "1");
    assert(operation_trace_inspection.find_field("first_mutation")->value == "place build piece");
    assert(operation_trace_inspection.find_field("first_derived_update")->value == "RoomGraph");
    assert(operation_trace_inspection.find_field("replication_dirty")->value == "true");
    assert(operation_trace_inspection.find_field("save_dirty")->value == "true");
    assert(operation_trace_inspection.issues.empty());

    auto invalid_operation_trace = operation_trace;
    invalid_operation_trace.replication_dirty = false;
    auto invalid_operation_trace_inspection =
        heartstead::debug::Inspector::inspect(invalid_operation_trace);
    assert(invalid_operation_trace_inspection.state == "invalid");
    assert(invalid_operation_trace_inspection.has_errors());

    heartstead::net::CommandDispatchResult dispatch_result;
    dispatch_result.sequence = 11;
    dispatch_result.command_type = "build.place_piece";
    dispatch_result.committed_world_mutation = true;
    dispatch_result.events.push_back(
        {"build_piece.placed", heartstead::core::SaveId::from_value(20), "wall"});
    dispatch_result.reserved_ids.push_back(heartstead::core::SaveId::from_value(20));
    dispatch_result.operation_trace = operation_trace;
    auto dispatch_result_inspection = heartstead::debug::Inspector::inspect(dispatch_result);
    assert(dispatch_result_inspection.object_type == "command_dispatch_result");
    assert(dispatch_result_inspection.state == "committed");
    assert(dispatch_result_inspection.runtime_id == "11");
    assert(dispatch_result_inspection.find_field("event_count")->value == "1");
    assert(dispatch_result_inspection.find_field("reserved_id_count")->value == "1");
    assert(dispatch_result_inspection.find_field("last_stage")->value == "committed");
    assert(dispatch_result_inspection.issues.empty());

    heartstead::net::CommandDispatchReport dispatch_report;
    dispatch_report.sequence = 12;
    dispatch_report.command_type = "build.place_piece";
    dispatch_report.succeeded = true;
    dispatch_report.committed_world_mutation = true;
    dispatch_report.events = dispatch_result.events;
    dispatch_report.reserved_ids = dispatch_result.reserved_ids;
    dispatch_report.operation_trace = operation_trace;
    auto dispatch_report_inspection = heartstead::debug::Inspector::inspect(dispatch_report);
    assert(dispatch_report_inspection.object_type == "command_dispatch_report");
    assert(dispatch_report_inspection.state == "committed");
    assert(dispatch_report_inspection.find_field("succeeded")->value == "true");
    assert(dispatch_report_inspection.find_field("error_code")->value.empty());
    assert(dispatch_report_inspection.issues.empty());

    heartstead::net::CommandDispatchReport failed_dispatch_report;
    failed_dispatch_report.sequence = 13;
    failed_dispatch_report.command_type = "build.place_piece";
    failed_dispatch_report.error =
        heartstead::core::Error{"command.invalid_payload", "payload rejected"};
    failed_dispatch_report.operation_trace.stages = {
        heartstead::world::OperationStage::begun, heartstead::world::OperationStage::validated,
        heartstead::world::OperationStage::rolled_back};
    auto failed_report_inspection = heartstead::debug::Inspector::inspect(failed_dispatch_report);
    assert(failed_report_inspection.object_type == "command_dispatch_report");
    assert(failed_report_inspection.state == "rejected");
    assert(failed_report_inspection.find_field("succeeded")->value == "false");
    assert(failed_report_inspection.find_field("error_code")->value == "command.invalid_payload");
    assert(failed_report_inspection.find_field("last_stage")->value == "rolled_back");
    assert(!failed_report_inspection.has_errors());

    heartstead::net::HostSessionCommandReport command_report;
    command_report.client_id = heartstead::core::NetId::from_value(8);
    command_report.sequence = 11;
    command_report.command_type = "build.place_piece";
    command_report.success = true;
    command_report.committed_world_mutation = true;
    command_report.events = dispatch_result.events;
    command_report.reserved_ids = dispatch_result.reserved_ids;
    command_report.operation_trace = operation_trace;
    auto command_report_inspection = heartstead::debug::Inspector::inspect(command_report);
    assert(command_report_inspection.object_type == "host_session_command_report");
    assert(command_report_inspection.state == "accepted");
    assert(command_report_inspection.find_field("client_id")->value == "8");
    assert(command_report_inspection.find_field("last_stage")->value == "committed");
    assert(command_report_inspection.issues.empty());

    heartstead::net::HostSessionCommandResult command_result;
    command_result.sequence = 12;
    command_result.command_type = "debug.ping";
    command_result.success = true;
    command_result.committed_world_mutation = true;
    command_result.event_count = 2;
    command_result.reserved_id_count = 1;
    auto command_result_inspection = heartstead::debug::Inspector::inspect(command_result);
    assert(command_result_inspection.object_type == "host_session_command_result");
    assert(command_result_inspection.state == "accepted");
    assert(command_result_inspection.runtime_id == "12");
    assert(command_result_inspection.find_field("command_type")->value == "debug.ping");
    assert(command_result_inspection.find_field("event_count")->value == "2");
    assert(command_result_inspection.issues.empty());

    command_result.success = false;
    command_result.committed_world_mutation = false;
    command_result.error_code = "debug.failed";
    command_result.error_message = "bad input";
    auto rejected_result_inspection = heartstead::debug::Inspector::inspect(command_result);
    assert(rejected_result_inspection.state == "rejected");
    assert(rejected_result_inspection.find_field("error_code")->value == "debug.failed");
    assert(rejected_result_inspection.issues.empty());

    heartstead::replay::CommandReplayLog replay_log;
    replay_log.scenario_id = "inspection_replay";
    replay_log.world_seed = 44;
    heartstead::replay::RecordedCommand recorded_command;
    recorded_command.envelope.sequence = 11;
    recorded_command.envelope.sender = heartstead::core::NetId::from_value(8);
    recorded_command.envelope.type = "build.place_piece";
    recorded_command.envelope.payload = "base:build_pieces/wall_frame";
    recorded_command.envelope.client_time_ms = 100;
    recorded_command.server_time_ms = 110;
    recorded_command.expectation.has_committed_world_mutation = true;
    recorded_command.expectation.committed_world_mutation = true;
    recorded_command.expectation.event_count = 1;
    recorded_command.expectation.reserved_ids = {heartstead::core::SaveId::from_value(20)};
    replay_log.commands.push_back(recorded_command);
    auto replay_log_inspection = heartstead::debug::Inspector::inspect(replay_log);
    assert(replay_log_inspection.object_type == "command_replay_log");
    assert(replay_log_inspection.state == "recorded");
    assert(replay_log_inspection.find_field("scenario_id")->value == "inspection_replay");
    assert(replay_log_inspection.find_field("command_count")->value == "1");
    assert(replay_log_inspection.find_field("expectation_count")->value == "1");
    assert(replay_log_inspection.find_field("mutating_expectation_count")->value == "1");
    assert(replay_log_inspection.find_field("last_command_type")->value == "build.place_piece");
    assert(replay_log_inspection.issues.empty());

    heartstead::replay::CommandReplayStep replay_step;
    replay_step.index = 1;
    replay_step.sequence = 11;
    replay_step.command_type = "build.place_piece";
    replay_step.success = true;
    replay_step.committed_world_mutation = true;
    replay_step.events = dispatch_result.events;
    replay_step.reserved_ids = dispatch_result.reserved_ids;
    replay_step.operation_trace = operation_trace;
    auto replay_step_inspection = heartstead::debug::Inspector::inspect(replay_step);
    assert(replay_step_inspection.object_type == "command_replay_step");
    assert(replay_step_inspection.state == "committed");
    assert(replay_step_inspection.find_field("index")->value == "1");
    assert(replay_step_inspection.find_field("last_stage")->value == "committed");
    assert(replay_step_inspection.issues.empty());

    heartstead::replay::CommandReplayReport replay_report;
    replay_report.steps.push_back(replay_step);
    auto replay_report_inspection = heartstead::debug::Inspector::inspect(replay_report);
    assert(replay_report_inspection.object_type == "command_replay_report");
    assert(replay_report_inspection.state == "complete");
    assert(replay_report_inspection.find_field("step_count")->value == "1");
    assert(replay_report_inspection.find_field("committed_step_count")->value == "1");
    assert(replay_report_inspection.find_field("event_count")->value == "1");
    assert(replay_report_inspection.find_field("reserved_id_count")->value == "1");
    assert(replay_report_inspection.find_field("last_command_type")->value == "build.place_piece");

    heartstead::net::ClientSession disconnected_client(heartstead::core::NetId::from_value(8));
    auto disconnected_client_inspection =
        heartstead::debug::Inspector::inspect(disconnected_client);
    assert(disconnected_client_inspection.object_type == "client_session");
    assert(disconnected_client_inspection.state == "disconnected");
    assert(disconnected_client_inspection.find_field("expected_client_id")->value == "8");
    assert(!disconnected_client_inspection.issues.empty());

    heartstead::net::ClientSession connected_client(heartstead::core::NetId::from_value(8));
    auto accepted_welcome = connected_client.accept_welcome(heartstead::net::TransportEnvelope{
        heartstead::core::NetId::from_value(7),
        heartstead::core::NetId::from_value(8),
        heartstead::net::make_server_welcome_transport_message(
            heartstead::net::TransportServerWelcome{
                heartstead::net::transport_control_protocol_version,
                heartstead::core::NetId::from_value(7),
                heartstead::core::NetId::from_value(8),
                2048,
                4,
                true,
                true,
            },
            10),
    });
    assert(accepted_welcome);
    auto pending_command = connected_client.create_command("debug.ping", "payload", 11);
    assert(pending_command);
    auto connected_client_inspection = heartstead::debug::Inspector::inspect(connected_client);
    assert(connected_client_inspection.state == "connected");
    assert(connected_client_inspection.find_field("server_id")->value == "7");
    assert(connected_client_inspection.find_field("client_id")->value == "8");
    assert(connected_client_inspection.find_field("pending_command_count")->value == "1");
    assert(connected_client_inspection.find_field("has_replication_sequence")->value == "false");
    assert(connected_client_inspection.find_field("last_replication_sequence")->value == "0");
    assert(connected_client_inspection.find_field("disconnect_reason_code")->value.empty());
    assert(connected_client_inspection.find_field("disconnected_at_ms")->value == "0");
    assert(connected_client_inspection.issues.empty());

    auto registry = build_snapshot_test_registry();

    const auto raw_clay_id = heartstead::core::PrototypeId::parse("base:items/raw_clay");
    const auto wall_id = heartstead::core::PrototypeId::parse("base:build_pieces/wall_frame");
    const auto heavy_log_id = heartstead::core::PrototypeId::parse("base:cargo/heavy_log");
    const auto drying_id = heartstead::core::PrototypeId::parse("base:processes/drying");
    assert(raw_clay_id && wall_id && heavy_log_id && drying_id);

    heartstead::entities::EntityRecord inspected_entity;
    inspected_entity.runtime_handle = heartstead::core::RuntimeHandle::from_value(30);
    inspected_entity.net_id = heartstead::core::NetId::from_value(31);
    inspected_entity.save_id = heartstead::core::SaveId::from_value(32);
    inspected_entity.prototype_id =
        heartstead::core::PrototypeId::parse("base:entities/hand_cart").value();
    inspected_entity.kind = heartstead::entities::EntityKind::cart;
    inspected_entity.persistent = true;
    inspected_entity.transform.position = {3.0, 4.0, 5.0};
    auto entity_inspection = heartstead::debug::Inspector::inspect(inspected_entity);
    assert(entity_inspection.object_type == "entity");
    assert(entity_inspection.save_id == "32");
    assert(entity_inspection.runtime_id == "30");
    assert(entity_inspection.find_field("position")->value == "3.000000|4.000000|5.000000");
    assert(entity_inspection.find_field("scale")->value == "1.000000|1.000000|1.000000");
    assert(entity_inspection.issues.empty());

    heartstead::build::BuildPieceRecord wall;
    wall.object_id = heartstead::core::SaveId::from_value(44);
    wall.prototype_id = wall_id.value();
    wall.construction_state = heartstead::build::ConstructionState::complete;
    wall.room_contribution_tags.push_back("wall");
    wall.network_ports.push_back(
        {"storage_access", heartstead::networks::NetworkKind::storage_access, 1});

    auto build_inspection = heartstead::debug::Inspector::inspect(wall);
    assert(build_inspection.object_type == "build_piece");
    assert(build_inspection.save_id == "44");
    assert(build_inspection.state == "complete");
    assert(!build_inspection.has_errors());
    const auto* room_field = build_inspection.find_field("contributes_to_rooms");
    assert(room_field != nullptr);
    assert(room_field->value == "true");

    heartstead::cargo::CargoRecord invalid_cargo;
    invalid_cargo.cargo_id = heartstead::core::SaveId::from_value(55);
    invalid_cargo.prototype_id = heavy_log_id.value();
    invalid_cargo.mass_grams = 0;
    invalid_cargo.volume_milliliters = 20;
    invalid_cargo.allowed_transport_modes =
        heartstead::cargo::CargoTransportModes::of({heartstead::cargo::CargoTransportMode::cart});

    auto cargo_inspection = heartstead::debug::Inspector::inspect(invalid_cargo);
    assert(cargo_inspection.object_type == "cargo");
    assert(cargo_inspection.has_errors());
    assert(cargo_inspection.find_field("mass_grams") != nullptr);

    auto process = heartstead::processes::ProcessRuntime::create(
        heartstead::core::ProcessId::from_value(9), heartstead::core::SaveId::from_value(44),
        drying_id.value(), 100, 1000);
    assert(process);
    heartstead::processes::ProcessRuntime::interrupt(process.value(), "fuel missing");
    auto process_inspection = heartstead::debug::Inspector::inspect(process.value());
    assert(process_inspection.state == "interrupted");
    assert(!process_inspection.issues.empty());

    heartstead::networks::SpatialNetwork road_network(heartstead::networks::NetworkKind::road);
    const auto road_start = heartstead::networks::NetworkNodeId::from_value(1);
    const auto road_end = heartstead::networks::NetworkNodeId::from_value(2);
    assert(road_network.add_node({road_start, {0, 0, 0}, 2, "camp"}));
    assert(road_network.add_node({road_end, {4, 0, 0}, 2, "mine"}));
    assert(road_network.add_edge(
        {heartstead::networks::NetworkEdgeId::from_value(1), road_start, road_end, 25, 1, true}));
    assert(road_network.add_port({heartstead::networks::NetworkPortId::from_value(1), road_start,
                                  "cart_access", 1, heartstead::core::SaveId::from_value(501),
                                  heartstead::core::SaveId::from_value(501)}));
    auto network_inspection = heartstead::debug::Inspector::inspect(road_network);
    assert(network_inspection.object_type == "spatial_network");
    assert(network_inspection.state == "dirty");
    assert(network_inspection.find_field("kind")->value == "road");
    assert(network_inspection.find_field("node_count")->value == "2");
    assert(network_inspection.find_field("edge_count")->value == "1");
    assert(network_inspection.find_field("blocked_edge_count")->value == "1");
    assert(network_inspection.find_field("port_count")->value == "1");
    assert(network_inspection.find_field("owned_port_count")->value == "1");
    assert(network_inspection.find_field("sourced_port_count")->value == "1");
    assert(network_inspection.find_field("dirty_region_kind")->value == "road_network");
    assert(!network_inspection.issues.empty());

    heartstead::world::RegionGraph region_graph;
    heartstead::world::RegionDescriptor camp_region;
    camp_region.id = "camp";
    camp_region.age = "early";
    camp_region.biome_cluster = "forest";
    camp_region.sub_biomes = {"meadow", "creek"};
    camp_region.resource_rules.push_back({raw_clay_id.value(), "deposit", 0.4});
    camp_region.danger_gradient = 0.2;
    camp_region.magic_gradient = 0.1;
    camp_region.future_tool_layers.push_back("iron");
    camp_region.mastery_return_layers.push_back("winter");
    camp_region.ecology_parameters.emplace("humidity", 0.6);
    assert(region_graph.add_region(camp_region));

    heartstead::world::RegionDescriptor mine_region;
    mine_region.id = "mine";
    mine_region.age = "deep";
    mine_region.biome_cluster = "mountain";
    mine_region.danger_gradient = 0.7;
    mine_region.magic_gradient = 0.5;
    assert(region_graph.add_region(mine_region));
    assert(region_graph.connect({"camp", "mine", "road", 2.0, 1.0}));

    auto region_inspection = heartstead::debug::Inspector::inspect(region_graph);
    assert(region_inspection.object_type == "region_graph");
    assert(region_inspection.state == "defined");
    assert(region_inspection.find_field("region_count")->value == "2");
    assert(region_inspection.find_field("connection_count")->value == "1");
    assert(region_inspection.find_field("sub_biome_count")->value == "2");
    assert(region_inspection.find_field("resource_rule_count")->value == "1");
    assert(region_inspection.find_field("future_tool_layer_count")->value == "1");
    assert(region_inspection.find_field("mastery_return_layer_count")->value == "1");
    assert(region_inspection.find_field("ecology_parameter_count")->value == "1");
    assert(region_inspection.find_field("max_danger_gradient")->value == "0.700000");
    assert(region_inspection.find_field("max_magic_gradient")->value == "0.500000");
    assert(region_inspection.find_field("first_region_id")->value == "camp");
    assert(region_inspection.find_field("first_region_connection_count")->value == "1");
    assert(region_inspection.issues.empty());

    heartstead::save::SaveSnapshot snapshot;
    snapshot.metadata.game_version = "0.1.0";
    snapshot.metadata.world_seed = 99;
    snapshot.metadata.enabled_mods.push_back({"base", "0.0.1", "hash"});
    snapshot.build_pieces.push_back(wall);

    auto snapshot_inspection = heartstead::debug::Inspector::inspect(snapshot, &registry);
    assert(snapshot_inspection.object_type == "save_snapshot");
    assert(snapshot_inspection.state == "valid");
    const auto* build_piece_count = snapshot_inspection.find_field("build_piece_count");
    assert(build_piece_count != nullptr);
    assert(build_piece_count->value == "1");
    assert(snapshot_inspection.find_field("voxel_palette_count")->value == "0");
    assert(snapshot_inspection.find_field("missing_prototype_count")->value == "0");
    assert(snapshot_inspection.find_field("fire_count")->value == "0");

    snapshot.voxel_palette.entries.push_back({1, raw_clay_id.value()});
    snapshot.missing_prototypes.push_back({heartstead::world::MissingPrototypeKind::build_piece,
                                           77,
                                           wall_id.value(),
                                           {},
                                           {},
                                           {},
                                           {}});
    snapshot.fires.push_back({heartstead::core::SaveId::from_value(88), wall_id.value()});
    const auto complete_snapshot_inspection = heartstead::debug::Inspector::inspect(snapshot);
    assert(complete_snapshot_inspection.find_field("voxel_palette_count")->value == "1");
    assert(complete_snapshot_inspection.find_field("missing_prototype_count")->value == "1");
    assert(complete_snapshot_inspection.find_field("fire_count")->value == "1");

    heartstead::world::WorldStateDesc world_desc;
    world_desc.metadata.game_version = "0.1.0";
    world_desc.metadata.world_seed = 99;
    world_desc.next_save_id = 100;
    world_desc.next_runtime_handle = 500;
    world_desc.next_entity_net_id = 600;
    world_desc.next_process_id = 700;
    heartstead::world::WorldState world_state(world_desc);
    assert(world_state.chunks().set({0, 0, 0}, {0, 0, 0}, heartstead::world::VoxelCell{1, 0},
                                    world_state.dirty_regions()));

    auto world_inspection = heartstead::debug::Inspector::inspect(world_state);
    assert(world_inspection.object_type == "world_state");
    assert(world_inspection.state == "loaded");
    assert(!world_inspection.has_errors());
    const auto* inspected_chunk_count = world_inspection.find_field("chunk_count");
    assert(inspected_chunk_count != nullptr);
    assert(inspected_chunk_count->value == "1");
    const auto* next_save_id = world_inspection.find_field("next_save_id");
    assert(next_save_id != nullptr);
    assert(next_save_id->value == "100");
    const auto* next_runtime_handle = world_inspection.find_field("next_runtime_handle");
    assert(next_runtime_handle != nullptr);
    assert(next_runtime_handle->value == "500");
    const auto* next_entity_net_id = world_inspection.find_field("next_entity_net_id");
    assert(next_entity_net_id != nullptr);
    assert(next_entity_net_id->value == "600");
    const auto* next_process_id = world_inspection.find_field("next_process_id");
    assert(next_process_id != nullptr);
    assert(next_process_id->value == "700");
    assert(world_inspection.find_field("room_count")->value == "0");
    assert(world_inspection.find_field("dirty_region_chunk_mesh_count")->value == "1");
    assert(world_inspection.find_field("dirty_region_chunk_collision_count")->value == "1");
    assert(world_inspection.find_field("dirty_region_chunk_lighting_count")->value == "1");
    assert(world_inspection.find_field("dirty_region_room_graph_count")->value == "0");

    heartstead::world::WorldStateDesc diagnostic_world_desc;
    diagnostic_world_desc.metadata.game_version = "0.1.0";
    heartstead::world::WorldState diagnostic_world(diagnostic_world_desc);
    diagnostic_world.missing_prototypes().push_back(
        {heartstead::world::MissingPrototypeKind::build_piece,
         77,
         wall_id.value(),
         {},
         {},
         {},
         {}});
    assert(diagnostic_world.fires().insert(
        {heartstead::core::SaveId::from_value(88), wall_id.value()}));
    const auto diagnostic_world_inspection =
        heartstead::debug::Inspector::inspect(diagnostic_world);
    assert(diagnostic_world_inspection.state == "loaded");
    assert(diagnostic_world_inspection.find_field("voxel_palette_count")->value == "0");
    assert(diagnostic_world_inspection.find_field("missing_prototype_count")->value == "1");
    assert(diagnostic_world_inspection.find_field("fire_count")->value == "1");

    const auto rendered = heartstead::debug::Inspector::render_text(build_inspection);
    assert(rendered.find("build_piece") != std::string::npos);
    assert(rendered.find("prototype_id=base:build_pieces/wall_frame") != std::string::npos);
}

void test_process_prototype_materialization() {
    const auto process_id = heartstead::core::PrototypeId::parse("base:processes/drying");
    assert(process_id);

    heartstead::modding::GenericPrototype prototype;
    prototype.kind = std::string(heartstead::modding::PrototypeKinds::process);
    prototype.id = process_id.value();
    prototype.display_name = "Drying";
    prototype.fields.emplace("default_required_work_ms", "60000");
    prototype.fields.emplace("requires_room", "true");
    prototype.fields.emplace("requires_power", "true");
    prototype.fields.emplace("required_power_capacity", "2");
    prototype.fields.emplace("base_quality_rate_per_mille", "1100");
    prototype.fields.emplace("tags", "ambient,timestamped");

    auto definition = heartstead::processes::process_definition_from_prototype(prototype);
    assert(definition);
    assert(definition.value().prototype_id == process_id.value());
    assert(definition.value().default_required_work_ms == 60000);
    assert(definition.value().requires_room);
    assert(definition.value().requires_power);
    assert(definition.value().required_power_capacity == 2);
    assert(definition.value().base_quality_rate_per_mille == 1100);
    assert(definition.value().tags.size() == 2);

    auto instance = heartstead::processes::ProcessRuntime::create(
        heartstead::core::ProcessId::from_value(3), heartstead::core::SaveId::from_value(99),
        definition.value(), 250);
    assert(instance);
    assert(instance.value().prototype_id == process_id.value());
    assert(instance.value().required_effective_work_ms == 60000);
    assert(instance.value().last_update_time_ms == 250);

    prototype.fields["default_required_work_ms"] = "0";
    auto invalid_work = heartstead::processes::process_definition_from_prototype(prototype);
    assert(!invalid_work);

    prototype.fields["default_required_work_ms"] = "60000";
    prototype.fields["requires_power"] = "maybe";
    auto invalid_bool = heartstead::processes::process_definition_from_prototype(prototype);
    assert(!invalid_bool);

    prototype.fields["requires_power"] = "true";
    prototype.fields["required_power_capacity"] = "0";
    auto invalid_power_capacity =
        heartstead::processes::process_definition_from_prototype(prototype);
    assert(!invalid_power_capacity);

    prototype.fields["required_power_capacity"] = "2";
    prototype.fields["base_quality_rate_per_mille"] = "10001";
    auto invalid_quality = heartstead::processes::process_definition_from_prototype(prototype);
    assert(!invalid_quality);

    prototype.fields["base_quality_rate_per_mille"] = "1100";
    prototype.fields["tags"] = "bad tag";
    auto invalid_tag = heartstead::processes::process_definition_from_prototype(prototype);
    assert(!invalid_tag);
}

void test_room_descriptor_prototype_materialization() {
    const auto descriptor_id = heartstead::core::PrototypeId::parse("base:room_descriptors/warm");
    assert(descriptor_id);

    heartstead::modding::GenericPrototype prototype;
    prototype.kind = std::string(heartstead::modding::PrototypeKinds::room_descriptor);
    prototype.id = descriptor_id.value();
    prototype.display_name = "Warm";
    prototype.fields.emplace("code", "warm");
    prototype.fields.emplace("label", "Warm");
    prototype.fields.emplace("severity", "positive");
    prototype.fields.emplace("tags", "comfort,temperature");

    auto definition = heartstead::rooms::room_descriptor_definition_from_prototype(prototype);
    assert(definition);
    assert(definition.value().prototype_id == descriptor_id.value());
    assert(definition.value().code == "warm");
    assert(definition.value().label == "Warm");
    assert(definition.value().severity == heartstead::rooms::RoomDescriptorSeverity::positive);
    assert(definition.value().tags.size() == 2);

    auto descriptor = definition.value().descriptor();
    assert(descriptor.code == "warm");
    assert(descriptor.label == "Warm");
    assert(descriptor.severity == heartstead::rooms::RoomDescriptorSeverity::positive);

    prototype.fields["severity"] = "severe";
    auto invalid_severity = heartstead::rooms::room_descriptor_definition_from_prototype(prototype);
    assert(!invalid_severity);
    assert(invalid_severity.error().code == "room_descriptor.invalid_severity");

    prototype.fields["severity"] = "positive";
    prototype.fields["code"] = "bad code";
    auto invalid_code = heartstead::rooms::room_descriptor_definition_from_prototype(prototype);
    assert(!invalid_code);
    assert(invalid_code.error().code == "room_descriptor_definition.invalid_code");

    prototype.fields["code"] = "warm";
    prototype.fields["tags"] = "bad tag";
    auto invalid_tag = heartstead::rooms::room_descriptor_definition_from_prototype(prototype);
    assert(!invalid_tag);
    assert(invalid_tag.error().code == "room_descriptor_prototype.invalid_tag");
}

void test_process_runtime() {
    const auto prototype = heartstead::core::PrototypeId::parse("base:processes/drying");
    assert(prototype);

    heartstead::processes::ProcessIdAllocator allocator(20);
    auto allocated_id = allocator.reserve();
    assert(allocated_id);
    assert(allocated_id.value() == heartstead::core::ProcessId::from_value(20));
    assert(allocator.peek_next() == heartstead::core::ProcessId::from_value(21));

    auto created = heartstead::processes::ProcessRuntime::create(
        heartstead::core::ProcessId::from_value(1), heartstead::core::SaveId::from_value(77),
        prototype.value(), 1000, 10000);
    assert(created);

    auto process = std::move(created).value();
    assert(process.validate());
    assert(process.remaining_effective_work_ms() == 10000);

    heartstead::processes::ProcessSlot valid_slot{prototype.value(), 1};
    assert(valid_slot.validate());
    heartstead::processes::ProcessSlot empty_slot{prototype.value(), 0};
    auto empty_slot_status = empty_slot.validate();
    assert(!empty_slot_status);
    assert(empty_slot_status.error().code == "process_slot.invalid_count");

    auto advanced = heartstead::processes::ProcessRuntime::advance(
        process, 6000, heartstead::processes::ProcessModifiers{});
    assert(advanced);
    assert(process.accumulated_effective_work_ms == 5000);
    assert(!process.is_complete());

    heartstead::processes::ProcessRuntime::interrupt(process, "input removed");
    advanced = heartstead::processes::ProcessRuntime::advance(
        process, 9000, heartstead::processes::ProcessModifiers{});
    assert(advanced);
    assert(process.accumulated_effective_work_ms == 5000);

    auto resumed = heartstead::processes::ProcessRuntime::resume(process, 9000);
    assert(resumed);

    advanced = heartstead::processes::ProcessRuntime::advance(
        process, 11500, heartstead::processes::ProcessModifiers{2000, 1000, 1000});
    assert(advanced);
    assert(process.is_complete());
    assert(process.validate());
    assert(process.remaining_effective_work_ms() == 0);

    auto invalid_complete = process;
    invalid_complete.accumulated_effective_work_ms =
        invalid_complete.required_effective_work_ms - 1;
    auto invalid_complete_status = invalid_complete.validate();
    assert(!invalid_complete_status);
    assert(invalid_complete_status.error().code == "process.invalid_complete_work");

    auto invalid_state = process;
    invalid_state.state = static_cast<heartstead::processes::ProcessState>(99);
    auto invalid_state_status = invalid_state.validate();
    assert(!invalid_state_status);
    assert(invalid_state_status.error().code == "process.invalid_state");

    auto invalid_reason = process;
    invalid_reason.interruption_reason = "stale reason";
    auto invalid_reason_status = invalid_reason.validate();
    assert(!invalid_reason_status);
    assert(invalid_reason_status.error().code == "process.invalid_interruption_reason");
}

void test_process_environment_resolver() {
    const auto owner_id = heartstead::core::SaveId::from_value(77);
    const auto process_prototype = heartstead::core::PrototypeId::parse("base:processes/drying");
    assert(process_prototype);

    heartstead::rooms::RoomRecord room;
    room.id = heartstead::rooms::RoomId::from_value(3);
    room.label = "Dry Workshop";
    room.volume_cells = 12;
    room.source_build_piece_ids.push_back(owner_id);
    room.metrics.enclosure_per_mille = 950;
    room.metrics.roof_coverage_per_mille = 900;
    room.metrics.wall_coverage_per_mille = 900;
    room.metrics.warmth = 300;
    room.metrics.dryness = 300;
    room.metrics.ventilation_per_mille = 800;
    room.metrics.safety_per_mille = 900;
    room.metrics.spaciousness_per_mille = 800;
    room.metrics.storage_access = true;
    room.metrics.cart_access = true;
    room.metrics.power_access = true;
    room.descriptors = heartstead::rooms::RoomEvaluator::evaluate(room.metrics);

    heartstead::rooms::RoomGraph graph;
    assert(graph.add_or_replace(room));
    graph.clear_dirty();

    const auto* found_room =
        heartstead::processes::ProcessEnvironmentResolver::find_room_for_owner(graph, owner_id);
    assert(found_room != nullptr);
    assert(found_room->id == heartstead::rooms::RoomId::from_value(3));

    heartstead::processes::ProcessEnvironmentDesc desc;
    desc.owner_id = owner_id;
    desc.room_graph = &graph;
    desc.requires_room = true;
    desc.requires_power = true;
    desc.available_power_capacity = 4;
    desc.required_power_capacity = 2;
    desc.base_quality_rate_per_mille = 1100;

    auto resolved = heartstead::processes::ProcessEnvironmentResolver::resolve(desc);
    assert(resolved);
    assert(resolved.value().room_found);
    assert(resolved.value().room_id == heartstead::rooms::RoomId::from_value(3));
    assert(resolved.value().power_satisfied);
    assert(resolved.value().modifiers.room_rate_per_mille > 1000);
    assert(resolved.value().modifiers.power_rate_per_mille == 2000);
    assert(resolved.value().modifiers.quality_rate_per_mille == 1100);
    assert(!resolved.value().factors.empty());
    assert(resolved.value().warnings.empty());

    auto process = heartstead::processes::ProcessRuntime::create(
        heartstead::core::ProcessId::from_value(50), owner_id, process_prototype.value(), 0, 2000);
    assert(process);
    auto advanced = heartstead::processes::ProcessRuntime::advance(process.value(), 1000,
                                                                   resolved.value().modifiers);
    assert(advanced);
    assert(process.value().is_complete());

    desc.owner_id = heartstead::core::SaveId::from_value(88);
    desc.available_power_capacity = 0;
    auto missing = heartstead::processes::ProcessEnvironmentResolver::resolve(desc);
    assert(missing);
    assert(!missing.value().room_found);
    assert(!missing.value().power_satisfied);
    assert(missing.value().modifiers.room_rate_per_mille == 0);
    assert(missing.value().modifiers.power_rate_per_mille == 0);
    assert(std::ranges::find(missing.value().warnings, "missing_room") !=
           missing.value().warnings.end());
    assert(std::ranges::find(missing.value().warnings, "missing_power") !=
           missing.value().warnings.end());

    desc.owner_id = owner_id;
    desc.available_power_capacity = 1;
    desc.required_power_capacity = 3;
    auto insufficient = heartstead::processes::ProcessEnvironmentResolver::resolve(desc);
    assert(insufficient);
    assert(insufficient.value().room_found);
    assert(!insufficient.value().power_satisfied);
    assert(insufficient.value().modifiers.power_rate_per_mille == 333);
    assert(std::ranges::find(insufficient.value().warnings, "insufficient_power") !=
           insufficient.value().warnings.end());

    desc.required_power_capacity = 0;
    auto invalid = heartstead::processes::ProcessEnvironmentResolver::resolve(desc);
    assert(!invalid);
    assert(invalid.error().code == "process_environment.invalid_power_requirement");
}

void test_simulation_lod_planner() {
    using namespace heartstead::simulation;

    SimulationLodPolicy policy;
    policy.full_radius = 16;
    policy.simplified_radius = 64;
    policy.full_tick_interval_ms = 16;
    policy.simplified_tick_interval_ms = 1000;
    policy.sleeping_tick_interval_ms = 5000;

    const std::vector<SimulationViewer> viewers{
        {heartstead::core::NetId::from_value(1), {0, 0, 0}}};

    const auto prototype = heartstead::core::PrototypeId::parse("base:entities/hand_cart");
    assert(prototype);

    SimulationSubject near_subject;
    near_subject.save_id = heartstead::core::SaveId::from_value(10);
    near_subject.runtime_handle = heartstead::core::RuntimeHandle::from_value(100);
    near_subject.prototype_id = prototype.value();
    near_subject.kind = SimulationSubjectKind::entity;
    near_subject.coord = {4, 0, 0};
    near_subject.last_update_time_ms = 9000;
    near_subject.label = "near cart";

    SimulationSubject far_subject = near_subject;
    far_subject.save_id = heartstead::core::SaveId::from_value(11);
    far_subject.coord = {48, 0, 0};
    far_subject.last_update_time_ms = 8800;

    SimulationSubject sleeping_subject = near_subject;
    sleeping_subject.save_id = heartstead::core::SaveId::from_value(12);
    sleeping_subject.coord = {6, 0, 0};
    sleeping_subject.last_update_time_ms = 4000;
    sleeping_subject.sleeping = true;

    SimulationSubject unloaded_subject = near_subject;
    unloaded_subject.save_id = heartstead::core::SaveId::from_value(13);
    unloaded_subject.coord = {256, 0, 0};
    unloaded_subject.last_update_time_ms = 2000;

    auto plan = SimulationLodPlanner::plan_frame(
        {near_subject, far_subject, sleeping_subject, unloaded_subject}, viewers, policy, 10000);
    assert(plan);
    assert(plan.value().count(SimulationLod::full) == 1);
    assert(plan.value().count(SimulationLod::simplified) == 1);
    assert(plan.value().count(SimulationLod::sleeping) == 1);
    assert(plan.value().count(SimulationLod::unloaded) == 1);
    assert(plan.value().due_tick_count == 3);

    assert(plan.value().decisions[0].lod == SimulationLod::full);
    assert(plan.value().decisions[0].due_for_tick);
    assert(plan.value().decisions[1].lod == SimulationLod::simplified);
    assert(plan.value().decisions[1].due_for_tick);
    assert(plan.value().decisions[2].lod == SimulationLod::sleeping);
    assert(plan.value().decisions[2].due_for_tick);
    assert(plan.value().decisions[3].lod == SimulationLod::unloaded);
    assert(!plan.value().decisions[3].due_for_tick);
    assert(plan.value().decisions[3].offline_delta_ms == 8000);

    auto forced = unloaded_subject;
    forced.forced_lod = SimulationLod::simplified;
    auto forced_decision = SimulationLodPlanner::classify(forced, {}, policy, 10000);
    assert(forced_decision);
    assert(forced_decision.value().lod == SimulationLod::simplified);
    assert(forced_decision.value().due_for_tick);

    auto invalid_policy = policy;
    invalid_policy.full_radius = 128;
    invalid_policy.simplified_radius = 64;
    auto invalid = SimulationLodPlanner::classify(near_subject, viewers, invalid_policy, 10000);
    assert(!invalid);

    auto reversed_time = SimulationLodPlanner::classify(near_subject, viewers, policy, 8000);
    assert(!reversed_time);

    auto process_subject = near_subject;
    process_subject.kind = SimulationSubjectKind::process_owner;
    auto missing_process_id =
        SimulationLodPlanner::classify(process_subject, viewers, policy, 10000);
    assert(!missing_process_id);
    assert(missing_process_id.error().code == "simulation.missing_process_id");

    process_subject.process_id = heartstead::core::ProcessId::from_value(77);
    auto process_decision = SimulationLodPlanner::classify(process_subject, viewers, policy, 10000);
    assert(process_decision);
    assert(process_decision.value().process_id == heartstead::core::ProcessId::from_value(77));
}

void test_world_simulation_subject_derivation() {
    heartstead::world::WorldStateDesc desc;
    desc.next_runtime_handle = 20;
    heartstead::world::WorldState state(desc);

    const auto build_prototype = heartstead::core::PrototypeId::parse("base:build_pieces/wall");
    const auto entity_prototype = heartstead::core::PrototypeId::parse("base:entities/cart");
    const auto cargo_prototype = heartstead::core::PrototypeId::parse("base:cargo/heavy_log");
    const auto assembly_prototype = heartstead::core::PrototypeId::parse("base:assemblies/kiln");
    const auto workpiece_prototype = heartstead::core::PrototypeId::parse("base:workpieces/clay");
    const auto process_prototype = heartstead::core::PrototypeId::parse("base:processes/drying");
    assert(build_prototype);
    assert(entity_prototype);
    assert(cargo_prototype);
    assert(assembly_prototype);
    assert(workpiece_prototype);
    assert(process_prototype);

    heartstead::build::BuildPieceRecord build_piece;
    build_piece.object_id = heartstead::core::SaveId::from_value(100);
    build_piece.prototype_id = build_prototype.value();
    build_piece.transform.position = {12.75, 0.0, -2.25};
    build_piece.construction_state = heartstead::build::ConstructionState::complete;
    assert(state.build_objects().insert(build_piece));

    auto runtime_handle = state.runtime_handles().reserve();
    assert(runtime_handle);
    heartstead::entities::EntityRecord entity;
    entity.runtime_handle = runtime_handle.value();
    entity.net_id = heartstead::core::NetId::from_value(40);
    entity.save_id = heartstead::core::SaveId::from_value(101);
    entity.prototype_id = entity_prototype.value();
    entity.kind = heartstead::entities::EntityKind::cart;
    entity.transform.position = {4.0, 0.0, 0.0};
    entity.persistent = true;
    entity.sleeping = true;
    assert(state.entities().insert(entity));

    heartstead::cargo::CargoRecord cargo;
    cargo.cargo_id = heartstead::core::SaveId::from_value(102);
    cargo.prototype_id = cargo_prototype.value();
    cargo.position = {-8.5, 0.0, 3.25};
    cargo.mass_grams = 90000;
    cargo.volume_milliliters = 180000;
    cargo.allowed_transport_modes =
        heartstead::cargo::CargoTransportModes::of({heartstead::cargo::CargoTransportMode::cart});
    assert(state.cargo().insert(cargo));

    heartstead::assemblies::AssemblyRecord assembly;
    assembly.assembly_id = heartstead::core::SaveId::from_value(103);
    assembly.root_build_piece_id = build_piece.object_id;
    assembly.prototype_id = assembly_prototype.value();
    assembly.operating = true;
    assembly.state = heartstead::assemblies::AssemblyState::operating;
    assert(state.assemblies().insert(assembly));

    auto workpiece_grid = heartstead::workpieces::WorkpieceGrid::create({1, 1, 1});
    assert(workpiece_grid);
    heartstead::world::WorkpieceRecord workpiece{heartstead::core::WorkpieceId::from_value(104),
                                                 workpiece_prototype.value(),
                                                 std::move(workpiece_grid).value()};
    workpiece.owner_session = heartstead::core::NetId::from_value(7);
    assert(state.workpieces().insert(std::move(workpiece)));

    auto& storage_network =
        state.networks().get_or_create(heartstead::networks::NetworkKind::storage_access);
    const auto network_node_id = heartstead::networks::NetworkNodeId::from_value(80);
    assert(storage_network.add_node(
        heartstead::networks::NetworkNode{network_node_id, {2, 0, 0}, 4, "storage"}));

    assert(state.chunks().set({0, 0, 0}, {1, 1, 1}, {3, 0}));

    auto build_process = heartstead::processes::ProcessRuntime::create(
        heartstead::core::ProcessId::from_value(300), build_piece.object_id,
        process_prototype.value(), 100, 2000);
    assert(build_process);
    assert(state.processes().insert(build_process.value()));

    auto cargo_process = heartstead::processes::ProcessRuntime::create(
        heartstead::core::ProcessId::from_value(301), cargo.cargo_id, process_prototype.value(),
        200, 2000);
    assert(cargo_process);
    assert(state.processes().insert(cargo_process.value()));

    auto second_build_process = heartstead::processes::ProcessRuntime::create(
        heartstead::core::ProcessId::from_value(302), build_piece.object_id,
        process_prototype.value(), 150, 2000);
    assert(second_build_process);
    assert(state.processes().insert(second_build_process.value()));

    heartstead::world::WorldSimulationSubjectOptions options;
    options.last_update_time_ms = 500;
    auto subjects = heartstead::world::derive_simulation_subjects(state, options);
    assert(subjects);
    assert(subjects.value().size() == 8);
    assert(subjects.value()[0].kind == heartstead::simulation::SimulationSubjectKind::entity);
    assert(subjects.value()[0].save_id == entity.save_id);
    assert(subjects.value()[0].runtime_handle == entity.runtime_handle);
    assert(subjects.value()[0].coord.x == 4);
    assert(subjects.value()[0].sleeping);
    assert(subjects.value()[0].last_update_time_ms == 500);
    assert(subjects.value()[1].kind == heartstead::simulation::SimulationSubjectKind::build_piece);
    assert(subjects.value()[1].save_id == build_piece.object_id);
    assert(subjects.value()[1].coord.x == 12);
    assert(subjects.value()[1].coord.z == -3);
    assert(subjects.value()[2].kind == heartstead::simulation::SimulationSubjectKind::assembly);
    assert(subjects.value()[2].save_id == assembly.assembly_id);
    assert(subjects.value()[2].prototype_id == assembly_prototype.value());
    assert(subjects.value()[2].coord.x == 12);
    assert(subjects.value()[2].coord.z == -3);
    assert(!subjects.value()[2].sleeping);
    assert(subjects.value()[2].last_update_time_ms == 500);
    assert(subjects.value()[3].kind ==
           heartstead::simulation::SimulationSubjectKind::process_owner);
    assert(subjects.value()[3].save_id == build_piece.object_id);
    assert(subjects.value()[3].process_id == heartstead::core::ProcessId::from_value(300));
    assert(subjects.value()[3].prototype_id == process_prototype.value());
    assert(subjects.value()[3].coord.x == 12);
    assert(subjects.value()[3].coord.z == -3);
    assert(subjects.value()[3].last_update_time_ms == 100);
    assert(subjects.value()[4].kind ==
           heartstead::simulation::SimulationSubjectKind::process_owner);
    assert(subjects.value()[4].save_id == cargo.cargo_id);
    assert(subjects.value()[4].process_id == heartstead::core::ProcessId::from_value(301));
    assert(subjects.value()[4].coord.x == -9);
    assert(subjects.value()[4].coord.z == 3);
    assert(subjects.value()[4].last_update_time_ms == 200);
    assert(subjects.value()[5].kind ==
           heartstead::simulation::SimulationSubjectKind::process_owner);
    assert(subjects.value()[5].save_id == build_piece.object_id);
    assert(subjects.value()[5].process_id == heartstead::core::ProcessId::from_value(302));
    assert(subjects.value()[5].coord.x == 12);
    assert(subjects.value()[5].coord.z == -3);
    assert(subjects.value()[5].last_update_time_ms == 150);
    assert(subjects.value()[6].kind == heartstead::simulation::SimulationSubjectKind::network);
    assert(!subjects.value()[6].persistent);
    assert(subjects.value()[6].runtime_handle.is_valid());
    assert(subjects.value()[6].coord.x == 2);
    assert(subjects.value()[6].coord.z == 0);
    assert(!subjects.value()[6].sleeping);
    assert(subjects.value()[7].kind == heartstead::simulation::SimulationSubjectKind::chunk_region);
    assert(!subjects.value()[7].persistent);
    assert(subjects.value()[7].runtime_handle.is_valid());
    assert(subjects.value()[7].coord.x == 0);
    assert(subjects.value()[7].coord.z == 0);
    assert(!subjects.value()[7].sleeping);

    heartstead::simulation::SimulationLodPolicy policy;
    policy.full_radius = 5;
    policy.simplified_radius = 16;
    policy.full_tick_interval_ms = 16;
    policy.simplified_tick_interval_ms = 1000;
    policy.sleeping_tick_interval_ms = 2000;
    const std::vector<heartstead::simulation::SimulationViewer> viewers{
        {heartstead::core::NetId::from_value(7), {0, 0, 0}}};
    auto plan = heartstead::simulation::SimulationLodPlanner::plan_frame(subjects.value(), viewers,
                                                                         policy, 2500);
    assert(plan);
    assert(plan.value().count(heartstead::simulation::SimulationLod::full) == 2);
    assert(plan.value().count(heartstead::simulation::SimulationLod::sleeping) == 1);
    assert(plan.value().count(heartstead::simulation::SimulationLod::simplified) == 5);
    assert(plan.value().due_tick_count == 8);
    assert(plan.value().decisions[3].process_id == heartstead::core::ProcessId::from_value(300));
    assert(plan.value().decisions[5].process_id == heartstead::core::ProcessId::from_value(302));

    heartstead::world::WorldSimulationFramePlanOptions frame_options;
    frame_options.subject_options = options;
    frame_options.viewers = viewers;
    frame_options.policy = policy;
    frame_options.now_ms = 2500;
    auto world_plan = heartstead::world::plan_world_simulation_frame(state, frame_options);
    assert(world_plan);
    assert(world_plan.value().decisions.size() == plan.value().decisions.size());
    assert(world_plan.value().full_count == plan.value().full_count);
    assert(world_plan.value().simplified_count == plan.value().simplified_count);
    assert(world_plan.value().sleeping_count == plan.value().sleeping_count);
    assert(world_plan.value().due_tick_count == plan.value().due_tick_count);
    assert(world_plan.value().decisions[5].process_id ==
           heartstead::core::ProcessId::from_value(302));

    heartstead::world::WorldReplicationInterestOptions interest_options;
    interest_options.subject_options = options;
    interest_options.viewers = {
        {heartstead::core::NetId::from_value(7), {0, 0, 0}},
        {heartstead::core::NetId::from_value(8), {1000, 0, 0}},
    };
    interest_options.policy = policy;
    interest_options.now_ms = 2500;
    auto interest_policy =
        heartstead::world::derive_replication_relevance_policy(state, interest_options);
    assert(interest_policy);
    assert(!interest_policy.value().broadcast_by_default);
    assert(interest_policy.value().client_rules.size() == 2);
    assert(interest_policy.value().client_rules[0].client_id ==
           heartstead::core::NetId::from_value(7));
    assert(interest_policy.value().client_rules[0].visible_subjects.size() == 5);
    assert(interest_policy.value().client_rules[0].visible_subjects[0] == build_piece.object_id);
    assert(interest_policy.value().client_rules[0].visible_subjects[1] == entity.save_id);
    assert(interest_policy.value().client_rules[0].visible_subjects[2] == cargo.cargo_id);
    assert(interest_policy.value().client_rules[0].visible_subjects[3] == assembly.assembly_id);
    assert(interest_policy.value().client_rules[0].visible_subjects[4] ==
           heartstead::core::SaveId::from_value(104));
    assert(interest_policy.value().client_rules[1].client_id ==
           heartstead::core::NetId::from_value(8));
    assert(interest_policy.value().client_rules[1].visible_subjects.empty());

    auto world_interest_report =
        heartstead::world::derive_replication_interest_report(state, interest_options);
    assert(world_interest_report);
    assert(world_interest_report.value().subject_count == 9);
    assert(world_interest_report.value().saved_subject_count == 7);
    assert(world_interest_report.value().non_saved_subject_count == 2);
    assert(world_interest_report.value().viewer_count == 2);
    assert(world_interest_report.value().policy.client_rules.size() == 2);
    assert(world_interest_report.value().policy.client_rules[0].visible_subjects ==
           interest_policy.value().client_rules[0].visible_subjects);
    assert(world_interest_report.value().viewer_reports.size() == 2);
    assert(world_interest_report.value().viewer_reports[0].viewer_id ==
           heartstead::core::NetId::from_value(7));
    assert(world_interest_report.value().viewer_reports[0].visible_subject_count == 5);
    assert(world_interest_report.value().viewer_reports[0].skipped_non_saved_subject_count == 2);
    assert(world_interest_report.value().viewer_reports[1].viewer_id ==
           heartstead::core::NetId::from_value(8));
    assert(world_interest_report.value().viewer_reports[1].visible_subject_count == 0);
    assert(world_interest_report.value().viewer_reports[1].excluded_lod_subject_count == 7);
    auto world_interest_inspection =
        heartstead::debug::Inspector::inspect(world_interest_report.value());
    assert(world_interest_inspection.object_type == "world_replication_interest_report");
    assert(world_interest_inspection.state == "active");
    assert(world_interest_inspection.find_field("subject_count")->value == "9");
    assert(world_interest_inspection.find_field("saved_subject_count")->value == "7");
    assert(world_interest_inspection.find_field("non_saved_subject_count")->value == "2");
    assert(world_interest_inspection.find_field("viewer_count")->value == "2");
    assert(world_interest_inspection.find_field("visible_subject_total")->value == "5");
    assert(world_interest_inspection.find_field("excluded_lod_subject_total")->value == "7");
    assert(world_interest_inspection.find_field("skipped_non_saved_subject_total")->value == "4");
    assert(world_interest_inspection.find_field("first_viewer_id")->value == "7");
    assert(world_interest_inspection.find_field("first_visible_subject_id")->value == "100");
    assert(!world_interest_inspection.has_errors());

    heartstead::net::ReplicationBatch build_event_batch;
    build_event_batch.command_sequence = 11;
    build_event_batch.command_type = "build.complete_piece";
    build_event_batch.events.push_back(
        {"build_piece.completed", build_piece.object_id, "completed"});
    auto interest_report = heartstead::net::ReplicationRelevance::evaluate(
        interest_policy.value(), build_event_batch,
        {heartstead::core::NetId::from_value(7), heartstead::core::NetId::from_value(8)});
    assert(interest_report.relevant_client_count == 1);
    assert(interest_report.filtered_client_count == 1);
    assert(interest_report.decisions[0].reason == "matched_subject");
    assert(interest_report.decisions[1].reason == "filtered_subject");

    heartstead::net::ReplicationBatch workpiece_event_batch;
    workpiece_event_batch.command_sequence = 12;
    workpiece_event_batch.command_type = "workpiece.edit_cell";
    workpiece_event_batch.events.push_back(
        {"workpiece.edited", heartstead::core::SaveId::from_value(104), "edited"});
    auto workpiece_interest = heartstead::net::ReplicationRelevance::evaluate(
        interest_policy.value(), workpiece_event_batch,
        {heartstead::core::NetId::from_value(7), heartstead::core::NetId::from_value(8)});
    assert(workpiece_interest.relevant_client_count == 1);
    assert(workpiece_interest.decisions[0].reason == "matched_subject");
    assert(workpiece_interest.decisions[1].reason == "filtered_subject");

    interest_options.include_sleeping = false;
    auto awake_interest_policy =
        heartstead::world::derive_replication_relevance_policy(state, interest_options);
    assert(awake_interest_policy);
    assert(awake_interest_policy.value().client_rules[0].visible_subjects.size() == 4);
    assert(std::ranges::find(awake_interest_policy.value().client_rules[0].visible_subjects,
                             entity.save_id) ==
           awake_interest_policy.value().client_rules[0].visible_subjects.end());

    interest_options.viewers.push_back({heartstead::core::NetId::from_value(7), {4, 0, 0}});
    auto duplicate_interest_policy =
        heartstead::world::derive_replication_relevance_policy(state, interest_options);
    assert(!duplicate_interest_policy);
    assert(duplicate_interest_policy.error().code == "replication_interest.duplicate_viewer");
    auto duplicate_interest_report =
        heartstead::world::derive_replication_interest_report(state, interest_options);
    assert(!duplicate_interest_report);
    assert(duplicate_interest_report.error().code == "replication_interest.duplicate_viewer");

    options.include_build_pieces = false;
    options.include_assemblies = false;
    options.include_processes = false;
    options.include_networks = false;
    options.include_chunk_regions = false;
    auto entity_only = heartstead::world::derive_simulation_subjects(state, options);
    assert(entity_only);
    assert(entity_only.value().size() == 1);
    assert(entity_only.value().front().kind ==
           heartstead::simulation::SimulationSubjectKind::entity);

    options.include_entities = false;
    options.include_processes = true;
    auto process_only = heartstead::world::derive_simulation_subjects(state, options);
    assert(process_only);
    assert(process_only.value().size() == 3);
    assert(process_only.value().front().kind ==
           heartstead::simulation::SimulationSubjectKind::process_owner);

    options.include_processes = false;
    options.include_assemblies = true;
    auto assembly_only = heartstead::world::derive_simulation_subjects(state, options);
    assert(assembly_only);
    assert(assembly_only.value().size() == 1);
    assert(assembly_only.value().front().kind ==
           heartstead::simulation::SimulationSubjectKind::assembly);

    options.include_assemblies = false;
    options.include_networks = true;
    auto network_only = heartstead::world::derive_simulation_subjects(state, options);
    assert(network_only);
    assert(network_only.value().size() == 1);
    assert(network_only.value().front().kind ==
           heartstead::simulation::SimulationSubjectKind::network);

    options.include_networks = false;
    options.include_chunk_regions = true;
    auto chunk_only = heartstead::world::derive_simulation_subjects(state, options);
    assert(chunk_only);
    assert(chunk_only.value().size() == 1);
    assert(chunk_only.value().front().kind ==
           heartstead::simulation::SimulationSubjectKind::chunk_region);

    heartstead::simulation::SimulationSubject far_time_subject;
    far_time_subject.save_id = heartstead::core::SaveId::from_value(99999);
    far_time_subject.last_update_time_ms = std::numeric_limits<std::uint64_t>::max();
    auto far_time = heartstead::simulation::SimulationLodPlanner::classify(
        far_time_subject, {}, {}, std::numeric_limits<std::uint64_t>::max());
    assert(far_time);
    assert(far_time.value().elapsed_since_update_ms == 0);
}

void test_world_replication_delta_planning() {
    heartstead::world::WorldState state;

    const auto build_prototype = heartstead::core::PrototypeId::parse("base:build_pieces/wall");
    const auto cargo_prototype = heartstead::core::PrototypeId::parse("base:cargo/heavy_log");
    const auto entity_prototype = heartstead::core::PrototypeId::parse("base:entities/cart");
    const auto assembly_prototype = heartstead::core::PrototypeId::parse("base:assemblies/kiln");
    const auto item_prototype = heartstead::core::PrototypeId::parse("base:items/raw_clay");
    const auto process_prototype = heartstead::core::PrototypeId::parse("base:processes/drying");
    assert(build_prototype);
    assert(cargo_prototype);
    assert(entity_prototype);
    assert(assembly_prototype);
    assert(item_prototype);
    assert(process_prototype);

    const auto build_id = heartstead::core::SaveId::from_value(10);
    heartstead::build::BuildPieceRecord build_piece;
    build_piece.object_id = build_id;
    build_piece.prototype_id = build_prototype.value();
    build_piece.construction_state = heartstead::build::ConstructionState::complete;
    assert(state.build_objects().insert(build_piece));

    auto stack = heartstead::items::ItemStack::create(item_prototype.value(), 3, 64);
    assert(stack);
    assert(state.inventories().insert({build_id, {stack.value()}}));

    auto drying = heartstead::processes::ProcessRuntime::create(
        heartstead::core::ProcessId::from_value(1), build_id, process_prototype.value(), 100, 1000);
    assert(drying);
    assert(state.processes().insert(drying.value()));
    auto smoking = heartstead::processes::ProcessRuntime::create(
        heartstead::core::ProcessId::from_value(2), build_id, process_prototype.value(), 200, 2000);
    assert(smoking);
    assert(state.processes().insert(smoking.value()));

    const auto cargo_id = heartstead::core::SaveId::from_value(20);
    heartstead::cargo::CargoRecord cargo;
    cargo.cargo_id = cargo_id;
    cargo.prototype_id = cargo_prototype.value();
    cargo.mass_grams = 90000;
    cargo.volume_milliliters = 180000;
    cargo.allowed_transport_modes =
        heartstead::cargo::CargoTransportModes::of({heartstead::cargo::CargoTransportMode::cart});
    assert(state.cargo().insert(cargo));

    const auto entity_id = heartstead::core::SaveId::from_value(30);
    auto runtime_handle = state.runtime_handles().reserve();
    assert(runtime_handle);
    heartstead::entities::EntityRecord entity;
    entity.runtime_handle = runtime_handle.value();
    entity.net_id = heartstead::core::NetId::from_value(70);
    entity.save_id = entity_id;
    entity.prototype_id = entity_prototype.value();
    entity.kind = heartstead::entities::EntityKind::cart;
    entity.persistent = true;
    entity.sleeping = true;
    entity.transform.position = {4.0, 0.0, 2.0};
    assert(state.entities().insert(entity));

    const auto assembly_id = heartstead::core::SaveId::from_value(40);
    heartstead::assemblies::AssemblyRecord assembly;
    assembly.assembly_id = assembly_id;
    assembly.root_build_piece_id = build_id;
    assembly.prototype_id = assembly_prototype.value();
    assembly.operating = true;
    assembly.state = heartstead::assemblies::AssemblyState::operating;
    assert(state.assemblies().insert(assembly));

    heartstead::net::ReplicationBatch batch;
    batch.command_sequence = 12;
    batch.command_type = "debug.delta|escaped=ok";
    batch.events.push_back({"world.voxel_changed", {}, "global|message=ok"});
    batch.events.push_back({"build_piece.completed", build_id, "wall"});
    batch.events.push_back({"inventory.items_transferred", build_id, "items"});
    batch.events.push_back({"cargo.created", cargo_id, "cargo"});
    batch.events.push_back({"entity.spawned", entity_id, "cart"});
    batch.events.push_back({"assembly.created", assembly_id, "kiln"});
    batch.events.push_back(
        {"assembly.created", heartstead::core::SaveId::from_value(999), "missing"});

    auto plan = heartstead::world::plan_replication_delta(state, batch);
    assert(plan.command_sequence == 12);
    assert(plan.command_type == "debug.delta|escaped=ok");
    assert(plan.event_count == 7);
    assert(plan.global_event_count == 1);
    assert(plan.subject_event_count == 6);
    assert(plan.unique_subject_count == 5);
    assert(plan.missing_subject_count == 1);
    assert(plan.materialized_record_count == 7);
    assert(plan.has_global_events);
    assert(plan.requires_snapshot_resync);
    assert(plan.global_events.size() == 1);
    assert(plan.global_events.front().type == "world.voxel_changed");
    assert(plan.subjects.size() == 5);
    assert(plan.subjects[0].subject_id == build_id);
    assert(plan.subjects[0].event_count == 2);
    assert(plan.subjects[0].has_build_piece);
    assert(!plan.subjects[0].has_entity);
    assert(!plan.subjects[0].has_cargo);
    assert(!plan.subjects[0].has_assembly);
    assert(plan.subjects[0].has_inventory);
    assert(plan.subjects[0].process_count == 2);
    assert(plan.subjects[0].materialized_record_count == 4);
    assert(!plan.subjects[0].missing_subject);
    assert(plan.subjects[1].subject_id == cargo_id);
    assert(plan.subjects[1].has_cargo);
    assert(plan.subjects[1].materialized_record_count == 1);
    assert(!plan.subjects[1].missing_subject);
    assert(plan.subjects[2].subject_id == entity_id);
    assert(plan.subjects[2].has_entity);
    assert(plan.subjects[2].materialized_record_count == 1);
    assert(!plan.subjects[2].missing_subject);
    assert(plan.subjects[3].subject_id == assembly_id);
    assert(plan.subjects[3].has_assembly);
    assert(plan.subjects[3].materialized_record_count == 1);
    assert(!plan.subjects[3].missing_subject);
    assert(plan.subjects[4].subject_id == heartstead::core::SaveId::from_value(999));
    assert(plan.subjects[4].missing_subject);
    assert(plan.subjects[4].materialized_record_count == 0);

    auto inspection = heartstead::debug::Inspector::inspect(plan);
    assert(inspection.object_type == "world_replication_delta_plan");
    assert(inspection.state == "needs_resync");
    assert(inspection.find_field("event_count")->value == "7");
    assert(inspection.find_field("global_event_count")->value == "1");
    assert(inspection.find_field("unique_subject_count")->value == "5");
    assert(inspection.find_field("missing_subject_count")->value == "1");
    assert(inspection.find_field("materialized_record_count")->value == "7");
    assert(inspection.find_field("first_global_event_type")->value == "world.voxel_changed");
    assert(inspection.find_field("first_subject_id")->value == "10");
    assert(inspection.find_field("first_subject_event_count")->value == "2");
    assert(inspection.find_field("first_subject_materialized_record_count")->value == "4");
    assert(inspection.find_field("first_subject_has_build_piece")->value == "true");
    assert(inspection.find_field("first_subject_has_inventory")->value == "true");
    assert(inspection.find_field("first_subject_process_count")->value == "2");
    assert(!inspection.has_errors());
    assert(std::ranges::any_of(inspection.issues, [](const auto& issue) {
        return issue.code == "world_replication_delta.missing_subjects";
    }));

    auto snapshot = heartstead::world::materialize_replication_delta(state, batch);
    assert(snapshot.plan.event_count == plan.event_count);
    assert(snapshot.plan.materialized_record_count == 7);
    assert(snapshot.build_pieces.size() == 1);
    assert(snapshot.build_pieces.front().object_id == build_id);
    assert(snapshot.entities.size() == 1);
    assert(snapshot.entities.front().save_id == entity_id);
    assert(snapshot.entities.front().kind == heartstead::entities::EntityKind::cart);
    assert(snapshot.entities.front().sleeping);
    assert(snapshot.entities.front().transform.position.approximate_global().x == 4.0);
    assert(snapshot.cargo_records.size() == 1);
    assert(snapshot.cargo_records.front().cargo_id == cargo_id);
    assert(snapshot.inventories.size() == 1);
    assert(snapshot.inventories.front().owner_id == build_id);
    assert(snapshot.inventories.front().stacks.size() == 1);
    assert(snapshot.assemblies.size() == 1);
    assert(snapshot.assemblies.front().assembly_id == assembly_id);
    assert(snapshot.processes.size() == 2);
    assert(snapshot.processes[0].process_id == heartstead::core::ProcessId::from_value(1));
    assert(snapshot.processes[1].process_id == heartstead::core::ProcessId::from_value(2));
    auto snapshot_inspection = heartstead::debug::Inspector::inspect(snapshot);
    assert(snapshot_inspection.object_type == "world_replication_delta_snapshot");
    assert(snapshot_inspection.state == "needs_resync");
    assert(snapshot_inspection.find_field("planned_materialized_record_count")->value == "7");
    assert(snapshot_inspection.find_field("section_record_count")->value == "7");
    assert(snapshot_inspection.find_field("build_piece_count")->value == "1");
    assert(snapshot_inspection.find_field("entity_count")->value == "1");
    assert(snapshot_inspection.find_field("cargo_count")->value == "1");
    assert(snapshot_inspection.find_field("inventory_count")->value == "1");
    assert(snapshot_inspection.find_field("assembly_count")->value == "1");
    assert(snapshot_inspection.find_field("process_count")->value == "2");
    assert(snapshot_inspection.find_field("first_build_piece_id")->value == "10");
    assert(snapshot_inspection.find_field("first_entity_id")->value == "30");
    assert(snapshot_inspection.find_field("first_cargo_id")->value == "20");
    assert(snapshot_inspection.find_field("first_inventory_owner_id")->value == "10");
    assert(snapshot_inspection.find_field("first_assembly_id")->value == "40");
    assert(snapshot_inspection.find_field("first_process_id")->value == "1");
    assert(!snapshot_inspection.has_errors());
    assert(std::ranges::any_of(snapshot_inspection.issues, [](const auto& issue) {
        return issue.code == "world_replication_delta_snapshot.requires_resync";
    }));

    const auto encoded_snapshot =
        heartstead::world::WorldReplicationDeltaSnapshotTextCodec::encode(snapshot);
    assert(encoded_snapshot.find("heartstead.replication_delta_snapshot.v1") != std::string::npos);
    assert(encoded_snapshot.find("snapshot_begin") != std::string::npos);
    auto decoded_snapshot =
        heartstead::world::WorldReplicationDeltaSnapshotTextCodec::decode(encoded_snapshot);
    assert(decoded_snapshot);
    assert(decoded_snapshot.value().plan.command_sequence == 12);
    assert(decoded_snapshot.value().plan.command_type == "debug.delta|escaped=ok");
    assert(decoded_snapshot.value().plan.global_events.size() == 1);
    assert(decoded_snapshot.value().plan.global_events.front().message == "global|message=ok");
    assert(decoded_snapshot.value().plan.subjects.size() == 5);
    assert(decoded_snapshot.value().plan.subjects.front().subject_id == build_id);
    assert(decoded_snapshot.value().build_pieces.size() == 1);
    assert(decoded_snapshot.value().build_pieces.front().object_id == build_id);
    assert(decoded_snapshot.value().entities.size() == 1);
    assert(decoded_snapshot.value().entities.front().save_id == entity_id);
    assert(decoded_snapshot.value().cargo_records.size() == 1);
    assert(decoded_snapshot.value().inventories.size() == 1);
    assert(decoded_snapshot.value().assemblies.size() == 1);
    assert(decoded_snapshot.value().processes.size() == 2);
    auto decoded_snapshot_inspection =
        heartstead::debug::Inspector::inspect(decoded_snapshot.value());
    assert(decoded_snapshot_inspection.object_type == "world_replication_delta_snapshot");
    assert(!decoded_snapshot_inspection.has_errors());
    auto binary_snapshot =
        heartstead::world::WorldReplicationDeltaSnapshotBinaryCodec::encode(snapshot);
    assert(binary_snapshot);
    assert(binary_snapshot.value().size() < encoded_snapshot.size());
    auto decoded_binary_snapshot =
        heartstead::world::WorldReplicationDeltaSnapshotBinaryCodec::decode(
            binary_snapshot.value());
    assert(decoded_binary_snapshot);
    assert(decoded_binary_snapshot.value().plan.command_sequence == 12);
    assert(decoded_binary_snapshot.value().plan.command_type == "debug.delta|escaped=ok");
    assert(decoded_binary_snapshot.value().plan.global_events.front().message ==
           "global|message=ok");
    assert(decoded_binary_snapshot.value().build_pieces.size() == 1);
    assert(!heartstead::world::WorldReplicationDeltaSnapshotBinaryCodec::decode(
        binary_snapshot.value().substr(0, binary_snapshot.value().size() - 1)));

    auto mismatched_payload = encoded_snapshot;
    const auto materialized_count_offset = mismatched_payload.find("materialized_record_count=7");
    assert(materialized_count_offset != std::string::npos);
    mismatched_payload.replace(materialized_count_offset,
                               std::string("materialized_record_count=7").size(),
                               "materialized_record_count=6");
    auto decoded_mismatched =
        heartstead::world::WorldReplicationDeltaSnapshotTextCodec::decode(mismatched_payload);
    assert(!decoded_mismatched);
    assert(decoded_mismatched.error().code == "replication_delta.count_mismatch");

    heartstead::net::HostSessionTickResult tick_result;
    heartstead::net::HostSessionCommandReport materialized_command;
    materialized_command.client_id = heartstead::core::NetId::from_value(7);
    materialized_command.sequence = 21;
    materialized_command.command_type = batch.command_type;
    materialized_command.success = true;
    materialized_command.committed_world_mutation = true;
    materialized_command.events = batch.events;
    tick_result.command_reports.push_back(materialized_command);

    heartstead::net::HostSessionCommandReport failed_command;
    failed_command.client_id = heartstead::core::NetId::from_value(7);
    failed_command.sequence = 22;
    failed_command.command_type = "debug.failed";
    failed_command.error_code = "debug.failed";
    failed_command.error_message = "failed";
    failed_command.operation_trace.stages = {
        heartstead::world::OperationStage::begun,
        heartstead::world::OperationStage::mutated,
        heartstead::world::OperationStage::rolled_back,
    };
    failed_command.operation_trace.mutations.push_back("debug mutation before rollback");
    failed_command.operation_trace.replication_dirty = true;
    failed_command.operation_trace.save_dirty = true;
    tick_result.command_reports.push_back(failed_command);

    heartstead::net::HostSessionCommandReport read_only_command;
    read_only_command.client_id = heartstead::core::NetId::from_value(7);
    read_only_command.sequence = 23;
    read_only_command.command_type = "debug.query";
    read_only_command.success = true;
    tick_result.command_reports.push_back(read_only_command);

    heartstead::net::HostSessionCommandReport eventless_command;
    eventless_command.client_id = heartstead::core::NetId::from_value(7);
    eventless_command.sequence = 24;
    eventless_command.command_type = "debug.eventless";
    eventless_command.success = true;
    eventless_command.committed_world_mutation = true;
    tick_result.command_reports.push_back(eventless_command);

    auto tick_delta_report =
        heartstead::world::materialize_replication_deltas_for_tick(state, tick_result);
    assert(tick_delta_report.command_report_count == 4);
    assert(tick_delta_report.materialized_command_count == 1);
    assert(tick_delta_report.skipped_command_count == 3);
    assert(tick_delta_report.total_event_count == 7);
    assert(tick_delta_report.total_materialized_record_count == 7);
    assert(tick_delta_report.requires_snapshot_resync);
    assert(tick_delta_report.commands.size() == 4);
    assert(!tick_delta_report.commands[0].skipped);
    assert(tick_delta_report.commands[0].snapshot.plan.command_sequence == 21);
    assert(tick_delta_report.commands[0].snapshot.build_pieces.size() == 1);
    assert(tick_delta_report.commands[1].skipped);
    assert(tick_delta_report.commands[1].skip_reason == "command_failed");
    assert(tick_delta_report.commands[1].error_code == "debug.failed");
    assert(tick_delta_report.commands[1].error_message == "failed");
    assert(tick_delta_report.commands[1].operation_trace.stages.back() ==
           heartstead::world::OperationStage::rolled_back);
    assert(tick_delta_report.commands[1].operation_trace.mutations.size() == 1);
    assert(tick_delta_report.commands[2].skipped);
    assert(tick_delta_report.commands[2].skip_reason == "not_mutating");
    assert(tick_delta_report.commands[3].skipped);
    assert(tick_delta_report.commands[3].skip_reason == "no_events");
    auto tick_delta_inspection = heartstead::debug::Inspector::inspect(tick_delta_report);
    assert(tick_delta_inspection.object_type == "world_replication_delta_tick_report");
    assert(tick_delta_inspection.state == "needs_resync");
    assert(tick_delta_inspection.find_field("command_report_count")->value == "4");
    assert(tick_delta_inspection.find_field("materialized_command_count")->value == "1");
    assert(tick_delta_inspection.find_field("skipped_command_count")->value == "3");
    assert(tick_delta_inspection.find_field("first_materialized_sequence")->value == "21");
    assert(tick_delta_inspection.find_field("first_skipped_sequence")->value == "22");
    assert(tick_delta_inspection.find_field("first_skipped_reason")->value == "command_failed");
    assert(tick_delta_inspection.find_field("first_skipped_error_code")->value == "debug.failed");
    assert(tick_delta_inspection.find_field("first_skipped_error_message")->value == "failed");
    assert(tick_delta_inspection.find_field("first_skipped_stage_count")->value == "3");
    assert(tick_delta_inspection.find_field("first_skipped_last_stage")->value == "rolled_back");
    assert(tick_delta_inspection.find_field("first_skipped_mutation_count")->value == "1");
    assert(tick_delta_inspection.find_field("first_skipped_replication_dirty")->value == "true");
    assert(tick_delta_inspection.find_field("first_skipped_save_dirty")->value == "true");
    assert(!tick_delta_inspection.has_errors());
    assert(std::ranges::any_of(tick_delta_inspection.issues, [](const auto& issue) {
        return issue.code == "world_replication_delta_tick.requires_resync";
    }));

    auto malformed_tick_delta = tick_delta_report;
    ++malformed_tick_delta.total_event_count;
    auto malformed_tick_delta_inspection =
        heartstead::debug::Inspector::inspect(malformed_tick_delta);
    assert(malformed_tick_delta_inspection.state == "invalid");
    assert(malformed_tick_delta_inspection.has_errors());
    assert(std::ranges::any_of(malformed_tick_delta_inspection.issues, [](const auto& issue) {
        return issue.code == "world_replication_delta_tick.count_mismatch";
    }));

    heartstead::world::WorldState rejected_apply_state;
    auto rejected_apply =
        heartstead::world::apply_replication_delta(rejected_apply_state, snapshot);
    assert(!rejected_apply);
    assert(rejected_apply.error().code == "replication_delta_apply.requires_resync");

    auto apply_batch = batch;
    apply_batch.events.pop_back();
    auto apply_snapshot = heartstead::world::materialize_replication_delta(state, apply_batch);
    assert(!apply_snapshot.plan.requires_snapshot_resync);
    assert(apply_snapshot.plan.event_count == 6);
    assert(apply_snapshot.plan.materialized_record_count == 7);

    const auto replication_server_id = heartstead::core::NetId::from_value(500);
    const auto replication_client_id = heartstead::core::NetId::from_value(501);
    auto delta_transport_result =
        heartstead::world::make_replication_delta_transport_message(apply_snapshot, 15);
    assert(delta_transport_result);
    auto delta_transport_message = std::move(delta_transport_result).value();
    assert(delta_transport_message.kind == heartstead::net::TransportMessageKind::replication);
    assert(delta_transport_message.channel == heartstead::net::TransportChannel::reliable);
    assert(delta_transport_message.sequence == 12);
    assert(delta_transport_message.payload_type ==
           heartstead::world::replication_delta_snapshot_payload_type);
    auto transported_delta = heartstead::world::replication_delta_snapshot_from_transport(
        heartstead::net::TransportEnvelope{
            replication_server_id,
            replication_client_id,
            delta_transport_message,
        });
    assert(transported_delta);
    assert(transported_delta.value().plan.command_sequence == 12);
    assert(transported_delta.value().plan.command_type == apply_batch.command_type);
    assert(transported_delta.value().build_pieces.size() == 1);

    auto wrong_delta_payload = delta_transport_message;
    wrong_delta_payload.payload_type =
        std::string(heartstead::net::replication_world_events_payload_type);
    auto wrong_delta_payload_result = heartstead::world::replication_delta_snapshot_from_transport(
        heartstead::net::TransportEnvelope{
            replication_server_id,
            replication_client_id,
            wrong_delta_payload,
        });
    assert(!wrong_delta_payload_result);
    assert(wrong_delta_payload_result.error().code == "replication_delta.unexpected_payload_type");

    auto unreliable_delta_payload = delta_transport_message;
    unreliable_delta_payload.channel = heartstead::net::TransportChannel::unreliable;
    auto unreliable_delta_result = heartstead::world::replication_delta_snapshot_from_transport(
        heartstead::net::TransportEnvelope{
            replication_server_id,
            replication_client_id,
            unreliable_delta_payload,
        });
    assert(!unreliable_delta_result);
    assert(unreliable_delta_result.error().code == "replication_delta.unreliable_replication");

    auto mismatched_delta_sequence = delta_transport_message;
    mismatched_delta_sequence.sequence = 99;
    auto mismatched_delta_result = heartstead::world::replication_delta_snapshot_from_transport(
        heartstead::net::TransportEnvelope{
            replication_server_id,
            replication_client_id,
            mismatched_delta_sequence,
        });
    assert(!mismatched_delta_result);
    assert(mismatched_delta_result.error().code == "replication_delta.sequence_mismatch");

    heartstead::net::HostSession delta_delivery_host;
    assert(delta_delivery_host.start());
    auto delivered_client = delta_delivery_host.connect_client();
    auto filtered_delta_client = delta_delivery_host.connect_client();
    assert(delivered_client);
    assert(filtered_delta_client);
    auto delivered_welcome_messages =
        delta_delivery_host.drain_client_messages(delivered_client.value());
    auto filtered_delta_welcome_messages =
        delta_delivery_host.drain_client_messages(filtered_delta_client.value());
    assert(delivered_welcome_messages);
    assert(delivered_welcome_messages.value().size() == 1);
    assert(filtered_delta_welcome_messages);
    assert(filtered_delta_welcome_messages.value().size() == 1);
    auto delta_delivery_policy = delta_delivery_host.replication_relevance_policy();
    delta_delivery_policy.private_access_rules.push_back({delivered_client.value(), {build_id}});
    delta_delivery_host.set_replication_relevance_policy(std::move(delta_delivery_policy));
    heartstead::net::ClientSession delivered_delta_protocol(delivered_client.value());
    assert(delivered_delta_protocol.receive_server_message(
        delivered_welcome_messages.value().front()));
    auto invalid_direct_replication = delta_delivery_host.send_replication_message(
        delivered_client.value(),
        heartstead::net::TransportMessage{heartstead::net::TransportMessageKind::command_result,
                                          heartstead::net::TransportChannel::reliable, 12,
                                          "debug.result", "", 80});
    assert(!invalid_direct_replication);
    assert(invalid_direct_replication.error().code == "host_session.not_replication_message");

    heartstead::net::HostSessionTickResult delivery_tick;
    heartstead::net::HostSessionCommandReport delivery_command;
    delivery_command.client_id = delivered_client.value();
    delivery_command.sequence = apply_batch.command_sequence;
    delivery_command.command_type = apply_batch.command_type;
    delivery_command.success = true;
    delivery_command.committed_world_mutation = true;
    delivery_command.events = apply_batch.events;
    delivery_tick.command_reports.push_back(delivery_command);

    heartstead::net::ReplicationRelevanceReport delivery_relevance;
    delivery_relevance.command_sequence = apply_batch.command_sequence;
    delivery_relevance.command_type = apply_batch.command_type;
    delivery_relevance.broadcast_by_default = false;
    delivery_relevance.event_count = static_cast<std::uint32_t>(apply_batch.events.size());
    delivery_relevance.candidate_client_count = 2;
    delivery_relevance.relevant_client_count = 1;
    delivery_relevance.filtered_client_count = 1;
    heartstead::net::ReplicationRelevanceDecision delivered_decision;
    delivered_decision.client_id = delivered_client.value();
    delivered_decision.relevant = true;
    delivered_decision.explicit_rule = true;
    delivered_decision.relevant_event_count = static_cast<std::uint32_t>(apply_batch.events.size());
    delivered_decision.reason = "matched_subject";
    delivery_relevance.decisions.push_back(delivered_decision);
    heartstead::net::ReplicationRelevanceDecision filtered_decision;
    filtered_decision.client_id = filtered_delta_client.value();
    filtered_decision.explicit_rule = true;
    filtered_decision.reason = "filtered_subject";
    delivery_relevance.decisions.push_back(filtered_decision);
    delivery_tick.replication_relevance_reports.push_back(delivery_relevance);

    auto delivery_delta_report =
        heartstead::world::materialize_replication_deltas_for_tick(state, delivery_tick);
    assert(delivery_delta_report.materialized_command_count == 1);
    assert(!delivery_delta_report.requires_snapshot_resync);
    auto delivery_report = heartstead::world::send_replication_delta_snapshots_for_tick(
        delta_delivery_host, delivery_delta_report, delivery_tick, 90);
    assert(delivery_report);
    assert(delivery_report.value().command_delta_count == 1);
    assert(delivery_report.value().materialized_command_count == 1);
    assert(delivery_report.value().skipped_command_count == 0);
    assert(delivery_report.value().relevance_report_count == 1);
    assert(delivery_report.value().sent_message_count == 1);
    assert(delivery_report.value().unmatched_relevance_count == 0);
    assert(delivery_report.value().commands.front().recipients.front() == delivered_client.value());
    auto delivery_inspection = heartstead::debug::Inspector::inspect(delivery_report.value());
    assert(delivery_inspection.object_type == "world_replication_delta_delivery_report");
    assert(delivery_inspection.state == "sent");
    assert(delivery_inspection.find_field("sent_message_count")->value == "1");
    assert(delivery_inspection.find_field("first_sent_sequence")->value == "12");
    assert(delivery_inspection.find_field("first_recipient_id")->value ==
           delivered_client.value().to_string());
    assert(!delivery_inspection.has_errors());

    auto delivered_messages = delta_delivery_host.drain_client_messages(delivered_client.value());
    assert(delivered_messages);
    assert(delivered_messages.value().size() == 1);
    assert(delivered_messages.value().front().message.kind ==
           heartstead::net::TransportMessageKind::replication);
    assert(delivered_messages.value().front().message.payload_type ==
           heartstead::world::replication_delta_snapshot_payload_type);
    auto decoded_delivered_delta = heartstead::world::replication_delta_snapshot_from_transport(
        delivered_messages.value().front());
    assert(decoded_delivered_delta);
    assert(decoded_delivered_delta.value().plan.command_sequence == 12);
    assert(decoded_delivered_delta.value().plan.command_type == apply_batch.command_type);
    assert(delivered_delta_protocol.receive_server_message(delivered_messages.value().front()));
    assert(delivered_delta_protocol.stats().queued_replication_message_count == 1);
    assert(delivered_delta_protocol.stats().first_queued_replication_message_payload_type ==
           heartstead::world::replication_delta_snapshot_payload_type);
    assert(delivered_delta_protocol.replication_intake_report().batch_count == 0);
    auto queued_delta_client_inspection =
        heartstead::debug::Inspector::inspect(delivered_delta_protocol);
    assert(queued_delta_client_inspection.find_field("queued_replication_message_count")->value ==
           "1");
    assert(
        queued_delta_client_inspection.find_field("first_queued_replication_message_payload_type")
            ->value == heartstead::world::replication_delta_snapshot_payload_type);
    assert(delivered_delta_protocol.receive_server_message(heartstead::net::TransportEnvelope{
        delta_delivery_host.server_id(),
        delivered_client.value(),
        heartstead::net::make_replication_transport_message(apply_batch, 95),
    }));
    assert(delivered_delta_protocol.replication_intake_report().batch_count == 1);
    heartstead::world::WorldState queued_delta_apply_state;
    auto queued_delta_apply = heartstead::world::apply_client_queued_replication_deltas(
        queued_delta_apply_state, delivered_delta_protocol);
    assert(queued_delta_apply);
    assert(queued_delta_apply.value().drained_batch_count == 1);
    assert(queued_delta_apply.value().delta_snapshot_count == 1);
    assert(queued_delta_apply.value().matched_delta_count == 1);
    assert(queued_delta_apply.value().applied_delta_count == 1);
    assert(queued_delta_apply.value().total_applied_record_count == 7);
    assert(queued_delta_apply_state.stats().build_object_count == 1);
    assert(queued_delta_apply_state.stats().persistent_entity_count == 1);
    assert(delivered_delta_protocol.stats().queued_replication_message_count == 0);
    assert(delivered_delta_protocol.replication_intake_report().batch_count == 0);
    assert(delivered_delta_protocol.receive_server_message(heartstead::net::TransportEnvelope{
        delta_delivery_host.server_id(),
        delivered_client.value(),
        heartstead::net::TransportMessage{heartstead::net::TransportMessageKind::replication,
                                          heartstead::net::TransportChannel::reliable, 13,
                                          "replication.debug_payload", "debug", 105},
    }));
    auto non_delta_snapshots =
        heartstead::world::drain_client_replication_delta_snapshots(delivered_delta_protocol);
    assert(non_delta_snapshots);
    assert(non_delta_snapshots.value().empty());
    assert(delivered_delta_protocol.stats().queued_replication_message_count == 1);
    auto leftover_replication_messages = delivered_delta_protocol.drain_replication_messages();
    assert(leftover_replication_messages.size() == 1);
    assert(leftover_replication_messages.front().message.payload_type ==
           "replication.debug_payload");

    auto filtered_delta_messages =
        delta_delivery_host.drain_client_messages(filtered_delta_client.value());
    assert(filtered_delta_messages);
    assert(filtered_delta_messages.value().empty());

    heartstead::net::HostSessionTickResult resync_delivery_tick;
    heartstead::net::ReplicationRelevanceReport resync_relevance;
    resync_relevance.command_sequence = tick_delta_report.commands.front().command_sequence;
    resync_relevance.command_type = tick_delta_report.commands.front().command_type;
    resync_relevance.event_count = tick_delta_report.commands.front().snapshot.plan.event_count;
    resync_relevance.candidate_client_count = 1;
    resync_relevance.relevant_client_count = 1;
    resync_relevance.decisions.push_back(delivered_decision);
    resync_delivery_tick.replication_relevance_reports.push_back(resync_relevance);
    auto resync_delivery_report = heartstead::world::send_replication_delta_snapshots_for_tick(
        delta_delivery_host, tick_delta_report, resync_delivery_tick, 100);
    assert(resync_delivery_report);
    assert(resync_delivery_report.value().sent_message_count == 0);
    assert(resync_delivery_report.value().resync_skipped_count == 1);
    assert(resync_delivery_report.value().commands.front().skipped);
    assert(resync_delivery_report.value().commands.front().skip_reason ==
           "requires_snapshot_resync");
    auto resync_delivery_inspection =
        heartstead::debug::Inspector::inspect(resync_delivery_report.value());
    assert(resync_delivery_inspection.state == "skipped");
    assert(std::ranges::any_of(resync_delivery_inspection.issues, [](const auto& issue) {
        return issue.code == "world_replication_delta_delivery.requires_resync";
    }));
    auto no_resync_delivery_messages =
        delta_delivery_host.drain_client_messages(delivered_client.value());
    assert(no_resync_delivery_messages);
    assert(no_resync_delivery_messages.value().empty());
    assert(delta_delivery_host.stop());

    heartstead::world::WorldState client_state;
    auto apply_report = heartstead::world::apply_replication_delta(client_state, apply_snapshot);
    assert(apply_report);
    assert(apply_report.value().applied);
    assert(apply_report.value().command_sequence == 12);
    assert(apply_report.value().event_count == 6);
    assert(apply_report.value().planned_record_count == 7);
    assert(apply_report.value().applied_record_count == 7);
    assert(apply_report.value().inserted_record_count == 7);
    assert(apply_report.value().updated_record_count == 0);
    assert(apply_report.value().build_pieces_inserted == 1);
    assert(apply_report.value().entities_inserted == 1);
    assert(apply_report.value().cargo_inserted == 1);
    assert(apply_report.value().inventories_inserted == 1);
    assert(apply_report.value().assemblies_inserted == 1);
    assert(apply_report.value().processes_inserted == 2);
    assert(client_state.stats().build_object_count == 1);
    assert(client_state.stats().persistent_entity_count == 1);
    assert(client_state.stats().cargo_count == 1);
    assert(client_state.stats().inventory_count == 1);
    assert(client_state.stats().assembly_count == 1);
    assert(client_state.stats().process_count == 2);
    assert(client_state.dirty_regions().count(heartstead::dirty::DirtyRegionKind::room_graph) == 1);
    auto* client_entity = client_state.entities().find_by_save_id(entity_id);
    assert(client_entity != nullptr);
    const auto client_entity_runtime = client_entity->runtime_handle;
    const auto client_entity_net_id = client_entity->net_id;
    assert(client_entity->save_id == entity_id);
    assert(client_entity->transform.position.approximate_global().x == 4.0);
    assert(client_state.runtime_handles().peek_next() ==
           heartstead::core::RuntimeHandle::from_value(2));
    assert(client_state.entity_net_ids().peek_next() ==
           heartstead::core::NetId::from_value(1000001));
    auto apply_inspection = heartstead::debug::Inspector::inspect(apply_report.value());
    assert(apply_inspection.object_type == "world_replication_delta_apply_report");
    assert(apply_inspection.state == "inserted");
    assert(apply_inspection.find_field("planned_record_count")->value == "7");
    assert(apply_inspection.find_field("applied_record_count")->value == "7");
    assert(apply_inspection.find_field("inserted_record_count")->value == "7");
    assert(apply_inspection.find_field("updated_record_count")->value == "0");
    assert(!apply_inspection.has_errors());

    heartstead::net::TransportServerWelcome replication_welcome{
        heartstead::net::transport_control_protocol_version,
        replication_server_id,
        replication_client_id,
        65535,
        4,
        true,
        true,
    };
    heartstead::net::ClientSession replication_client(replication_client_id);
    auto accepted_replication_client =
        replication_client.receive_server_message(heartstead::net::TransportEnvelope{
            replication_server_id,
            replication_client_id,
            heartstead::net::make_server_welcome_transport_message(replication_welcome, 10),
        });
    assert(accepted_replication_client);
    auto queued_replication_batch =
        replication_client.receive_server_message(heartstead::net::TransportEnvelope{
            replication_server_id,
            replication_client_id,
            heartstead::net::make_replication_transport_message(apply_batch, 20),
        });
    assert(queued_replication_batch);
    assert(replication_client.replication_intake_report().batch_count == 1);

    std::array<heartstead::world::WorldReplicationDeltaSnapshot, 1> decoded_client_deltas{
        transported_delta.value()};
    heartstead::world::WorldState client_adapter_state;
    auto client_apply_report = heartstead::world::apply_client_replication_deltas(
        client_adapter_state, replication_client,
        std::span<const heartstead::world::WorldReplicationDeltaSnapshot>(decoded_client_deltas));
    assert(client_apply_report);
    assert(client_apply_report.value().drained_batch_count == 1);
    assert(client_apply_report.value().delta_snapshot_count == 1);
    assert(client_apply_report.value().matched_delta_count == 1);
    assert(client_apply_report.value().applied_delta_count == 1);
    assert(client_apply_report.value().pending_delta_count == 0);
    assert(client_apply_report.value().observed_event_only_count == 0);
    assert(client_apply_report.value().unmatched_delta_count == 0);
    assert(client_apply_report.value().total_event_count == 6);
    assert(client_apply_report.value().total_applied_record_count == 7);
    assert(client_apply_report.value().batches.size() == 1);
    assert(client_apply_report.value().batches.front().state == "applied_delta");
    assert(client_apply_report.value().batches.front().has_delta_snapshot);
    assert(client_apply_report.value().batches.front().applied_delta);
    assert(client_adapter_state.stats().build_object_count == 1);
    assert(client_adapter_state.stats().persistent_entity_count == 1);
    assert(replication_client.replication_intake_report().batch_count == 0);
    auto client_apply_inspection =
        heartstead::debug::Inspector::inspect(client_apply_report.value());
    assert(client_apply_inspection.object_type == "world_client_replication_apply_report");
    assert(client_apply_inspection.state == "applied");
    assert(client_apply_inspection.find_field("drained_batch_count")->value == "1");
    assert(client_apply_inspection.find_field("applied_delta_count")->value == "1");
    assert(client_apply_inspection.find_field("total_applied_record_count")->value == "7");
    assert(client_apply_inspection.find_field("first_batch_state")->value == "applied_delta");
    assert(!client_apply_inspection.has_errors());

    const auto pending_client_id = heartstead::core::NetId::from_value(502);
    auto pending_welcome = replication_welcome;
    pending_welcome.assigned_client_id = pending_client_id;
    heartstead::net::ClientSession pending_client(pending_client_id);
    assert(pending_client.receive_server_message(heartstead::net::TransportEnvelope{
        replication_server_id,
        pending_client_id,
        heartstead::net::make_server_welcome_transport_message(pending_welcome, 30),
    }));
    assert(pending_client.receive_server_message(heartstead::net::TransportEnvelope{
        replication_server_id,
        pending_client_id,
        heartstead::net::make_replication_transport_message(apply_batch, 40),
    }));
    heartstead::world::WorldState pending_adapter_state;
    auto pending_apply_report = heartstead::world::apply_client_replication_deltas(
        pending_adapter_state, pending_client,
        std::span<const heartstead::world::WorldReplicationDeltaSnapshot>{});
    assert(pending_apply_report);
    assert(pending_apply_report.value().drained_batch_count == 1);
    assert(pending_apply_report.value().delta_snapshot_count == 0);
    assert(pending_apply_report.value().pending_delta_count == 1);
    assert(pending_apply_report.value().applied_delta_count == 0);
    assert(pending_adapter_state.stats().build_object_count == 0);
    auto pending_apply_inspection =
        heartstead::debug::Inspector::inspect(pending_apply_report.value());
    assert(pending_apply_inspection.state == "pending");
    assert(!pending_apply_inspection.has_errors());
    assert(std::ranges::any_of(pending_apply_inspection.issues, [](const auto& issue) {
        return issue.code == "world_client_replication_apply.pending_delta";
    }));

    const auto duplicate_client_id = heartstead::core::NetId::from_value(503);
    auto duplicate_welcome = replication_welcome;
    duplicate_welcome.assigned_client_id = duplicate_client_id;
    heartstead::net::ClientSession duplicate_delta_client(duplicate_client_id);
    assert(duplicate_delta_client.receive_server_message(heartstead::net::TransportEnvelope{
        replication_server_id,
        duplicate_client_id,
        heartstead::net::make_server_welcome_transport_message(duplicate_welcome, 50),
    }));
    assert(duplicate_delta_client.receive_server_message(heartstead::net::TransportEnvelope{
        replication_server_id,
        duplicate_client_id,
        heartstead::net::make_replication_transport_message(apply_batch, 60),
    }));
    std::array<heartstead::world::WorldReplicationDeltaSnapshot, 2> duplicate_client_deltas{
        apply_snapshot, apply_snapshot};
    heartstead::world::WorldState duplicate_adapter_state;
    auto duplicate_apply_report = heartstead::world::apply_client_replication_deltas(
        duplicate_adapter_state, duplicate_delta_client,
        std::span<const heartstead::world::WorldReplicationDeltaSnapshot>(duplicate_client_deltas));
    assert(!duplicate_apply_report);
    assert(duplicate_apply_report.error().code ==
           "world_client_replication.duplicate_delta_sequence");
    assert(duplicate_delta_client.replication_intake_report().batch_count == 1);

    auto* mutable_build = state.build_objects().find(build_id);
    assert(mutable_build != nullptr);
    mutable_build->transform.position = {5.0, 0.0, 0.0};
    auto* mutable_entity = state.entities().find_by_save_id(entity_id);
    assert(mutable_entity != nullptr);
    mutable_entity->sleeping = false;
    mutable_entity->transform.position = {8.0, 0.0, 0.0};
    auto* mutable_cargo = state.cargo().find(cargo_id);
    assert(mutable_cargo != nullptr);
    mutable_cargo->mass_grams = 91000;
    auto* mutable_inventory = state.inventories().find(build_id);
    assert(mutable_inventory != nullptr);
    mutable_inventory->stacks.front().count = 5;

    auto update_snapshot = heartstead::world::materialize_replication_delta(state, apply_batch);
    auto update_report = heartstead::world::apply_replication_delta(client_state, update_snapshot);
    assert(update_report);
    assert(update_report.value().applied_record_count == 7);
    assert(update_report.value().inserted_record_count == 0);
    assert(update_report.value().updated_record_count == 7);
    assert(update_report.value().build_pieces_updated == 1);
    assert(update_report.value().entities_updated == 1);
    assert(update_report.value().cargo_updated == 1);
    assert(update_report.value().inventories_updated == 1);
    assert(update_report.value().assemblies_updated == 1);
    assert(update_report.value().processes_updated == 2);
    client_entity = client_state.entities().find_by_save_id(entity_id);
    assert(client_entity != nullptr);
    assert(client_entity->runtime_handle == client_entity_runtime);
    assert(client_entity->net_id == client_entity_net_id);
    assert(!client_entity->sleeping);
    assert(client_entity->transform.position.approximate_global().x == 8.0);
    assert(client_state.build_objects().find(build_id)->transform.position.approximate_global().x ==
           5.0);
    assert(client_state.cargo().find(cargo_id)->mass_grams == 91000);
    assert(client_state.inventories().find(build_id)->stacks.front().count == 5);
    auto update_inspection = heartstead::debug::Inspector::inspect(update_report.value());
    assert(update_inspection.state == "updated");
    assert(update_inspection.find_field("updated_record_count")->value == "7");
    assert(!update_inspection.has_errors());

    auto malformed_apply_report = update_report.value();
    ++malformed_apply_report.applied_record_count;
    auto malformed_apply_inspection = heartstead::debug::Inspector::inspect(malformed_apply_report);
    assert(malformed_apply_inspection.state == "invalid");
    assert(malformed_apply_inspection.has_errors());
    assert(std::ranges::any_of(malformed_apply_inspection.issues, [](const auto& issue) {
        return issue.code == "world_replication_delta_apply.count_mismatch";
    }));

    auto malformed = plan;
    malformed.subject_event_count = 99;
    auto malformed_inspection = heartstead::debug::Inspector::inspect(malformed);
    assert(malformed_inspection.state == "invalid");
    assert(malformed_inspection.has_errors());
    assert(std::ranges::any_of(malformed_inspection.issues, [](const auto& issue) {
        return issue.code == "world_replication_delta.count_mismatch";
    }));

    auto malformed_snapshot = snapshot;
    malformed_snapshot.processes.pop_back();
    auto malformed_snapshot_inspection = heartstead::debug::Inspector::inspect(malformed_snapshot);
    assert(malformed_snapshot_inspection.state == "invalid");
    assert(malformed_snapshot_inspection.has_errors());
    assert(std::ranges::any_of(malformed_snapshot_inspection.issues, [](const auto& issue) {
        return issue.code == "world_replication_delta_snapshot.record_count_mismatch";
    }));
}

void test_spatial_network() {
    using namespace heartstead::networks;

    SpatialNetwork roads(NetworkKind::road);
    const auto a = NetworkNodeId::from_value(1);
    const auto b = NetworkNodeId::from_value(2);
    const auto c = NetworkNodeId::from_value(3);

    assert(roads.add_node(NetworkNode{a, {0, 0, 0}, 2, "camp"}));
    assert(roads.add_node(NetworkNode{b, {1, 0, 0}, 2, "bridge"}));
    assert(roads.add_node(NetworkNode{c, {2, 0, 0}, 2, "mine"}));
    assert(roads.is_dirty());
    roads.clear_dirty();
    assert(!roads.is_dirty());

    assert(roads.add_edge(NetworkEdge{NetworkEdgeId::from_value(10), a, b, 100, 2, false}));
    assert(roads.add_edge(NetworkEdge{NetworkEdgeId::from_value(11), b, c, 80, 1, false}));
    assert(roads.add_edge(NetworkEdge{NetworkEdgeId::from_value(12), a, c, 10, 1, true}));
    assert(roads.can_reach(a, c));
    assert(roads.neighbors(b).size() == 2);
    assert(roads.nodes().size() == 3);
    assert(roads.node_count() == 3);
    assert(roads.edge_count() == 3);
    assert(roads.blocked_edge_count() == 1);
    assert(roads.total_node_capacity() == 6);
    assert(roads.total_edge_capacity() == 4);

    auto missing = roads.add_edge(NetworkEdge{NetworkEdgeId::from_value(13), c,
                                              NetworkNodeId::from_value(99), 100, 1, false});
    assert(!missing);

    const auto road_owner = heartstead::core::SaveId::from_value(700);
    assert(roads.add_port(NetworkPort{NetworkPortId::from_value(20), b, "cart_access", 1,
                                      road_owner, heartstead::core::SaveId::from_value(701)}));
    assert(roads.find_port(NetworkPortId::from_value(20)) != nullptr);
    assert(roads.port_count() == 1);
    assert(roads.owned_port_count() == 1);
    assert(roads.sourced_port_count() == 1);
    assert(roads.port_count_for_owner(road_owner) == 1);
    assert(roads.total_port_capacity() == 1);
    assert(roads.total_port_capacity_for_owner(road_owner) == 1);
    assert(roads.total_port_capacity_for_owner(heartstead::core::SaveId::from_value(999)) == 0);

    heartstead::dirty::DirtyRegionTracker dirty_regions;
    roads.clear_dirty();
    auto marked = roads.mark_dirty_region(dirty_regions, {{0, 0, 0}, {2, 0, 0}}, "road edited");
    assert(marked);
    assert(roads.is_dirty());
    assert(dirty_regions.count(heartstead::dirty::DirtyRegionKind::road_network) == 1);
    assert(heartstead::networks::dirty_region_kind_for(NetworkKind::power) ==
           heartstead::dirty::DirtyRegionKind::power_network);
    assert(heartstead::networks::network_kind_name(NetworkKind::smoke_ventilation) ==
           "smoke_ventilation");
}

void test_chunk_database_records() {
    heartstead::world::ChunkDatabase chunks;
    auto& chunk = chunks.get_or_create({2, 0, -1});
    assert(chunk.coord().x == 2);
    assert(chunk.coord().z == -1);
    auto records = chunks.records();
    assert(records.size() == 1);
    assert(records.front()->coord().x == 2);
    assert(records.front()->coord().z == -1);
}

void test_spatial_network_derivation() {
    using namespace heartstead::networks;

    const auto build_prototype = heartstead::core::PrototypeId::parse("base:build_pieces/test");
    const auto assembly_prototype = heartstead::core::PrototypeId::parse("base:assemblies/test");
    assert(build_prototype);
    assert(assembly_prototype);

    heartstead::build::BuildPieceRecord storage_piece;
    storage_piece.object_id = heartstead::core::SaveId::from_value(100);
    storage_piece.prototype_id = build_prototype.value();
    storage_piece.construction_state = heartstead::build::ConstructionState::complete;
    storage_piece.transform.position = {4.5, 0.0, 2.0};
    storage_piece.network_ports.push_back({"storage", NetworkKind::storage_access, 3});

    heartstead::build::BuildPieceRecord smoke_piece;
    smoke_piece.object_id = heartstead::core::SaveId::from_value(101);
    smoke_piece.prototype_id = build_prototype.value();
    smoke_piece.construction_state = heartstead::build::ConstructionState::complete;
    smoke_piece.transform.position = {8.0, 0.0, 2.0};
    smoke_piece.network_ports.push_back({"smoke_output", NetworkKind::smoke_ventilation, 1});

    heartstead::build::BuildPieceRecord planned_piece;
    planned_piece.object_id = heartstead::core::SaveId::from_value(102);
    planned_piece.prototype_id = build_prototype.value();
    planned_piece.construction_state = heartstead::build::ConstructionState::planned;
    planned_piece.network_ports.push_back({"power", NetworkKind::power, 5});

    heartstead::assemblies::AssemblyRecord assembly;
    assembly.assembly_id = heartstead::core::SaveId::from_value(200);
    assembly.root_build_piece_id = storage_piece.object_id;
    assembly.prototype_id = assembly_prototype.value();
    assembly.ports.push_back({"storage", NetworkKind::storage_access, storage_piece.object_id, 2});
    assembly.ports.push_back(
        {"smoke_output", NetworkKind::smoke_ventilation, smoke_piece.object_id, 1});

    SpatialNetworkDerivationInput input;
    input.build_pieces = {&storage_piece, &smoke_piece, &planned_piece};
    input.assemblies = {&assembly};

    auto derived = SpatialNetworkDeriver::derive(input);
    assert(derived);
    assert(derived.value().stats.network_count == 2);
    assert(derived.value().stats.build_piece_port_count == 2);
    assert(derived.value().stats.assembly_port_count == 2);
    assert(derived.value().stats.skipped_incomplete_build_piece_count == 1);
    assert(derived.value().stats.generated_node_count == 4);
    assert(derived.value().stats.generated_port_count == 4);
    assert(derived.value().stats.generated_edge_count == 2);

    const auto storage_network =
        std::ranges::find_if(derived.value().networks, [](const SpatialNetwork& network) {
            return network.kind() == NetworkKind::storage_access;
        });
    assert(storage_network != derived.value().networks.end());
    assert(!storage_network->is_dirty());
    assert(storage_network->node_count() == 2);
    assert(storage_network->edge_count() == 1);
    assert(storage_network->port_count() == 2);
    assert(storage_network->owned_port_count() == 2);
    assert(storage_network->sourced_port_count() == 2);
    assert(storage_network->total_node_capacity() == 5);
    assert(storage_network->total_edge_capacity() == 2);
    assert(storage_network->total_port_capacity() == 5);
    assert(storage_network->port_count_for_owner(storage_piece.object_id) == 1);
    assert(storage_network->port_count_for_owner(assembly.assembly_id) == 1);
    assert(storage_network->total_port_capacity_for_owner(storage_piece.object_id) == 3);
    assert(storage_network->total_port_capacity_for_owner(assembly.assembly_id) == 2);

    const auto smoke_network =
        std::ranges::find_if(derived.value().networks, [](const SpatialNetwork& network) {
            return network.kind() == NetworkKind::smoke_ventilation;
        });
    assert(smoke_network != derived.value().networks.end());
    assert(smoke_network->node_count() == 2);
    assert(smoke_network->edge_count() == 1);

    auto network_inspection = heartstead::debug::Inspector::inspect(*storage_network);
    assert(network_inspection.find_field("total_node_capacity")->value == "5");
    assert(network_inspection.find_field("total_edge_capacity")->value == "2");
    assert(network_inspection.find_field("total_port_capacity")->value == "5");
    assert(network_inspection.find_field("owned_port_count")->value == "2");
    assert(network_inspection.find_field("sourced_port_count")->value == "2");

    auto missing_source = assembly;
    missing_source.ports.front().source_build_piece_id = heartstead::core::SaveId::from_value(9999);
    input.assemblies = {&missing_source};
    auto missing = SpatialNetworkDeriver::derive(input);
    assert(!missing);
    assert(missing.error().code == "network_derivation.missing_assembly_port_source");
}

void test_room_graph_descriptors() {
    using namespace heartstead::rooms;

    RoomGraph graph;

    RoomRecord warm_room;
    warm_room.id = RoomId::from_value(1);
    warm_room.label = "Smithy";
    warm_room.volume_cells = 200;
    warm_room.source_build_piece_ids.push_back(heartstead::core::SaveId::from_value(44));
    warm_room.source_build_piece_ids.push_back(heartstead::core::SaveId::from_value(45));
    warm_room.metrics.enclosure_per_mille = 900;
    warm_room.metrics.roof_coverage_per_mille = 900;
    warm_room.metrics.wall_coverage_per_mille = 850;
    warm_room.metrics.warmth = 300;
    warm_room.metrics.dryness = 300;
    warm_room.metrics.light_per_mille = 800;
    warm_room.metrics.smoke_per_mille = 0;
    warm_room.metrics.ventilation_per_mille = 700;
    warm_room.metrics.safety_per_mille = 900;
    warm_room.metrics.spaciousness_per_mille = 800;
    warm_room.metrics.storage_access = true;
    warm_room.metrics.cart_access = true;
    warm_room.metrics.power_access = true;
    warm_room.metrics.ward_coverage = true;

    assert(graph.add_or_replace(warm_room));
    assert(graph.is_dirty());
    graph.evaluate_all();
    assert(!graph.is_dirty());

    heartstead::dirty::DirtyRegionTracker dirty_regions;
    auto marked = graph.mark_dirty_region(dirty_regions, {{0, 0, 0}, {4, 2, 4}}, "wall placed");
    assert(marked);
    assert(graph.is_dirty());
    assert(dirty_regions.count(heartstead::dirty::DirtyRegionKind::room_graph) == 1);
    graph.clear_dirty();

    const auto* found = graph.find(RoomId::from_value(1));
    assert(found != nullptr);
    assert(found->has_descriptor("enclosed"));
    assert(found->has_descriptor("warm"));
    assert(found->has_descriptor("dry"));
    assert(found->has_descriptor("bright"));
    assert(found->has_descriptor("ventilated"));
    assert(found->has_descriptor("storage_access"));
    assert(found->has_descriptor("cart_access"));
    assert(found->has_descriptor("power_access"));
    assert(found->has_descriptor("warded"));
    assert(graph.count_descriptor("warm") == 1);

    auto room_inspection = heartstead::debug::Inspector::inspect(*found);
    assert(room_inspection.object_type == "room");
    assert(room_inspection.runtime_id == "1");
    assert(!room_inspection.has_errors());
    const auto* descriptor_count = room_inspection.find_field("descriptor_count");
    assert(descriptor_count != nullptr);
    assert(descriptor_count->value == "10");

    RoomRecord smoky_room;
    smoky_room.id = RoomId::from_value(2);
    smoky_room.label = "Damp Kitchen";
    smoky_room.volume_cells = 80;
    smoky_room.metrics.enclosure_per_mille = 700;
    smoky_room.metrics.roof_coverage_per_mille = 400;
    smoky_room.metrics.wall_coverage_per_mille = 500;
    smoky_room.metrics.warmth = -300;
    smoky_room.metrics.dryness = -300;
    smoky_room.metrics.light_per_mille = 100;
    smoky_room.metrics.smoke_per_mille = 800;
    smoky_room.metrics.ventilation_per_mille = 100;
    smoky_room.metrics.safety_per_mille = 100;
    smoky_room.metrics.spaciousness_per_mille = 100;

    assert(graph.add_or_replace(smoky_room));
    graph.evaluate_all();
    found = graph.find(RoomId::from_value(2));
    assert(found != nullptr);
    assert(found->has_descriptor("exposed"));
    assert(found->has_descriptor("smoky"));
    assert(found->has_descriptor("unsafe"));
    assert(found->has_descriptor("crowded"));
    assert(found->has_descriptor("poor_cart_access"));
    assert(graph.count_descriptor("poor_cart_access") == 1);

    room_inspection = heartstead::debug::Inspector::inspect(*found);
    assert(room_inspection.state == "exposed");
    assert(!room_inspection.issues.empty());

    auto graph_inspection = heartstead::debug::Inspector::inspect(graph);
    assert(graph_inspection.object_type == "room_graph");
    assert(graph_inspection.state == "derived");
    assert(graph_inspection.find_field("room_count")->value == "2");
    assert(graph_inspection.find_field("total_volume_cells")->value == "280");
    assert(graph_inspection.find_field("source_build_piece_ref_count")->value == "2");
    assert(graph_inspection.find_field("descriptor_count")->value == "18");
    assert(graph_inspection.find_field("positive_descriptor_count")->value == "10");
    assert(graph_inspection.find_field("neutral_descriptor_count")->value == "1");
    assert(graph_inspection.find_field("warning_descriptor_count")->value == "7");
    assert(graph_inspection.find_field("exposed_room_count")->value == "1");
    assert(graph_inspection.find_field("smoky_room_count")->value == "1");
    assert(graph_inspection.find_field("poor_cart_access_room_count")->value == "1");
    assert(graph_inspection.find_field("storage_access_room_count")->value == "1");
    assert(graph_inspection.find_field("cart_access_room_count")->value == "1");
    assert(graph_inspection.find_field("power_access_room_count")->value == "1");
    assert(graph_inspection.find_field("warded_room_count")->value == "1");
    assert(graph_inspection.find_field("first_room_id")->value == "1");
    assert(graph_inspection.find_field("first_room_descriptor_count")->value == "10");
    assert(!graph_inspection.has_errors());
    assert(!graph_inspection.issues.empty());

    RoomRecord invalid_room;
    invalid_room.id = RoomId::from_value(3);
    invalid_room.metrics.enclosure_per_mille = 1001;
    assert(!graph.add_or_replace(invalid_room));
}

void test_room_extraction() {
    using namespace heartstead::rooms;

    const auto contains_save_id = [](const std::vector<heartstead::core::SaveId>& ids,
                                     heartstead::core::SaveId expected) {
        return std::ranges::any_of(
            ids, [expected](heartstead::core::SaveId candidate) { return candidate == expected; });
    };

    auto enclosed_grid = RoomExtractionGrid::create({5, 3, 5});
    assert(enclosed_grid);
    auto grid = std::move(enclosed_grid).value();

    const auto wall_id = heartstead::core::SaveId::from_value(100);
    for (std::uint16_t y = 0; y < 3; ++y) {
        for (std::uint16_t z = 0; z < 5; ++z) {
            for (std::uint16_t x = 0; x < 5; ++x) {
                RoomExtractionCell cell;
                const bool border = x == 0 || x == 4 || y == 0 || y == 2 || z == 0 || z == 4;
                cell.solid = border;
                cell.roofed = !border;
                cell.warmth = 300;
                cell.dryness = 300;
                cell.light_per_mille = 800;
                cell.ventilation_per_mille = 700;
                cell.safety_per_mille = 900;
                cell.storage_access = x == 2 && y == 1 && z == 2;
                cell.cart_access = x == 2 && y == 1 && z == 2;
                cell.power_access = x == 2 && y == 1 && z == 2;
                if (border) {
                    cell.source_build_piece_ids.push_back(wall_id);
                }
                assert(grid.set_cell({x, y, z}, cell));
            }
        }
    }

    auto graph = RoomExtractor::extract(grid);
    assert(graph);
    assert(graph.value().room_count() == 1);
    const auto* room = graph.value().find(RoomId::from_value(1));
    assert(room != nullptr);
    assert(room->volume_cells == 9);
    assert(room->metrics.enclosure_per_mille == 1000);
    assert(room->metrics.roof_coverage_per_mille == 1000);
    assert(room->source_build_piece_ids.size() == 1);
    assert(room->source_build_piece_ids.front() == wall_id);
    assert(room->has_descriptor("enclosed"));
    assert(room->has_descriptor("storage_access"));
    assert(room->has_descriptor("cart_access"));

    auto open_grid_result = RoomExtractionGrid::create({3, 1, 3});
    assert(open_grid_result);
    auto open_grid = std::move(open_grid_result).value();
    for (std::uint16_t z = 0; z < 3; ++z) {
        for (std::uint16_t x = 0; x < 3; ++x) {
            RoomExtractionCell cell;
            cell.smoke_per_mille = 800;
            cell.ventilation_per_mille = 100;
            cell.safety_per_mille = 250;
            assert(open_grid.set_cell({x, 0, z}, cell));
        }
    }

    graph = RoomExtractor::extract(open_grid);
    assert(graph);
    assert(graph.value().room_count() == 1);
    room = graph.value().find(RoomId::from_value(1));
    assert(room != nullptr);
    assert(room->metrics.enclosure_per_mille < 850);
    assert(room->has_descriptor("exposed"));
    assert(room->has_descriptor("smoky"));
    assert(room->has_descriptor("unsafe"));

    assert(!RoomExtractionGrid::create({0, 1, 1}));
    assert(!RoomExtractor::extract(open_grid, RoomExtractionConfig{RoomId{}, 1}));

    auto builder_result = RoomExtractionGridBuilder::create({5, 3, 5});
    assert(builder_result);
    auto builder = std::move(builder_result).value();

    const auto wall_prototype =
        heartstead::core::PrototypeId::parse("base:build_pieces/wall_frame");
    assert(wall_prototype);

    heartstead::build::BuildPieceRecord wall_piece;
    wall_piece.object_id = heartstead::core::SaveId::from_value(200);
    wall_piece.prototype_id = wall_prototype.value();
    wall_piece.construction_state = heartstead::build::ConstructionState::complete;
    wall_piece.room_contribution_tags = {"wall", "enclosure"};

    heartstead::build::BuildPieceRecord roof_piece;
    roof_piece.object_id = heartstead::core::SaveId::from_value(201);
    roof_piece.prototype_id = wall_prototype.value();
    roof_piece.construction_state = heartstead::build::ConstructionState::complete;
    roof_piece.room_contribution_tags = {"roof"};

    heartstead::build::BuildPieceRecord access_piece;
    access_piece.object_id = heartstead::core::SaveId::from_value(202);
    access_piece.prototype_id = wall_prototype.value();
    access_piece.construction_state = heartstead::build::ConstructionState::complete;
    access_piece.network_ports.push_back(
        {"storage", heartstead::networks::NetworkKind::storage_access, 1});
    access_piece.network_ports.push_back(
        {"cart", heartstead::networks::NetworkKind::cart_access, 1});
    access_piece.network_ports.push_back({"power", heartstead::networks::NetworkKind::power, 1});
    access_piece.network_ports.push_back({"ward", heartstead::networks::NetworkKind::ward, 1});

    for (std::uint16_t z = 0; z < 5; ++z) {
        for (std::uint16_t x = 0; x < 5; ++x) {
            assert(builder.apply_terrain_voxel({x, 0, z}, heartstead::world::VoxelCell{1, 0}));
            assert(builder.apply_build_piece(roof_piece, {x, 2, z}));
            if (x == 0 || x == 4 || z == 0 || z == 4) {
                assert(builder.apply_build_piece(wall_piece, {x, 1, z}));
            } else {
                assert(
                    builder.apply_terrain_voxel({x, 1, z}, heartstead::world::VoxelCell{0, 255}));
            }
        }
    }
    assert(builder.apply_build_piece(access_piece, {2, 1, 2}));

    graph = RoomExtractor::extract(builder.grid());
    assert(graph);
    assert(graph.value().room_count() == 1);
    room = graph.value().find(RoomId::from_value(1));
    assert(room != nullptr);
    assert(room->volume_cells == 9);
    assert(room->metrics.enclosure_per_mille == 1000);
    assert(room->metrics.roof_coverage_per_mille == 1000);
    assert(room->metrics.storage_access);
    assert(room->metrics.cart_access);
    assert(room->metrics.power_access);
    assert(room->metrics.ward_coverage);
    assert(contains_save_id(room->source_build_piece_ids, wall_piece.object_id));
    assert(contains_save_id(room->source_build_piece_ids, roof_piece.object_id));
    assert(contains_save_id(room->source_build_piece_ids, access_piece.object_id));
    assert(room->has_descriptor("enclosed"));
    assert(room->has_descriptor("storage_access"));
    assert(room->has_descriptor("cart_access"));
    assert(room->has_descriptor("power_access"));
    assert(room->has_descriptor("warded"));
}

void test_world_operation_transaction() {
    heartstead::save::SaveIdAllocator allocator(100);
    heartstead::world::WorldOperation operation("place_build_piece");

    assert(operation.validate(true, "placement valid"));
    auto reserved_id = operation.reserve_save_id(allocator);
    assert(reserved_id);
    assert(operation.record_mutation("insert build object"));
    operation.record_derived_update("RoomGraph");
    operation.emit_event({"build_piece.placed", reserved_id.value(), "placed wall frame"});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();

    auto committed = operation.commit();
    assert(committed);
    assert(operation.is_committed());
    assert(operation.reserved_ids().size() == 1);
    assert(operation.mutations().size() == 1);
    assert(operation.derived_updates().size() == 1);
    assert(operation.events().size() == 1);
    assert(operation.replication_dirty());
    assert(operation.save_dirty());
    assert(heartstead::world::operation_stage_name(operation.stages().back()) == "committed");
    auto operation_inspection = heartstead::debug::Inspector::inspect(operation);
    assert(operation_inspection.object_type == "world_operation");
    assert(operation_inspection.state == "committed");
    assert(operation_inspection.find_field("mutation_count")->value == "1");
    assert(operation_inspection.find_field("derived_update_count")->value == "1");
    assert(operation_inspection.find_field("replication_dirty")->value == "true");
    assert(operation_inspection.find_field("save_dirty")->value == "true");

    heartstead::world::WorldOperation invalid_operation("transfer_items");
    assert(invalid_operation.record_mutation("move item stack"));
    invalid_operation.mark_save_dirty();
    auto invalid_commit = invalid_operation.commit();
    assert(!invalid_commit);
    assert(invalid_operation.has_failed());
    auto invalid_inspection = heartstead::debug::Inspector::inspect(invalid_operation);
    assert(invalid_inspection.state == "failed");
    assert(invalid_inspection.has_errors());
    assert(invalid_inspection.find_field("replication_dirty")->value == "false");

    heartstead::world::WorldOperation eventless_operation("silent_mutation");
    assert(eventless_operation.record_mutation("mutate without event"));
    eventless_operation.mark_replication_dirty();
    eventless_operation.mark_save_dirty();
    auto eventless_commit = eventless_operation.commit();
    assert(!eventless_commit);
    assert(eventless_commit.error().code == "operation.no_event");
    assert(eventless_operation.has_failed());
}

void test_server_command_dispatcher() {
    const auto wall_id = heartstead::core::PrototypeId::parse("base:build_pieces/wall_frame");
    assert(wall_id);

    heartstead::modding::GenericPrototype wall_prototype;
    wall_prototype.kind = std::string(heartstead::modding::PrototypeKinds::build_piece);
    wall_prototype.id = wall_id.value();
    wall_prototype.display_name = "Wall Frame";
    wall_prototype.fields.emplace("material_tags", "wood,frame");
    wall_prototype.fields.emplace("room_contribution_tags", "wall,enclosure");
    wall_prototype.fields.emplace("network_ports", "storage_access,smoke_output");

    heartstead::modding::PrototypeRegistry registry;
    auto registry_result = registry.build({wall_prototype});
    assert(!registry_result.has_errors());

    heartstead::net::ServerCommandDispatcher dispatcher;
    auto registered = dispatcher.register_command(heartstead::net::CommandDescriptor{
        "build.place_piece",
        true,
        true,
        [](const heartstead::net::CommandEnvelope& envelope,
           const heartstead::net::CommandExecutionContext& context,
           heartstead::world::WorldOperation& operation) {
            if (!envelope.sender.is_valid()) {
                return heartstead::core::Status::failure("command.invalid_sender",
                                                         "sender net id is required");
            }
            if (context.save_ids == nullptr) {
                return heartstead::core::Status::failure("command.missing_save_ids",
                                                         "save id allocator is required");
            }
            if (context.prototypes == nullptr) {
                return heartstead::core::Status::failure("command.missing_registry",
                                                         "prototype registry is required");
            }

            const auto prototype_id = heartstead::core::PrototypeId::parse(envelope.payload);
            if (!prototype_id) {
                return heartstead::core::Status::failure("command.invalid_payload",
                                                         "payload must be a prototype id");
            }

            auto prototype_status = context.prototypes->require_kind(
                prototype_id.value(), heartstead::modding::PrototypeKinds::build_piece);
            if (!prototype_status) {
                return prototype_status;
            }

            auto reserved_id = operation.reserve_save_id(*context.save_ids);
            if (!reserved_id) {
                return heartstead::core::Status::failure(reserved_id.error().code,
                                                         reserved_id.error().message);
            }

            auto mutation = operation.record_mutation("place build piece " + envelope.payload);
            if (!mutation) {
                return mutation;
            }
            operation.record_derived_update("RoomGraph");
            operation.emit_event({"build_piece.placed", reserved_id.value(), envelope.payload});
            operation.mark_replication_dirty();
            operation.mark_save_dirty();
            return heartstead::core::Status::ok();
        },
    });
    assert(registered);
    assert(dispatcher.contains("build.place_piece"));

    heartstead::save::SaveIdAllocator save_ids(1000);
    const heartstead::net::CommandEnvelope envelope{
        55,
        heartstead::core::NetId::from_value(22),
        "build.place_piece",
        "base:build_pieces/wall_frame",
        12345,
    };

    const heartstead::net::CommandExecutionContext client_context{
        heartstead::net::CommandExecutorRole::client_prediction,
        13000,
        &save_ids,
        &registry,
    };
    auto client_result = dispatcher.dispatch(envelope, client_context);
    assert(!client_result);

    const heartstead::net::CommandExecutionContext server_context{
        heartstead::net::CommandExecutorRole::authoritative_server,
        13000,
        &save_ids,
        &registry,
    };
    auto server_result = dispatcher.dispatch(envelope, server_context);
    assert(server_result);
    assert(server_result.value().sequence == 55);
    assert(server_result.value().committed_world_mutation);
    assert(server_result.value().events.size() == 1);
    assert(server_result.value().reserved_ids.size() == 1);
    assert(server_result.value().reserved_ids.front().value() == 1000);
    assert(server_result.value().operation_trace.stages.back() ==
           heartstead::world::OperationStage::committed);
    assert(server_result.value().operation_trace.mutations.size() == 1);
    assert(server_result.value().operation_trace.mutations.front() ==
           "place build piece base:build_pieces/wall_frame");
    assert(server_result.value().operation_trace.derived_updates.size() == 1);
    assert(server_result.value().operation_trace.derived_updates.front() == "RoomGraph");
    assert(server_result.value().operation_trace.replication_dirty);
    assert(server_result.value().operation_trace.save_dirty);

    auto bad_payload = envelope;
    bad_payload.payload = "base:items/raw_clay";
    auto bad_result = dispatcher.dispatch(bad_payload, server_context);
    assert(!bad_result);
    auto bad_report = dispatcher.dispatch_report(bad_payload, server_context);
    assert(!bad_report.succeeded);
    assert(bad_report.error.has_value());
    assert(bad_report.operation_trace.stages.back() ==
           heartstead::world::OperationStage::rolled_back);
    assert(bad_report.operation_trace.mutations.empty());
    assert(!bad_report.committed_world_mutation);

    auto failing_registered = dispatcher.register_command(heartstead::net::CommandDescriptor{
        "debug.fail_after_mutation",
        true,
        true,
        [](const heartstead::net::CommandEnvelope& command_envelope,
           const heartstead::net::CommandExecutionContext&,
           heartstead::world::WorldOperation& operation) {
            auto mutation =
                operation.record_mutation("partial mutation " + command_envelope.payload);
            if (!mutation) {
                return mutation;
            }
            operation.record_derived_update("DebugOnly");
            operation.emit_event({"debug.partial_failure", {}, command_envelope.payload});
            return heartstead::core::Status::failure("debug.intentional_failure",
                                                     "intentional command failure");
        },
    });
    assert(failing_registered);
    const heartstead::net::CommandEnvelope failing_envelope{
        56, heartstead::core::NetId::from_value(22), "debug.fail_after_mutation", "probe", 12346,
    };
    auto failing_dispatch = dispatcher.dispatch(failing_envelope, server_context);
    assert(!failing_dispatch);
    assert(failing_dispatch.error().code == "debug.intentional_failure");
    auto failing_report = dispatcher.dispatch_report(failing_envelope, server_context);
    assert(!failing_report.succeeded);
    assert(!failing_report.committed_world_mutation);
    assert(failing_report.error.has_value());
    assert(failing_report.error->code == "debug.intentional_failure");
    assert(failing_report.events.size() == 1);
    assert(failing_report.events.front().type == "debug.partial_failure");
    assert(failing_report.operation_trace.stages.back() ==
           heartstead::world::OperationStage::rolled_back);
    assert(failing_report.operation_trace.mutations.size() == 1);
    assert(failing_report.operation_trace.mutations.front() == "partial mutation probe");
    assert(failing_report.operation_trace.derived_updates.size() == 1);
    assert(failing_report.operation_trace.derived_updates.front() == "DebugOnly");
    assert(!failing_report.operation_trace.replication_dirty);
    assert(!failing_report.operation_trace.save_dirty);

    heartstead::net::ServerCommandDispatcher query_dispatcher;
    auto query_registered = query_dispatcher.register_command(heartstead::net::CommandDescriptor{
        "debug.ping",
        false,
        false,
        [](const heartstead::net::CommandEnvelope&, const heartstead::net::CommandExecutionContext&,
           heartstead::world::WorldOperation&) { return heartstead::core::Status::ok(); },
    });
    assert(query_registered);

    auto query_result = query_dispatcher.dispatch(
        {1, heartstead::core::NetId::from_value(1), "debug.ping", "", 0}, client_context);
    assert(query_result);
    assert(!query_result.value().committed_world_mutation);
    assert(query_result.value().operation_trace.stages.back() ==
           heartstead::world::OperationStage::validated);
    assert(query_result.value().operation_trace.mutations.empty());
    assert(!query_result.value().operation_trace.replication_dirty);
    assert(!query_result.value().operation_trace.save_dirty);
    auto query_report = query_dispatcher.dispatch_report(
        {1, heartstead::core::NetId::from_value(1), "debug.ping", "", 0}, client_context);
    assert(query_report.succeeded);
    assert(!query_report.error.has_value());
    assert(!query_report.committed_world_mutation);
    assert(query_report.operation_trace.stages.back() ==
           heartstead::world::OperationStage::validated);
}

void test_command_payload_codec() {
    using namespace heartstead;

    net::CommandPayload payload;
    assert(payload.set("chunk", "0|0|0"));
    assert(payload.set("message", "hello;=world%|\n"));
    assert(payload.size() == 2);
    assert(!payload.set("Bad", "x"));
    assert(!payload.set(".bad", "x"));
    assert(!payload.set("chunk", "duplicate"));

    const auto encoded = net::CommandPayloadTextCodec::encode(payload);
    assert(encoded == "chunk=0%7C0%7C0;message=hello%3B%3Dworld%25%7C%0A");

    auto decoded = net::CommandPayloadTextCodec::decode(encoded);
    assert(decoded);
    auto chunk = decoded.value().require("chunk");
    auto message = decoded.value().require("message");
    assert(chunk);
    assert(message);
    assert(chunk.value() == "0|0|0");
    assert(message.value() == "hello;=world%|\n");
    assert(decoded.value().find("missing") == nullptr);
    assert(!decoded.value().require("missing"));
    const auto binary = net::CommandPayloadBinaryCodec::encode(payload);
    assert(net::CommandPayloadBinaryCodec::is_encoded(binary));
    assert(binary.size() < encoded.size());
    auto decoded_binary = net::CommandPayloadBinaryCodec::decode(binary);
    assert(decoded_binary);
    assert(decoded_binary.value().require("chunk").value() == "0|0|0");
    assert(decoded_binary.value().require("message").value() == "hello;=world%|\n");
    assert(!net::CommandPayloadBinaryCodec::decode(binary.substr(0, binary.size() - 1)));
    const net::CommandEnvelope command{4, core::NetId::from_value(2), "world.set_voxel", encoded,
                                       17};
    const auto wire_command = net::make_command_transport_message(command);
    assert(net::CommandPayloadBinaryCodec::is_encoded(wire_command.payload));
    auto decoded_wire_command = net::command_envelope_from_transport(
        {command.sender, core::NetId::from_value(1), wire_command});
    assert(decoded_wire_command);
    assert(decoded_wire_command.value().payload == encoded);

    net::CommandPayload empty;
    assert(net::CommandPayloadTextCodec::encode(empty).empty());
    assert(!net::CommandPayloadTextCodec::decode(""));
    assert(!net::CommandPayloadTextCodec::decode("missing_separator"));
    assert(!net::CommandPayloadTextCodec::decode("a=1;a=2"));
    assert(!net::CommandPayloadTextCodec::decode("a=%GG"));
    assert(!net::CommandPayloadTextCodec::decode("Bad=x"));
    assert(!net::CommandPayloadTextCodec::decode("a=raw=value"));

    net::CommandPayload aggregate_limited;
    assert(aggregate_limited.set("a", std::string(32U * 1024U, 'x')));
    assert(!aggregate_limited.set("b", std::string(32U * 1024U, 'y')));
}

void test_world_command_registry() {
    const auto wall_id = heartstead::core::PrototypeId::parse("base:build_pieces/wall_frame");
    const auto firebox_id = heartstead::core::PrototypeId::parse("base:build_pieces/firebox");
    const auto chimney_id = heartstead::core::PrototypeId::parse("base:build_pieces/chimney");
    const auto kiln_id = heartstead::core::PrototypeId::parse("base:assemblies/clay_kiln");
    const auto heavy_log_id = heartstead::core::PrototypeId::parse("base:cargo/heavy_log");
    const auto hand_cart_id = heartstead::core::PrototypeId::parse("base:entities/hand_cart");
    const auto process_id = heartstead::core::PrototypeId::parse("base:processes/drying");
    assert(wall_id);
    assert(firebox_id);
    assert(chimney_id);
    assert(kiln_id);
    assert(heavy_log_id);
    assert(hand_cart_id);
    assert(process_id);

    heartstead::modding::GenericPrototype wall_prototype;
    wall_prototype.kind = std::string(heartstead::modding::PrototypeKinds::build_piece);
    wall_prototype.id = wall_id.value();
    wall_prototype.display_name = "Wall Frame";
    wall_prototype.fields.emplace("material_tags", "wood,frame");
    wall_prototype.fields.emplace("room_contribution_tags", "wall,enclosure");
    wall_prototype.fields.emplace("network_ports", "storage_access,smoke_output");

    heartstead::modding::GenericPrototype firebox_prototype;
    firebox_prototype.kind = std::string(heartstead::modding::PrototypeKinds::build_piece);
    firebox_prototype.id = firebox_id.value();
    firebox_prototype.display_name = "Firebox";
    firebox_prototype.fields.emplace("material_tags", "clay,fireproof");
    firebox_prototype.fields.emplace("network_ports", "item_input");

    heartstead::modding::GenericPrototype chimney_prototype;
    chimney_prototype.kind = std::string(heartstead::modding::PrototypeKinds::build_piece);
    chimney_prototype.id = chimney_id.value();
    chimney_prototype.display_name = "Chimney";
    chimney_prototype.fields.emplace("material_tags", "clay,vent");
    chimney_prototype.fields.emplace("network_ports", "smoke_output");

    heartstead::modding::GenericPrototype kiln_prototype;
    kiln_prototype.kind = std::string(heartstead::modding::PrototypeKinds::assembly);
    kiln_prototype.id = kiln_id.value();
    kiln_prototype.display_name = "Clay Kiln";
    kiln_prototype.fields.emplace(
        "required_parts", "firebox:base:build_pieces/firebox,chimney:base:build_pieces/chimney");
    kiln_prototype.fields.emplace("required_ports", "item_input,smoke_output");

    heartstead::modding::GenericPrototype heavy_log_prototype;
    heavy_log_prototype.kind = std::string(heartstead::modding::PrototypeKinds::cargo);
    heavy_log_prototype.id = heavy_log_id.value();
    heavy_log_prototype.display_name = "Heavy Log";
    heavy_log_prototype.fields.emplace("mass_grams", "120000");
    heavy_log_prototype.fields.emplace("volume_milliliters", "200000");
    heavy_log_prototype.fields.emplace("transport_modes", "hand,cart");
    heavy_log_prototype.fields.emplace("hazard_tags", "unstable");

    heartstead::modding::GenericPrototype hand_cart_prototype;
    hand_cart_prototype.kind = std::string(heartstead::modding::PrototypeKinds::entity);
    hand_cart_prototype.id = hand_cart_id.value();
    hand_cart_prototype.display_name = "Hand Cart";
    hand_cart_prototype.fields.emplace("entity_kind", "cart");
    hand_cart_prototype.fields.emplace("persistent", "true");

    heartstead::modding::GenericPrototype process_prototype;
    process_prototype.kind = std::string(heartstead::modding::PrototypeKinds::process);
    process_prototype.id = process_id.value();
    process_prototype.display_name = "Drying";
    process_prototype.fields.emplace("default_required_work_ms", "1200");
    process_prototype.fields.emplace("requires_room", "true");
    process_prototype.fields.emplace("requires_power", "true");
    process_prototype.fields.emplace("required_power_capacity", "2");
    process_prototype.fields.emplace("tags", "ambient,timestamped");

    heartstead::modding::PrototypeRegistry registry;
    auto registry_result =
        registry.build({wall_prototype, firebox_prototype, chimney_prototype, kiln_prototype,
                        heavy_log_prototype, hand_cart_prototype, process_prototype});
    assert(!registry_result.has_errors());

    heartstead::world::WorldStateDesc desc;
    desc.next_save_id = 3000;
    desc.next_runtime_handle = 7000;
    desc.next_entity_net_id = 8000;
    desc.next_process_id = 9000;
    heartstead::world::WorldState state(desc);

    heartstead::net::ServerCommandDispatcher dispatcher;
    assert(heartstead::world::WorldCommandRegistry::register_engine_commands(dispatcher));
    assert(dispatcher.contains("world.set_voxel"));
    assert(dispatcher.contains("build.place_piece"));
    assert(dispatcher.contains("build.complete_piece"));
    assert(dispatcher.contains("workpiece.edit_cell"));
    assert(dispatcher.contains("inventory.transfer_items"));
    assert(dispatcher.contains("process.start"));
    assert(dispatcher.contains("process.advance_all"));
    assert(dispatcher.contains("cargo.create"));
    assert(dispatcher.contains("entity.spawn"));
    assert(dispatcher.contains("assembly.create"));

    heartstead::net::CommandExecutionContext context;
    context.executor_role = heartstead::net::CommandExecutorRole::authoritative_server;
    context.server_time_ms = 500;
    context.prototypes = &registry;
    context.world_state = &state;
    heartstead::world::VoxelPalette command_palette;
    heartstead::world::VoxelDefinition command_voxel;
    command_voxel.type = 12;
    command_voxel.prototype_id =
        heartstead::core::PrototypeId::parse("base:voxels/command_test").value();
    command_voxel.display_name = "Command Test";
    command_voxel.terrain_material = "test";
    command_voxel.mining_tool = "none";
    command_voxel.light_emission = 7;
    assert(command_palette.add(std::move(command_voxel)));
    context.voxel_palette = &command_palette;
    state.chunks().get_or_create({0, 0, 0}).clear_all_dirty();

    heartstead::net::CommandEnvelope voxel_command;
    voxel_command.sequence = 1;
    voxel_command.sender = heartstead::core::NetId::from_value(10);
    voxel_command.type = "world.set_voxel";
    heartstead::net::CommandPayload voxel_payload;
    assert(voxel_payload.set("chunk", "0|0|0"));
    assert(voxel_payload.set("voxel", "2|3|4"));
    assert(voxel_payload.set("prototype", "base:voxels/command_test"));
    voxel_command.payload = heartstead::net::CommandPayloadTextCodec::encode(voxel_payload);
    auto voxel_result = dispatcher.dispatch(voxel_command, context);
    assert(voxel_result);
    assert(voxel_result.value().committed_world_mutation);
    assert(voxel_result.value().events.size() == 1);
    assert(voxel_result.value().events.front().type == heartstead::world::voxel_changed_event_type);
    assert(voxel_result.value().reserved_ids.empty());

    auto voxel = state.chunks().get({0, 0, 0}, {2, 3, 4});
    assert(voxel);
    assert(voxel.value().type == 12);
    assert(voxel.value().light == 7);
    assert(!state.dirty_regions().empty());
    state.dirty_regions().clear();

    heartstead::net::CommandEnvelope build_command;
    build_command.sequence = 2;
    build_command.sender = heartstead::core::NetId::from_value(10);
    build_command.type = "build.place_piece";
    heartstead::net::CommandPayload build_payload;
    assert(build_payload.set("prototype", "base:build_pieces/wall_frame"));
    assert(build_payload.set("position", "1.5|2|3"));
    assert(build_payload.set("rotation", "0|90|0"));
    assert(build_payload.set("scale", "1|1|1"));
    build_command.payload = heartstead::net::CommandPayloadTextCodec::encode(build_payload);
    auto build_result = dispatcher.dispatch(build_command, context);
    assert(build_result);
    assert(build_result.value().committed_world_mutation);
    assert(build_result.value().reserved_ids.size() == 1);
    assert(build_result.value().reserved_ids.front() == heartstead::core::SaveId::from_value(3000));
    assert(build_result.value().events.front().type == "build_piece.placed");

    auto* placed = state.build_objects().find(heartstead::core::SaveId::from_value(3000));
    assert(placed != nullptr);
    assert(placed->prototype_id == wall_id.value());
    assert(placed->transform.position.approximate_global().x == 1.5);
    assert(placed->transform.rotation_degrees.y == 90.0);
    assert(placed->material_tags.size() == 2);
    assert(placed->room_contribution_tags.size() == 2);
    assert(placed->network_ports.size() == 2);
    assert(placed->network_ports[0].kind == heartstead::networks::NetworkKind::storage_access);
    assert(placed->network_ports[1].kind == heartstead::networks::NetworkKind::smoke_ventilation);
    assert(state.dirty_regions().count(heartstead::dirty::DirtyRegionKind::room_graph) == 1);
    assert(state.dirty_regions().count(
               heartstead::dirty::DirtyRegionKind::storage_access_network) == 1);
    assert(state.dirty_regions().count(
               heartstead::dirty::DirtyRegionKind::smoke_ventilation_network) == 1);
    assert(state.networks().find(heartstead::networks::NetworkKind::storage_access) != nullptr);
    assert(state.networks().find(heartstead::networks::NetworkKind::smoke_ventilation) != nullptr);
    assert(state.networks().find(heartstead::networks::NetworkKind::storage_access)->is_dirty());
    assert(state.networks().find(heartstead::networks::NetworkKind::smoke_ventilation)->is_dirty());
    assert(placed->construction_state == heartstead::build::ConstructionState::planned);
    state.dirty_regions().clear();

    heartstead::net::CommandEnvelope complete_build_command;
    complete_build_command.sequence = 3;
    complete_build_command.sender = heartstead::core::NetId::from_value(10);
    complete_build_command.type = "build.complete_piece";
    heartstead::net::CommandPayload complete_build_payload;
    assert(complete_build_payload.set("object", "3000"));
    complete_build_command.payload =
        heartstead::net::CommandPayloadTextCodec::encode(complete_build_payload);
    auto complete_build_result = dispatcher.dispatch(complete_build_command, context);
    assert(complete_build_result);
    assert(complete_build_result.value().committed_world_mutation);
    assert(complete_build_result.value().reserved_ids.empty());
    assert(complete_build_result.value().events.size() == 1);
    assert(complete_build_result.value().events.front().type == "build_piece.completed");
    assert(complete_build_result.value().events.front().subject ==
           heartstead::core::SaveId::from_value(3000));
    placed = state.build_objects().find(heartstead::core::SaveId::from_value(3000));
    assert(placed != nullptr);
    assert(placed->construction_state == heartstead::build::ConstructionState::complete);
    assert(state.dirty_regions().count(heartstead::dirty::DirtyRegionKind::room_graph) == 1);
    assert(state.dirty_regions().count(
               heartstead::dirty::DirtyRegionKind::storage_access_network) == 1);
    assert(state.dirty_regions().count(
               heartstead::dirty::DirtyRegionKind::smoke_ventilation_network) == 1);
    const auto* completed_storage_network =
        state.networks().find(heartstead::networks::NetworkKind::storage_access);
    const auto* completed_smoke_network =
        state.networks().find(heartstead::networks::NetworkKind::smoke_ventilation);
    assert(completed_storage_network != nullptr);
    assert(completed_smoke_network != nullptr);
    assert(!completed_storage_network->is_dirty());
    assert(!completed_smoke_network->is_dirty());
    assert(completed_storage_network->node_count() == 1);
    assert(completed_storage_network->port_count() == 1);
    assert(completed_smoke_network->node_count() == 1);
    assert(completed_smoke_network->port_count() == 1);

    auto duplicate_complete_result = dispatcher.dispatch(complete_build_command, context);
    assert(!duplicate_complete_result);
    assert(duplicate_complete_result.error().code == "world_command.build_piece_already_complete");
    state.dirty_regions().clear();

    auto firebox_save_id = state.save_ids().reserve();
    auto chimney_save_id = state.save_ids().reserve();
    assert(firebox_save_id);
    assert(chimney_save_id);
    const auto* firebox_registry_prototype = registry.find(firebox_id.value());
    const auto* chimney_registry_prototype = registry.find(chimney_id.value());
    assert(firebox_registry_prototype != nullptr);
    assert(chimney_registry_prototype != nullptr);
    auto firebox_record = heartstead::build::build_piece_record_from_prototype(
        *firebox_registry_prototype, firebox_save_id.value(), {});
    auto chimney_record = heartstead::build::build_piece_record_from_prototype(
        *chimney_registry_prototype, chimney_save_id.value(), {});
    assert(firebox_record);
    assert(chimney_record);
    firebox_record.value().construction_state = heartstead::build::ConstructionState::complete;
    chimney_record.value().construction_state = heartstead::build::ConstructionState::complete;
    assert(state.build_objects().insert(firebox_record.value()));
    assert(state.build_objects().insert(chimney_record.value()));

    const auto workpiece_prototype =
        heartstead::core::PrototypeId::parse("base:workpieces/clay_lump");
    assert(workpiece_prototype);
    auto workpiece_grid = heartstead::workpieces::WorkpieceGrid::create({4, 4, 4});
    assert(workpiece_grid);
    heartstead::world::WorkpieceRecord editable_workpiece{
        heartstead::core::WorkpieceId::from_value(7), workpiece_prototype.value(),
        std::move(workpiece_grid).value()};
    editable_workpiece.owner_session = heartstead::core::NetId::from_value(10);
    assert(state.workpieces().insert(std::move(editable_workpiece)));

    heartstead::net::CommandEnvelope workpiece_command;
    workpiece_command.sequence = 4;
    workpiece_command.sender = heartstead::core::NetId::from_value(10);
    workpiece_command.type = "workpiece.edit_cell";
    heartstead::net::CommandPayload workpiece_payload;
    assert(workpiece_payload.set("workpiece_id", "7"));
    assert(workpiece_payload.set("operation", "add_cell"));
    assert(workpiece_payload.set("coord", "1|2|3"));
    assert(workpiece_payload.set("cell", "9|255"));
    workpiece_command.payload = heartstead::net::CommandPayloadTextCodec::encode(workpiece_payload);
    auto workpiece_result = dispatcher.dispatch(workpiece_command, context);
    assert(workpiece_result);
    assert(workpiece_result.value().committed_world_mutation);
    assert(workpiece_result.value().reserved_ids.empty());
    assert(workpiece_result.value().events.size() == 1);
    assert(workpiece_result.value().events.front().type == "workpiece.edited");
    assert(workpiece_result.value().events.front().subject ==
           heartstead::core::SaveId::from_value(7));

    const auto* edited_workpiece =
        state.workpieces().find(heartstead::core::WorkpieceId::from_value(7));
    assert(edited_workpiece != nullptr);
    assert(edited_workpiece->grid.occupied_count() == 1);
    assert(edited_workpiece->grid.history().size() == 1);
    auto edited_cell = edited_workpiece->grid.get({1, 2, 3});
    assert(edited_cell);
    assert(edited_cell.value().material == 9);
    assert(edited_cell.value().occupancy == 255);

    auto duplicate_workpiece_result = dispatcher.dispatch(workpiece_command, context);
    assert(!duplicate_workpiece_result);
    assert(duplicate_workpiece_result.error().code == "workpiece.cell_occupied");

    auto* unbound_workpiece = state.workpieces().find(heartstead::core::WorkpieceId::from_value(7));
    assert(unbound_workpiece != nullptr);
    unbound_workpiece->owner_session = {};
    auto unbound_workpiece_command = workpiece_command;
    unbound_workpiece_command.sequence = 499;
    auto unbound_workpiece_result = dispatcher.dispatch(unbound_workpiece_command, context);
    assert(!unbound_workpiece_result);
    assert(unbound_workpiece_result.error().code == "world_command.workpiece_owner_unbound");

    const auto item_prototype = heartstead::core::PrototypeId::parse("base:items/raw_clay");
    assert(item_prototype);
    auto inventory_source_stack =
        heartstead::items::ItemStack::create(item_prototype.value(), 12, 64);
    auto inventory_destination_stack =
        heartstead::items::ItemStack::create(item_prototype.value(), 8, 64);
    assert(inventory_source_stack);
    assert(inventory_destination_stack);
    assert(state.inventories().insert(
        {heartstead::core::SaveId::from_value(4000), {inventory_source_stack.value()}}));
    assert(state.inventories().insert(
        {heartstead::core::SaveId::from_value(4001), {inventory_destination_stack.value()}}));

    heartstead::net::CommandEnvelope inventory_command;
    inventory_command.sequence = 5;
    inventory_command.sender = heartstead::core::NetId::from_value(10);
    inventory_command.type = "inventory.transfer_items";
    heartstead::net::CommandPayload inventory_payload;
    assert(inventory_payload.set("source_owner", "4000"));
    assert(inventory_payload.set("destination_owner", "4001"));
    assert(inventory_payload.set("source_slot", "0"));
    assert(inventory_payload.set("destination_slot", "0"));
    assert(inventory_payload.set("count", "5"));
    inventory_command.payload = heartstead::net::CommandPayloadTextCodec::encode(inventory_payload);
    auto inventory_result = dispatcher.dispatch(inventory_command, context);
    assert(inventory_result);
    assert(inventory_result.value().committed_world_mutation);
    assert(inventory_result.value().reserved_ids.empty());
    assert(inventory_result.value().events.size() == 2);
    assert(inventory_result.value().events.front().type == "inventory.items_transferred");
    assert(inventory_result.value().events.front().subject ==
           heartstead::core::SaveId::from_value(4000));
    assert(inventory_result.value().events.back().type == "inventory.items_transferred");
    assert(inventory_result.value().events.back().subject ==
           heartstead::core::SaveId::from_value(4001));

    heartstead::net::ReplicationBatch inventory_replication_batch;
    inventory_replication_batch.command_sequence = inventory_result.value().sequence;
    inventory_replication_batch.command_type = inventory_result.value().command_type;
    inventory_replication_batch.events = inventory_result.value().events;
    auto inventory_delta =
        heartstead::world::materialize_replication_delta(state, inventory_replication_batch);
    assert(inventory_delta.inventories.size() == 2);
    assert(inventory_delta.plan.unique_subject_count == 2);

    const auto* source_inventory =
        state.inventories().find(heartstead::core::SaveId::from_value(4000));
    const auto* destination_inventory =
        state.inventories().find(heartstead::core::SaveId::from_value(4001));
    assert(source_inventory != nullptr);
    assert(destination_inventory != nullptr);
    assert(source_inventory->stacks.front().count == 7);
    assert(destination_inventory->stacks.front().count == 13);

    auto inventory_self_command = inventory_command;
    inventory_self_command.sequence = 500;
    heartstead::net::CommandPayload inventory_self_payload;
    assert(inventory_self_payload.set("source_owner", "4000"));
    assert(inventory_self_payload.set("destination_owner", "4000"));
    assert(inventory_self_payload.set("source_slot", "0"));
    assert(inventory_self_payload.set("destination_slot", "0"));
    assert(inventory_self_payload.set("count", "1"));
    inventory_self_command.payload =
        heartstead::net::CommandPayloadTextCodec::encode(inventory_self_payload);
    auto inventory_self_result = dispatcher.dispatch(inventory_self_command, context);
    assert(inventory_self_result);
    assert(inventory_self_result.value().events.size() == 1);
    assert(inventory_self_result.value().events.front().subject ==
           heartstead::core::SaveId::from_value(4000));
    source_inventory = state.inventories().find(heartstead::core::SaveId::from_value(4000));
    assert(source_inventory != nullptr);
    assert(source_inventory->stacks.front().count == 7);

    heartstead::net::CommandEnvelope process_command;
    process_command.sequence = 6;
    process_command.sender = heartstead::core::NetId::from_value(10);
    process_command.type = "process.start";
    heartstead::net::CommandPayload process_payload;
    assert(process_payload.set("owner", "3000"));
    assert(process_payload.set("prototype", "base:processes/drying"));
    process_command.payload = heartstead::net::CommandPayloadTextCodec::encode(process_payload);
    assert(state.advance_world_time(500));
    auto process_result = dispatcher.dispatch(process_command, context);
    assert(process_result);
    assert(process_result.value().committed_world_mutation);
    assert(process_result.value().reserved_ids.empty());
    assert(process_result.value().events.size() == 1);
    assert(process_result.value().events.front().type == "process.started");
    assert(process_result.value().events.front().subject ==
           heartstead::core::SaveId::from_value(3000));

    auto owner_processes =
        state.processes().find_by_owner(heartstead::core::SaveId::from_value(3000));
    assert(owner_processes.size() == 1);
    assert(owner_processes.front()->process_id == heartstead::core::ProcessId::from_value(9000));
    assert(owner_processes.front()->prototype_id == process_id.value());
    assert(owner_processes.front()->start_time_ms == 500);
    assert(owner_processes.front()->last_update_time_ms == 500);
    assert(owner_processes.front()->required_effective_work_ms == 1200);
    assert(state.process_ids().peek_next() == heartstead::core::ProcessId::from_value(9001));

    heartstead::net::CommandEnvelope missing_process_owner_command;
    missing_process_owner_command.sequence = 7;
    missing_process_owner_command.sender = heartstead::core::NetId::from_value(10);
    missing_process_owner_command.type = "process.start";
    heartstead::net::CommandPayload missing_process_owner_payload;
    assert(missing_process_owner_payload.set("owner", "999999"));
    assert(missing_process_owner_payload.set("prototype", "base:processes/drying"));
    missing_process_owner_command.payload =
        heartstead::net::CommandPayloadTextCodec::encode(missing_process_owner_payload);
    auto missing_process_owner_result = dispatcher.dispatch(missing_process_owner_command, context);
    assert(!missing_process_owner_result);
    assert(missing_process_owner_result.error().code == "world_command.missing_process_owner");
    assert(state.process_ids().peek_next() == heartstead::core::ProcessId::from_value(9001));

    const auto process_owner_id = heartstead::core::SaveId::from_value(3000);
    heartstead::rooms::RoomRecord process_room;
    process_room.id = heartstead::rooms::RoomId::from_value(44);
    process_room.label = "Drying Workshop";
    process_room.volume_cells = 20;
    process_room.source_build_piece_ids.push_back(process_owner_id);
    process_room.metrics.enclosure_per_mille = 950;
    process_room.metrics.roof_coverage_per_mille = 900;
    process_room.metrics.wall_coverage_per_mille = 900;
    process_room.metrics.warmth = 300;
    process_room.metrics.dryness = 300;
    process_room.metrics.ventilation_per_mille = 800;
    process_room.metrics.safety_per_mille = 900;
    process_room.metrics.spaciousness_per_mille = 800;
    process_room.metrics.storage_access = true;
    process_room.metrics.cart_access = true;
    process_room.metrics.power_access = true;
    process_room.descriptors = heartstead::rooms::RoomEvaluator::evaluate(process_room.metrics);
    assert(state.rooms().add_or_replace(process_room));
    state.rooms().clear_dirty();

    auto& process_power = state.networks().get_or_create(heartstead::networks::NetworkKind::power);
    const auto process_power_node = heartstead::networks::NetworkNodeId::from_value(70000);
    assert(process_power.add_node(
        heartstead::networks::NetworkNode{process_power_node, {1, 2, 3}, 4, "drying power"}));
    assert(process_power.add_port(heartstead::networks::NetworkPort{
        heartstead::networks::NetworkPortId::from_value(70001),
        process_power_node,
        "power_input",
        4,
        process_owner_id,
        process_owner_id,
    }));
    process_power.clear_dirty();

    context.server_time_ms = 1200;
    assert(state.advance_world_time(700));
    heartstead::net::CommandEnvelope process_advance_command;
    process_advance_command.sequence = 8;
    process_advance_command.sender = heartstead::core::NetId::from_value(10);
    process_advance_command.type = "process.advance_all";
    auto process_advance_result = dispatcher.dispatch(process_advance_command, context);
    assert(process_advance_result);
    assert(process_advance_result.value().committed_world_mutation);
    assert(process_advance_result.value().reserved_ids.empty());
    assert(process_advance_result.value().events.size() == 1);
    assert(process_advance_result.value().events.front().type == "processes.advanced");
    assert(process_advance_result.value().events.front().message == "1");
    owner_processes = state.processes().find_by_owner(heartstead::core::SaveId::from_value(3000));
    assert(owner_processes.size() == 1);
    assert(owner_processes.front()->last_update_time_ms == 1200);
    assert(owner_processes.front()->accumulated_effective_work_ms == 1200);
    assert(owner_processes.front()->state == heartstead::processes::ProcessState::complete);

    auto duplicate_process_advance_result = dispatcher.dispatch(process_advance_command, context);
    assert(!duplicate_process_advance_result);
    assert(duplicate_process_advance_result.error().code == "world_command.no_processes_advanced");

    heartstead::net::CommandEnvelope untrusted_process_advance_command = process_advance_command;
    untrusted_process_advance_command.sequence = 9;
    heartstead::net::CommandPayload untrusted_process_advance_payload;
    assert(untrusted_process_advance_payload.set("room_rate", "10000"));
    untrusted_process_advance_command.payload =
        heartstead::net::CommandPayloadTextCodec::encode(untrusted_process_advance_payload);
    auto untrusted_process_advance_result =
        dispatcher.dispatch(untrusted_process_advance_command, context);
    assert(!untrusted_process_advance_result);
    assert(untrusted_process_advance_result.error().code ==
           "world_command.untrusted_process_modifiers");

    heartstead::net::CommandEnvelope assembly_command;
    assembly_command.sequence = 10;
    assembly_command.sender = heartstead::core::NetId::from_value(10);
    assembly_command.type = "assembly.create";
    heartstead::net::CommandPayload assembly_payload;
    assert(assembly_payload.set("prototype", "base:assemblies/clay_kiln"));
    assert(assembly_payload.set("root", firebox_save_id.value().to_string()));
    assert(assembly_payload.set("parts", "firebox:" + firebox_save_id.value().to_string() +
                                             ",chimney:" + chimney_save_id.value().to_string()));
    assembly_command.payload = heartstead::net::CommandPayloadTextCodec::encode(assembly_payload);
    auto assembly_result = dispatcher.dispatch(assembly_command, context);
    assert(assembly_result);
    assert(assembly_result.value().committed_world_mutation);
    assert(assembly_result.value().reserved_ids.size() == 1);
    assert(assembly_result.value().reserved_ids.front() ==
           heartstead::core::SaveId::from_value(3003));
    assert(assembly_result.value().events.size() == 1);
    assert(assembly_result.value().events.front().type == "assembly.created");
    assert(assembly_result.value().events.front().subject ==
           heartstead::core::SaveId::from_value(3003));

    const auto* assembly = state.assemblies().find(heartstead::core::SaveId::from_value(3003));
    assert(assembly != nullptr);
    assert(assembly->root_build_piece_id == firebox_save_id.value());
    assert(assembly->prototype_id == kiln_id.value());
    assert(assembly->parts.size() == 2);
    assert(assembly->ports.size() == 2);
    assert(assembly->ports[0].name == "item_input");
    assert(assembly->ports[0].source_build_piece_id == firebox_save_id.value());
    assert(assembly->ports[0].capacity == 1);
    assert(assembly->ports[1].name == "smoke_output");
    assert(assembly->ports[1].source_build_piece_id == chimney_save_id.value());
    assert(assembly->ports[1].capacity == 1);
    const auto* assembled_storage_network =
        state.networks().find(heartstead::networks::NetworkKind::storage_access);
    const auto* assembled_smoke_network =
        state.networks().find(heartstead::networks::NetworkKind::smoke_ventilation);
    assert(assembled_storage_network != nullptr);
    assert(assembled_smoke_network != nullptr);
    assert(!assembled_storage_network->is_dirty());
    assert(!assembled_smoke_network->is_dirty());
    assert(assembled_storage_network->node_count() == 3);
    assert(assembled_storage_network->edge_count() == 1);
    assert(assembled_storage_network->port_count() == 3);
    assert(assembled_smoke_network->node_count() == 3);
    assert(assembled_smoke_network->edge_count() == 1);
    assert(assembled_smoke_network->port_count() == 3);

    heartstead::net::CommandEnvelope cargo_command;
    cargo_command.sequence = 11;
    cargo_command.sender = heartstead::core::NetId::from_value(10);
    cargo_command.type = "cargo.create";
    heartstead::net::CommandPayload cargo_payload;
    assert(cargo_payload.set("prototype", "base:cargo/heavy_log"));
    assert(cargo_payload.set("position", "5.5|0|-2.25"));
    cargo_command.payload = heartstead::net::CommandPayloadTextCodec::encode(cargo_payload);
    auto cargo_result = dispatcher.dispatch(cargo_command, context);
    assert(cargo_result);
    assert(cargo_result.value().committed_world_mutation);
    assert(cargo_result.value().reserved_ids.size() == 1);
    assert(cargo_result.value().reserved_ids.front() == heartstead::core::SaveId::from_value(3004));
    assert(cargo_result.value().events.size() == 1);
    assert(cargo_result.value().events.front().type == "cargo.created");
    assert(cargo_result.value().events.front().subject ==
           heartstead::core::SaveId::from_value(3004));

    const auto* cargo_record = state.cargo().find(heartstead::core::SaveId::from_value(3004));
    assert(cargo_record != nullptr);
    assert(cargo_record->prototype_id == heavy_log_id.value());
    assert(cargo_record->position.approximate_global().x == 5.5);
    assert(cargo_record->position.approximate_global().z == -2.25);
    assert(cargo_record->mass_grams == 120000);
    assert(cargo_record->volume_milliliters == 200000);
    assert(
        cargo_record->allowed_transport_modes.allows(heartstead::cargo::CargoTransportMode::cart));
    assert(cargo_record->is_hazardous());
    assert(state.stats().inventory_count == 2);

    heartstead::net::CommandEnvelope entity_command;
    entity_command.sequence = 12;
    entity_command.sender = heartstead::core::NetId::from_value(10);
    entity_command.type = "entity.spawn";
    heartstead::net::CommandPayload entity_payload;
    assert(entity_payload.set("prototype", "base:entities/hand_cart"));
    assert(entity_payload.set("position", "4.5|1|2"));
    assert(entity_payload.set("rotation", "0|180|0"));
    assert(entity_payload.set("scale", "1|1|1"));
    entity_command.payload = heartstead::net::CommandPayloadTextCodec::encode(entity_payload);
    auto entity_result = dispatcher.dispatch(entity_command, context);
    assert(entity_result);
    assert(entity_result.value().committed_world_mutation);
    assert(entity_result.value().reserved_ids.size() == 1);
    assert(entity_result.value().reserved_ids.front() ==
           heartstead::core::SaveId::from_value(3005));
    assert(entity_result.value().events.size() == 1);
    assert(entity_result.value().events.front().type == "entity.spawned");
    assert(entity_result.value().events.front().subject ==
           heartstead::core::SaveId::from_value(3005));

    const auto* spawned_entity =
        state.entities().find(heartstead::core::RuntimeHandle::from_value(7000));
    assert(spawned_entity != nullptr);
    assert(spawned_entity->net_id == heartstead::core::NetId::from_value(8000));
    assert(spawned_entity->save_id == heartstead::core::SaveId::from_value(3005));
    assert(spawned_entity->prototype_id == hand_cart_id.value());
    assert(spawned_entity->kind == heartstead::entities::EntityKind::cart);
    assert(spawned_entity->persistent);
    assert(spawned_entity->transform.position.approximate_global().x == 4.5);
    assert(spawned_entity->transform.position.approximate_global().y == 1.0);
    assert(spawned_entity->transform.rotation_degrees.y == 180.0);
    assert(state.entities().find_by_net_id(heartstead::core::NetId::from_value(8000)) != nullptr);
    assert(state.entities().find_by_save_id(heartstead::core::SaveId::from_value(3005)) != nullptr);
    assert(state.runtime_handles().peek_next() ==
           heartstead::core::RuntimeHandle::from_value(7001));
    assert(state.entity_net_ids().peek_next() == heartstead::core::NetId::from_value(8001));

    heartstead::net::CommandEnvelope invalid_entity_transform_command = entity_command;
    invalid_entity_transform_command.sequence = 13;
    heartstead::net::CommandPayload invalid_entity_transform_payload;
    assert(invalid_entity_transform_payload.set("prototype", "base:entities/hand_cart"));
    assert(invalid_entity_transform_payload.set("scale", "0|1|1"));
    invalid_entity_transform_command.payload =
        heartstead::net::CommandPayloadTextCodec::encode(invalid_entity_transform_payload);
    auto invalid_entity_transform_result =
        dispatcher.dispatch(invalid_entity_transform_command, context);
    assert(!invalid_entity_transform_result);
    assert(invalid_entity_transform_result.error().code == "world_command.invalid_transform_scale");
    assert(state.save_ids().peek_next() == heartstead::core::SaveId::from_value(3006));
    assert(state.runtime_handles().peek_next() ==
           heartstead::core::RuntimeHandle::from_value(7001));
    assert(state.entity_net_ids().peek_next() == heartstead::core::NetId::from_value(8001));

    heartstead::net::CommandExecutionContext client_context = context;
    client_context.executor_role = heartstead::net::CommandExecutorRole::client_prediction;
    auto client_result = dispatcher.dispatch(voxel_command, client_context);
    assert(!client_result);
    assert(client_result.error().code == "command.not_authoritative");

    heartstead::net::CommandExecutionContext missing_world = context;
    missing_world.world_state = nullptr;
    auto missing_world_result = dispatcher.dispatch(voxel_command, missing_world);
    assert(!missing_world_result);
    assert(missing_world_result.error().code == "world_command.missing_world_state");

    auto bad_payload = voxel_command;
    heartstead::net::CommandPayload incomplete_payload;
    assert(incomplete_payload.set("chunk", "0|0|0"));
    assert(incomplete_payload.set("cell", "1|0"));
    bad_payload.payload = heartstead::net::CommandPayloadTextCodec::encode(incomplete_payload);
    auto bad_result = dispatcher.dispatch(bad_payload, context);
    assert(!bad_result);
}

void test_network_transport() {
    using namespace heartstead;

    const auto in_memory_info = net::transport_backend_info(net::TransportBackend::in_memory);
    assert(in_memory_info.available);
    assert(in_memory_info.name == "in_memory");

    const auto external_info = net::transport_backend_info(net::TransportBackend::external_library);
    assert(external_info.name == "external_library");

    assert(net::transport_endpoint_name(net::TransportEndpoint{"127.0.0.1", 7777}) ==
           "127.0.0.1:7777");
    assert(!net::validate_transport_endpoint(net::TransportEndpoint{"", 7777}));
    assert(net::validate_transport_endpoint(net::TransportEndpoint{"127.0.0.1", 0}));
    assert(!net::validate_transport_endpoint(net::TransportEndpoint{"bad host", 7777}));

    assert(!net::validate_transport_host_config(
        net::InMemoryTransportHostConfig{core::NetId{}, 1024}));
    assert(!net::validate_transport_host_config(
        net::InMemoryTransportHostConfig{core::NetId::from_value(1), 0}));
    assert(!net::validate_transport_host_config(
        net::InMemoryTransportHostConfig{core::NetId::from_value(1), 1024, 0}));
    assert(!net::validate_transport_host_desc(net::TransportHostDesc{
        net::TransportBackend::in_memory,
        net::InMemoryTransportHostConfig{core::NetId::from_value(1), 0},
    }));

    net::ExternalTransportHostConfig external_config{
        core::NetId::from_value(9), net::TransportEndpoint{"127.0.0.1", 0}, 4096, 16, false};
    external_config.reliability = net::TransportReliabilityConfig{10, 3, 8, 8};
    assert(net::validate_external_transport_host_config(external_config));
    auto invalid_external = external_config;
    invalid_external.server_id = {};
    assert(!net::validate_external_transport_host_config(invalid_external));
    invalid_external = external_config;
    invalid_external.bind_endpoint.address = "bad host";
    assert(!net::validate_external_transport_host_config(invalid_external));
    invalid_external = external_config;
    invalid_external.max_payload_bytes = 0;
    assert(!net::validate_external_transport_host_config(invalid_external));
    invalid_external = external_config;
    invalid_external.max_clients = 0;
    assert(!net::validate_external_transport_host_config(invalid_external));
    invalid_external = external_config;
    invalid_external.reliability.retry_delay_ms = 0;
    assert(!net::validate_external_transport_host_config(invalid_external));

    net::TransportHostDesc external_desc;
    external_desc.backend = net::TransportBackend::external_library;
    external_desc.external = external_config;
    auto external_capabilities = net::transport_host_capabilities(external_desc);
    assert(external_capabilities);
    assert(external_capabilities.value().supports_reliable);
    assert(!external_capabilities.value().supports_unreliable);
    assert(external_capabilities.value().enforces_reliable_command_order);
    assert(external_capabilities.value().max_payload_bytes == 4096);
    assert(external_capabilities.value().max_clients == 16);
    assert(net::transport_host_server_id(external_desc) == core::NetId::from_value(9));

    auto external_transport = net::create_transport_host(external_desc);
    if (external_info.available) {
        assert(external_transport);
        assert(external_transport.value()->backend() == net::TransportBackend::external_library);
        assert(external_transport.value()->backend_name() == "external_library");
        assert(external_transport.value()->server_id() == core::NetId::from_value(9));
        auto external_client = external_transport.value()->connect_client();
        assert(external_client);
        assert(external_transport.value()->is_client_connected(external_client.value()));

        net::TransportMessage external_command{
            net::TransportMessageKind::command,
            net::TransportChannel::reliable,
            1,
            "world.set_voxel",
            "small_payload",
            100,
        };
        assert(external_transport.value()->send_client_to_server(external_client.value(),
                                                                 external_command));
        auto external_replay = external_transport.value()->send_client_to_server(
            external_client.value(), external_command);
        assert(!external_replay);
        assert(external_replay.error().code == "transport.reliable_command_replayed");

        auto external_early_maintenance = external_transport.value()->poll_maintenance(5);
        assert(external_early_maintenance);
        assert(external_early_maintenance.value().retransmission_count == 0);
        assert(external_early_maintenance.value().dropped_reliable_message_count == 0);
        auto external_retry_maintenance = external_transport.value()->poll_maintenance(10);
        assert(external_retry_maintenance);
        assert(external_retry_maintenance.value().retransmission_count == 1);
        assert(external_retry_maintenance.value().dropped_reliable_message_count == 0);

        auto external_server_messages = external_transport.value()->drain_server_messages();
        assert(external_server_messages.size() == 1);
        assert(external_server_messages.front().sender == external_client.value());
        assert(external_server_messages.front().recipient == core::NetId::from_value(9));
        assert(external_server_messages.front().message.payload == "small_payload");

        auto external_ack_messages =
            external_transport.value()->drain_client_messages(external_client.value());
        assert(external_ack_messages);
        assert(external_ack_messages.value().empty());
        auto external_after_ack_maintenance = external_transport.value()->poll_maintenance(20);
        assert(external_after_ack_maintenance);
        assert(external_after_ack_maintenance.value().retransmission_count == 0);

        external_command.sequence = 2;
        external_command.payload.assign(3000, 'p');
        external_command.payload.front() = 'H';
        external_command.payload.back() = 'd';
        assert(external_transport.value()->send_client_to_server(external_client.value(),
                                                                 external_command));
        external_server_messages = external_transport.value()->drain_server_messages();
        assert(external_server_messages.size() == 1);
        assert(external_server_messages.front().message.sequence == 2);
        assert(external_server_messages.front().message.payload == external_command.payload);

        net::TransportMessage external_response{
            net::TransportMessageKind::command_result,
            net::TransportChannel::reliable,
            2,
            "host.command_result",
            "ok",
            200,
        };
        assert(external_transport.value()->send_server_to_client(external_client.value(),
                                                                 external_response));
        auto external_disconnected =
            external_transport.value()->disconnect_client(external_client.value());
        assert(external_disconnected);
        assert(!external_transport.value()->is_client_connected(external_client.value()));
        auto external_client_messages =
            external_transport.value()->drain_client_messages(external_client.value());
        assert(external_client_messages);
        assert(external_client_messages.value().size() == 1);
        assert(external_client_messages.value().front().sender == core::NetId::from_value(9));
        assert(external_client_messages.value().front().recipient == external_client.value());
        assert(external_client_messages.value().front().message.payload == "ok");
        assert(!external_transport.value()->send_client_to_server(external_client.value(),
                                                                  external_command));
    } else {
        assert(!external_transport);
        assert(external_transport.error().code == "transport.external_unavailable");
    }

    net::TransportHostDesc in_memory_desc{
        net::TransportBackend::in_memory,
        net::InMemoryTransportHostConfig{core::NetId::from_value(8), 32, 4},
    };
    auto in_memory_capabilities = net::transport_host_capabilities(in_memory_desc);
    assert(in_memory_capabilities);
    assert(in_memory_capabilities.value().supports_reliable);
    assert(in_memory_capabilities.value().supports_unreliable);
    assert(in_memory_capabilities.value().enforces_reliable_command_order);
    assert(in_memory_capabilities.value().max_payload_bytes == 32);
    assert(in_memory_capabilities.value().max_clients == 4);
    assert(net::transport_host_server_id(in_memory_desc) == core::NetId::from_value(8));

    auto transport_interface = net::create_transport_host(in_memory_desc);
    assert(transport_interface);
    assert(transport_interface.value()->backend() == net::TransportBackend::in_memory);
    assert(transport_interface.value()->backend_name() == "in_memory");
    assert(transport_interface.value()->capabilities().max_clients == 4);
    assert(transport_interface.value()->server_id() == core::NetId::from_value(8));
    auto interface_maintenance = transport_interface.value()->poll_maintenance(1000);
    assert(interface_maintenance);
    assert(interface_maintenance.value().retransmission_count == 0);
    assert(interface_maintenance.value().dropped_reliable_message_count == 0);

    net::InMemoryTransportHost limited_transport(
        net::InMemoryTransportHostConfig{core::NetId::from_value(5), 32, 1});
    assert(limited_transport.connect_client());
    auto limited_second_client = limited_transport.connect_client();
    assert(!limited_second_client);
    assert(limited_second_client.error().code == "transport.client_limit_reached");

    net::InMemoryTransportHost transport(
        net::InMemoryTransportHostConfig{core::NetId::from_value(9), 32});
    assert(transport.backend() == net::TransportBackend::in_memory);
    assert(transport.backend_name() == "in_memory");
    assert(transport.server_id() == core::NetId::from_value(9));
    auto in_memory_maintenance = transport.poll_maintenance(1000);
    assert(in_memory_maintenance);
    assert(in_memory_maintenance.value().retransmission_count == 0);
    assert(in_memory_maintenance.value().dropped_reliable_message_count == 0);

    auto client = transport.connect_client();
    assert(client);
    assert(client.value().is_valid());
    assert(transport.connected_client_count() == 1);
    assert(transport.is_client_connected(client.value()));
    assert(transport.connected_client_ids().size() == 1);
    assert(transport.connected_client_ids().front() == client.value());

    auto oversized = transport.send_client_to_server(
        client.value(),
        net::TransportMessage{net::TransportMessageKind::control, net::TransportChannel::reliable,
                              1, "debug.too_large", std::string(33, 'x'), 0});
    assert(!oversized);

    net::CommandEnvelope outgoing;
    outgoing.sequence = 7;
    outgoing.sender = client.value();
    outgoing.type = "debug.ping";
    outgoing.payload = "hello";
    outgoing.client_time_ms = 123;

    auto sent = transport.send_client_to_server(client.value(),
                                                net::make_command_transport_message(outgoing));
    assert(sent);

    auto server_messages = transport.drain_server_messages();
    assert(server_messages.size() == 1);
    assert(server_messages.front().sender == client.value());
    assert(server_messages.front().recipient == transport.server_id());
    assert(server_messages.front().message.kind == net::TransportMessageKind::command);

    auto command = net::command_envelope_from_transport(server_messages.front());
    assert(command);
    assert(command.value().sequence == outgoing.sequence);
    assert(command.value().sender == client.value());
    assert(command.value().type == "debug.ping");
    assert(command.value().payload == "hello");
    assert(command.value().client_time_ms == 123);

    auto replayed = transport.send_client_to_server(client.value(),
                                                    net::make_command_transport_message(outgoing));
    assert(!replayed);
    assert(replayed.error().code == "transport.reliable_command_replayed");
    auto out_of_order = outgoing;
    out_of_order.sequence = 6;
    auto out_of_order_sent = transport.send_client_to_server(
        client.value(), net::make_command_transport_message(out_of_order));
    assert(!out_of_order_sent);
    assert(out_of_order_sent.error().code == "transport.reliable_command_replayed");
    auto next_reliable = outgoing;
    next_reliable.sequence = 8;
    auto next_reliable_sent = transport.send_client_to_server(
        client.value(), net::make_command_transport_message(next_reliable));
    assert(next_reliable_sent);
    auto next_server_messages = transport.drain_server_messages();
    assert(next_server_messages.size() == 1);
    assert(next_server_messages.front().message.sequence == 8);

    net::TransportServerWelcome welcome{
        net::transport_control_protocol_version,
        transport.server_id(),
        client.value(),
        transport.capabilities().max_payload_bytes,
        transport.capabilities().max_clients,
        transport.capabilities().supports_unreliable,
        transport.capabilities().enforces_reliable_command_order,
    };
    auto welcome_status = net::validate_transport_server_welcome(welcome);
    assert(welcome_status);
    const auto welcome_payload = net::TransportControlTextCodec::encode_server_welcome(welcome);
    auto decoded_welcome = net::TransportControlTextCodec::decode_server_welcome(welcome_payload);
    assert(decoded_welcome);
    assert(decoded_welcome.value().server_id == transport.server_id());
    assert(decoded_welcome.value().assigned_client_id == client.value());
    assert(decoded_welcome.value().max_payload_bytes == 32);
    assert(decoded_welcome.value().max_clients == 64);
    assert(decoded_welcome.value().supports_unreliable);
    assert(decoded_welcome.value().enforces_reliable_command_order);

    auto welcome_message = net::make_server_welcome_transport_message(welcome, 321);
    assert(welcome_message.kind == net::TransportMessageKind::control);
    assert(welcome_message.channel == net::TransportChannel::reliable);
    assert(welcome_message.sequence == 0);
    assert(welcome_message.payload_type == net::transport_server_welcome_payload_type);
    assert(welcome_message.timestamp_ms == 321);
    auto decoded_welcome_from_message =
        net::TransportControlTextCodec::decode_server_welcome(welcome_message.payload);
    assert(decoded_welcome_from_message);

    const net::TransportEnvelope welcome_envelope{
        transport.server_id(),
        client.value(),
        welcome_message,
    };
    auto accepted_session = net::accept_transport_server_welcome(client.value(), welcome_envelope);
    assert(accepted_session);
    assert(accepted_session.value().server_id == transport.server_id());
    assert(accepted_session.value().client_id == client.value());
    assert(accepted_session.value().max_payload_bytes == 32);
    assert(accepted_session.value().max_clients == 64);
    assert(accepted_session.value().established_at_ms == 321);
    assert(accepted_session.value().supports_unreliable);
    assert(accepted_session.value().enforces_reliable_command_order);
    assert(net::validate_transport_client_session(accepted_session.value()));

    auto wrong_recipient = welcome_envelope;
    wrong_recipient.recipient = core::NetId::from_value(500);
    auto wrong_recipient_result =
        net::accept_transport_server_welcome(client.value(), wrong_recipient);
    assert(!wrong_recipient_result);
    assert(wrong_recipient_result.error().code == "transport_control.recipient_mismatch");

    auto wrong_sender = welcome_envelope;
    wrong_sender.sender = core::NetId::from_value(501);
    auto wrong_sender_result = net::accept_transport_server_welcome(client.value(), wrong_sender);
    assert(!wrong_sender_result);
    assert(wrong_sender_result.error().code == "transport_control.server_id_mismatch");

    auto wrong_payload_type = welcome_envelope;
    wrong_payload_type.message.payload_type = "control.other";
    auto wrong_payload_type_result =
        net::accept_transport_server_welcome(client.value(), wrong_payload_type);
    assert(!wrong_payload_type_result);
    assert(wrong_payload_type_result.error().code == "transport_control.unexpected_payload_type");

    auto unreliable_welcome = welcome_envelope;
    unreliable_welcome.message.channel = net::TransportChannel::unreliable;
    auto unreliable_welcome_result =
        net::accept_transport_server_welcome(client.value(), unreliable_welcome);
    assert(!unreliable_welcome_result);
    assert(unreliable_welcome_result.error().code == "transport_control.unreliable_server_welcome");

    auto invalid_welcome = welcome_payload;
    const auto protocol_begin = invalid_welcome.find("protocol=1");
    assert(protocol_begin != std::string::npos);
    invalid_welcome.replace(protocol_begin, std::string("protocol=1").size(), "protocol=99");
    auto unsupported_welcome =
        net::TransportControlTextCodec::decode_server_welcome(invalid_welcome);
    assert(!unsupported_welcome);
    assert(unsupported_welcome.error().code == "transport_control.unsupported_protocol_version");

    net::TransportServerDisconnect disconnect{
        net::transport_control_protocol_version,
        transport.server_id(),
        client.value(),
        "server.shutdown",
        "server closed|with\nescaped=message",
    };
    auto disconnect_status = net::validate_transport_server_disconnect(disconnect);
    assert(disconnect_status);
    const auto disconnect_payload =
        net::TransportControlTextCodec::encode_server_disconnect(disconnect);
    auto decoded_disconnect =
        net::TransportControlTextCodec::decode_server_disconnect(disconnect_payload);
    assert(decoded_disconnect);
    assert(decoded_disconnect.value().server_id == transport.server_id());
    assert(decoded_disconnect.value().client_id == client.value());
    assert(decoded_disconnect.value().reason_code == "server.shutdown");
    assert(decoded_disconnect.value().reason_message == "server closed|with\nescaped=message");
    auto disconnect_message = net::make_server_disconnect_transport_message(disconnect, 654);
    assert(disconnect_message.kind == net::TransportMessageKind::control);
    assert(disconnect_message.channel == net::TransportChannel::reliable);
    assert(disconnect_message.payload_type == net::transport_server_disconnect_payload_type);
    assert(disconnect_message.timestamp_ms == 654);
    auto accepted_disconnect = net::accept_transport_server_disconnect(
        transport.server_id(), client.value(),
        net::TransportEnvelope{transport.server_id(), client.value(), disconnect_message});
    assert(accepted_disconnect);
    assert(accepted_disconnect.value().reason_code == "server.shutdown");

    auto bad_disconnect = disconnect;
    bad_disconnect.reason_code = "Bad Reason";
    assert(!net::validate_transport_server_disconnect(bad_disconnect));

    std::string complex_payload = "line1\npayload:\n";
    complex_payload.push_back('\0');
    complex_payload += "tail";
    const net::TransportEnvelope packet_envelope{
        client.value(),
        transport.server_id(),
        net::TransportMessage{net::TransportMessageKind::control, net::TransportChannel::unreliable,
                              9, "debug.payload", complex_payload, -5},
    };
    const auto encoded_packet = net::TransportPacketCodec::encode(packet_envelope);
    auto decoded_packet =
        net::TransportPacketCodec::decode(encoded_packet, net::TransportPacketCodecConfig{64});
    assert(decoded_packet);
    assert(decoded_packet.value().sender == client.value());
    assert(decoded_packet.value().recipient == transport.server_id());
    assert(decoded_packet.value().message.kind == net::TransportMessageKind::control);
    assert(decoded_packet.value().message.channel == net::TransportChannel::unreliable);
    assert(decoded_packet.value().message.sequence == 9);
    assert(decoded_packet.value().message.payload_type == "debug.payload");
    assert(decoded_packet.value().message.payload == complex_payload);
    assert(decoded_packet.value().message.timestamp_ms == -5);

    auto packet_too_large =
        net::TransportPacketCodec::decode(encoded_packet, net::TransportPacketCodecConfig{4});
    assert(!packet_too_large);
    assert(packet_too_large.error().code == "transport.payload_too_large");

    auto bad_magic = encoded_packet;
    bad_magic.replace(0, std::string("heartstead.transport.v1").size(), "bad.transport.v1");
    auto bad_magic_result =
        net::TransportPacketCodec::decode(bad_magic, net::TransportPacketCodecConfig{64});
    assert(!bad_magic_result);
    assert(bad_magic_result.error().code == "transport_packet.invalid_magic");

    auto bad_size = encoded_packet;
    const auto size_begin = bad_size.find("payload_size=");
    assert(size_begin != std::string::npos);
    const auto size_end = bad_size.find('\n', size_begin);
    assert(size_end != std::string::npos);
    bad_size.replace(size_begin, size_end - size_begin, "payload_size=999");
    auto bad_size_result =
        net::TransportPacketCodec::decode(bad_size, net::TransportPacketCodecConfig{1024});
    assert(!bad_size_result);
    assert(bad_size_result.error().code == "transport_packet.payload_size_mismatch");

    auto bad_kind = encoded_packet;
    const auto kind_begin = bad_kind.find("kind=control");
    assert(kind_begin != std::string::npos);
    bad_kind.replace(kind_begin, std::string("kind=control").size(), "kind=bad");
    auto bad_kind_result =
        net::TransportPacketCodec::decode(bad_kind, net::TransportPacketCodecConfig{64});
    assert(!bad_kind_result);
    assert(bad_kind_result.error().code == "transport_packet.invalid_kind");

    auto bad_type = encoded_packet;
    const auto type_begin = bad_type.find("payload_type=debug.payload");
    assert(type_begin != std::string::npos);
    bad_type.replace(type_begin, std::string("payload_type=debug.payload").size(),
                     "payload_type=Bad");
    auto bad_type_result =
        net::TransportPacketCodec::decode(bad_type, net::TransportPacketCodecConfig{64});
    assert(!bad_type_result);
    assert(bad_type_result.error().code == "transport.invalid_payload_type");

    const net::TransportPacketFragmentCodecConfig fragment_config{17, 4096, 64};
    auto fragments =
        net::TransportPacketFragmentCodec::fragment_packet(encoded_packet, 42, fragment_config);
    assert(fragments);
    assert(fragments.value().size() > 1);
    assert(fragments.value().front().packet_id == 42);
    assert(fragments.value().front().fragment_count ==
           static_cast<std::uint32_t>(fragments.value().size()));
    for (const auto& fragment : fragments.value()) {
        assert(fragment.payload.size() <=
               static_cast<std::size_t>(fragment_config.max_fragment_payload_bytes));
        auto fragment_status =
            net::TransportPacketFragmentCodec::validate_fragment(fragment, fragment_config);
        assert(fragment_status);
    }

    auto encoded_fragment = net::TransportPacketFragmentCodec::encode(fragments.value().front());
    auto decoded_fragment =
        net::TransportPacketFragmentCodec::decode(encoded_fragment, fragment_config);
    assert(decoded_fragment);
    assert(decoded_fragment.value().packet_id == fragments.value().front().packet_id);
    assert(decoded_fragment.value().fragment_index == fragments.value().front().fragment_index);
    assert(decoded_fragment.value().payload == fragments.value().front().payload);

    net::TransportPacketReassembler duplicate_reassembler(fragment_config);
    auto first_fragment_result =
        duplicate_reassembler.accept_fragment(net::TransportPacketFragment{fragments.value()[0]});
    assert(first_fragment_result);
    assert(!first_fragment_result.value().complete);
    auto duplicate_fragment_result =
        duplicate_reassembler.accept_fragment(net::TransportPacketFragment{fragments.value()[0]});
    assert(duplicate_fragment_result);
    assert(!duplicate_fragment_result.value().complete);
    assert(duplicate_fragment_result.value().received_fragment_count == 1);
    auto conflicting_fragment = fragments.value()[0];
    conflicting_fragment.payload.back() = conflicting_fragment.payload.back() == 'x' ? 'y' : 'x';
    auto conflicting_duplicate =
        duplicate_reassembler.accept_fragment(std::move(conflicting_fragment));
    assert(!conflicting_duplicate);
    assert(conflicting_duplicate.error().code == "transport_fragment.conflicting_duplicate");
    assert(
        duplicate_reassembler.accept_fragment(net::TransportPacketFragment{fragments.value()[0]}));

    net::TransportPacketReassembler reassembler(fragment_config);
    std::vector<net::TransportPacketFragment> reversed_fragments = fragments.value();
    std::ranges::reverse(reversed_fragments);
    bool saw_incomplete_reassembly = false;
    std::string reassembled_packet;
    for (auto fragment : reversed_fragments) {
        auto reassembled = reassembler.accept_fragment(std::move(fragment));
        assert(reassembled);
        if (!reassembled.value().complete) {
            saw_incomplete_reassembly = true;
            continue;
        }
        reassembled_packet = std::move(reassembled).value().packet;
    }
    assert(saw_incomplete_reassembly);
    assert(reassembled_packet == encoded_packet);
    auto decoded_reassembled =
        net::TransportPacketCodec::decode(reassembled_packet, net::TransportPacketCodecConfig{64});
    assert(decoded_reassembled);
    assert(decoded_reassembled.value().message.payload == complex_payload);

    auto mismatched_fragment = fragments.value()[1];
    mismatched_fragment.fragment_count += 1;
    auto mismatched_reassembly =
        duplicate_reassembler.accept_fragment(std::move(mismatched_fragment));
    assert(!mismatched_reassembly);
    assert(mismatched_reassembly.error().code == "transport_fragment.mismatched_packet_metadata");

    auto oversized_fragment_packet = net::TransportPacketFragmentCodec::fragment_packet(
        encoded_packet, 44, net::TransportPacketFragmentCodecConfig{8, 8, 64});
    assert(!oversized_fragment_packet);
    assert(oversized_fragment_packet.error().code == "transport_fragment.packet_too_large");

    auto bad_fragment_magic = encoded_fragment;
    bad_fragment_magic.replace(0, std::string("heartstead.transport.fragment.v1").size(),
                               "bad.transport.fragment.v1");
    auto bad_fragment_magic_result =
        net::TransportPacketFragmentCodec::decode(bad_fragment_magic, fragment_config);
    assert(!bad_fragment_magic_result);
    assert(bad_fragment_magic_result.error().code == "transport_fragment.invalid_magic");

    auto bad_fragment_size = encoded_fragment;
    const auto fragment_size_begin = bad_fragment_size.find("payload_size=");
    assert(fragment_size_begin != std::string::npos);
    const auto fragment_size_end = bad_fragment_size.find('\n', fragment_size_begin);
    assert(fragment_size_end != std::string::npos);
    bad_fragment_size.replace(fragment_size_begin, fragment_size_end - fragment_size_begin,
                              "payload_size=999");
    auto bad_fragment_size_result =
        net::TransportPacketFragmentCodec::decode(bad_fragment_size, fragment_config);
    assert(!bad_fragment_size_result);
    assert(bad_fragment_size_result.error().code == "transport_fragment.payload_size_mismatch");

    net::TransportReliabilityConfig reliability_config{10, 3, 2, 2};
    assert(net::validate_transport_reliability_config(reliability_config));
    auto invalid_reliability_config = reliability_config;
    invalid_reliability_config.max_attempts = 0;
    assert(!net::validate_transport_reliability_config(invalid_reliability_config));

    const net::TransportEnvelope reliable_envelope{
        client.value(),
        transport.server_id(),
        net::TransportMessage{net::TransportMessageKind::command, net::TransportChannel::reliable,
                              21, "debug.reliable", "payload", 1000},
    };
    auto reliable_key = net::transport_reliable_message_key_from_envelope(reliable_envelope);
    assert(reliable_key);
    assert(reliable_key.value().sender == client.value());
    assert(reliable_key.value().recipient == transport.server_id());
    assert(reliable_key.value().sequence == 21);
    assert(reliable_key.value().payload_type == "debug.reliable");
    assert(net::transport_reliable_message_key_name(reliable_key.value()) ==
           "2>9:command:21:debug.reliable");

    const net::TransportEnvelope zero_sequence_control{
        transport.server_id(),
        client.value(),
        net::make_server_welcome_transport_message(welcome, 1200),
    };
    auto control_key = net::transport_reliable_message_key_from_envelope(zero_sequence_control);
    assert(control_key);
    assert(control_key.value().sender == transport.server_id());
    assert(control_key.value().recipient == client.value());
    assert(control_key.value().kind == net::TransportMessageKind::control);
    assert(control_key.value().sequence == 0);
    assert(control_key.value().payload_type == net::transport_server_welcome_payload_type);

    auto zero_sequence_command = reliable_envelope;
    zero_sequence_command.message.sequence = 0;
    auto invalid_zero_sequence_command =
        net::transport_reliable_message_key_from_envelope(zero_sequence_command);
    assert(!invalid_zero_sequence_command);
    assert(invalid_zero_sequence_command.error().code == "transport_reliability.invalid_sequence");

    auto encoded_ack = net::TransportReliabilityAckTextCodec::encode(reliable_key.value());
    auto decoded_ack = net::TransportReliabilityAckTextCodec::decode(encoded_ack);
    assert(decoded_ack);
    assert(decoded_ack.value().sender == client.value());
    assert(decoded_ack.value().recipient == transport.server_id());
    assert(decoded_ack.value().sequence == 21);

    auto ack_envelope = net::make_transport_reliability_ack_envelope(reliable_envelope, 1100);
    assert(ack_envelope);
    assert(ack_envelope.value().sender == transport.server_id());
    assert(ack_envelope.value().recipient == client.value());
    assert(ack_envelope.value().message.kind == net::TransportMessageKind::control);
    assert(ack_envelope.value().message.channel == net::TransportChannel::unreliable);
    assert(ack_envelope.value().message.sequence == 21);
    assert(ack_envelope.value().message.payload_type ==
           net::transport_reliability_ack_payload_type);

    net::TransportReliabilityTracker control_sender_tracker(transport.server_id(), client.value(),
                                                            reliability_config);
    assert(control_sender_tracker.track_send(zero_sequence_control, 1200));
    net::TransportReliabilityTracker control_receiver_tracker(client.value(), transport.server_id(),
                                                              reliability_config);
    auto received_control =
        control_receiver_tracker.accept_reliable_message(zero_sequence_control, 1210);
    assert(received_control);
    assert(received_control.value().accepted);
    assert(received_control.value().acknowledgement.sender == client.value());
    assert(received_control.value().acknowledgement.recipient == transport.server_id());
    assert(received_control.value().acknowledgement.message.sequence == 0);
    auto control_acked =
        control_sender_tracker.accept_acknowledgement(received_control.value().acknowledgement);
    assert(control_acked);
    assert(control_acked.value().matched_pending);
    assert(control_acked.value().message.sequence == 0);
    assert(control_sender_tracker.pending_count() == 0);

    net::TransportReliabilityTracker sender_tracker(client.value(), transport.server_id(),
                                                    reliability_config);
    assert(sender_tracker.track_send(reliable_envelope, 1000));
    assert(sender_tracker.pending_count() == 1);
    auto duplicate_pending = sender_tracker.track_send(reliable_envelope, 1001);
    assert(!duplicate_pending);
    assert(duplicate_pending.error().code == "transport_reliability.duplicate_pending");

    auto early_reliability_poll = sender_tracker.poll(1005);
    assert(early_reliability_poll.retransmissions.empty());
    assert(early_reliability_poll.dropped.empty());

    auto first_retry = sender_tracker.poll(1010);
    assert(first_retry.retransmissions.size() == 1);
    assert(first_retry.retransmissions.front().message.sequence == 21);
    assert(first_retry.dropped.empty());

    auto second_retry = sender_tracker.poll(1020);
    assert(second_retry.retransmissions.size() == 1);
    assert(second_retry.dropped.empty());

    auto dropped_retry = sender_tracker.poll(1030);
    assert(dropped_retry.retransmissions.empty());
    assert(dropped_retry.dropped.size() == 1);
    assert(dropped_retry.dropped.front().attempt_count == 3);
    assert(sender_tracker.pending_count() == 0);

    net::TransportReliabilityTracker ack_sender_tracker(client.value(), transport.server_id(),
                                                        reliability_config);
    assert(ack_sender_tracker.track_send(reliable_envelope, 2000));
    net::TransportReliabilityTracker receiver_tracker(transport.server_id(), client.value(),
                                                      reliability_config);
    auto received_reliable = receiver_tracker.accept_reliable_message(reliable_envelope, 2010);
    assert(received_reliable);
    assert(received_reliable.value().accepted);
    assert(!received_reliable.value().duplicate);
    assert(receiver_tracker.tracked_received_count() == 1);
    assert(received_reliable.value().acknowledgement.sender == transport.server_id());
    assert(received_reliable.value().acknowledgement.recipient == client.value());

    auto repeated_reliable = receiver_tracker.accept_reliable_message(reliable_envelope, 2011);
    assert(repeated_reliable);
    assert(!repeated_reliable.value().accepted);
    assert(repeated_reliable.value().duplicate);
    assert(repeated_reliable.value().acknowledgement.message.payload_type ==
           net::transport_reliability_ack_payload_type);

    auto acked =
        ack_sender_tracker.accept_acknowledgement(received_reliable.value().acknowledgement);
    assert(acked);
    assert(acked.value().matched_pending);
    assert(ack_sender_tracker.pending_count() == 0);
    auto duplicate_ack =
        ack_sender_tracker.accept_acknowledgement(received_reliable.value().acknowledgement);
    assert(duplicate_ack);
    assert(!duplicate_ack.value().matched_pending);

    auto invalid_ack_envelope = received_reliable.value().acknowledgement;
    invalid_ack_envelope.message.channel = net::TransportChannel::reliable;
    auto invalid_ack = ack_sender_tracker.accept_acknowledgement(invalid_ack_envelope);
    assert(!invalid_ack);
    assert(invalid_ack.error().code == "transport_reliability.invalid_ack_envelope");

    net::TransportReliabilityTracker limited_sender(client.value(), transport.server_id(),
                                                    net::TransportReliabilityConfig{10, 3, 1, 2});
    assert(limited_sender.track_send(reliable_envelope, 3000));
    auto second_reliable = reliable_envelope;
    second_reliable.message.sequence = 22;
    auto in_flight_limited = limited_sender.track_send(second_reliable, 3001);
    assert(!in_flight_limited);
    assert(in_flight_limited.error().code == "transport_reliability.in_flight_limit_reached");

    auto evicting_receiver = receiver_tracker;
    assert(evicting_receiver.accept_reliable_message(second_reliable, 4000));
    assert(evicting_receiver.tracked_received_count() == 2);
    auto third_reliable = reliable_envelope;
    third_reliable.message.sequence = 23;
    assert(evicting_receiver.accept_reliable_message(third_reliable, 4010));
    assert(evicting_receiver.tracked_received_count() == 2);
    auto evicted_replay = evicting_receiver.accept_reliable_message(reliable_envelope, 4020);
    assert(evicted_replay);
    assert(evicted_replay.value().accepted);
    assert(!evicted_replay.value().duplicate);

    net::ServerCommandDispatcher dispatcher;
    auto registered = dispatcher.register_command(net::CommandDescriptor{
        "debug.ping",
        false,
        true,
        [](const net::CommandEnvelope& envelope, const net::CommandExecutionContext&,
           world::WorldOperation&) {
            if (envelope.payload != "hello") {
                return core::Status::failure("transport_test.bad_payload",
                                             "unexpected command payload");
            }
            return core::Status::ok();
        },
    });
    assert(registered);

    auto result = dispatcher.dispatch(command.value(), net::CommandExecutionContext{});
    assert(result);
    assert(result.value().sequence == 7);
    assert(!result.value().committed_world_mutation);

    auto response = transport.send_server_to_client(
        client.value(),
        net::TransportMessage{net::TransportMessageKind::command_result,
                              net::TransportChannel::reliable, result.value().sequence,
                              "debug.ping.result", "accepted", 456});
    assert(response);

    auto client_messages = transport.drain_client_messages(client.value());
    assert(client_messages);
    assert(client_messages.value().size() == 1);
    assert(client_messages.value().front().sender == transport.server_id());
    assert(client_messages.value().front().recipient == client.value());
    assert(client_messages.value().front().message.kind ==
           net::TransportMessageKind::command_result);
    assert(client_messages.value().front().message.payload == "accepted");

    auto not_command = net::command_envelope_from_transport(client_messages.value().front());
    assert(!not_command);

    net::HostSessionCommandReport response_report;
    response_report.client_id = client.value();
    response_report.sequence = 12;
    response_report.command_type = "debug.ping";
    response_report.success = true;
    response_report.committed_world_mutation = true;
    response_report.events.push_back({"debug.changed", core::SaveId::from_value(1), "accepted"});
    response_report.reserved_ids.push_back(core::SaveId::from_value(4));
    const auto response_payload = net::host_session_result_payload(response_report);
    auto decoded_response_payload =
        net::HostSessionCommandResultBinaryCodec::decode(response_payload);
    assert(decoded_response_payload);
    assert(decoded_response_payload.value().success);
    assert(decoded_response_payload.value().sequence == 12);
    assert(decoded_response_payload.value().command_type == "debug.ping");
    assert(decoded_response_payload.value().committed_world_mutation);
    assert(decoded_response_payload.value().event_count == 1);
    assert(decoded_response_payload.value().reserved_id_count == 1);

    const net::TransportEnvelope response_envelope{
        transport.server_id(),
        client.value(),
        net::TransportMessage{net::TransportMessageKind::command_result,
                              net::TransportChannel::reliable, 12, "debug.ping.result",
                              response_payload, 900},
    };
    auto decoded_response = net::host_session_command_result_from_transport(response_envelope);
    assert(decoded_response);
    assert(decoded_response.value().sequence == 12);
    assert(decoded_response.value().success);

    auto mismatched_response = response_envelope;
    mismatched_response.message.sequence = 13;
    auto mismatched_response_result =
        net::host_session_command_result_from_transport(mismatched_response);
    assert(!mismatched_response_result);
    assert(mismatched_response_result.error().code == "host_session_result.sequence_mismatch");

    mismatched_response = response_envelope;
    mismatched_response.message.payload_type = "debug.other.result";
    mismatched_response_result =
        net::host_session_command_result_from_transport(mismatched_response);
    assert(!mismatched_response_result);
    assert(mismatched_response_result.error().code == "host_session_result.payload_type_mismatch");

    net::HostSessionCommandReport failure_report;
    failure_report.client_id = client.value();
    failure_report.sequence = 13;
    failure_report.command_type = "debug.ping";
    failure_report.error_code = "debug.failed";
    failure_report.error_message = "bad|input\nwith=delimiters;kept";
    auto failure_payload = net::host_session_result_payload(failure_report);
    auto decoded_failure_payload =
        net::HostSessionCommandResultBinaryCodec::decode(failure_payload);
    assert(decoded_failure_payload);
    assert(!decoded_failure_payload.value().success);
    assert(decoded_failure_payload.value().error_code == "debug.failed");
    assert(decoded_failure_payload.value().error_message == "bad|input\nwith=delimiters;kept");

    auto invalid_result_payload = net::HostSessionCommandResultTextCodec::encode(
        net::host_session_command_result_from_report(response_report));
    const auto status_begin = invalid_result_payload.find("status=ok");
    assert(status_begin != std::string::npos);
    invalid_result_payload.replace(status_begin, std::string("status=ok").size(), "status=maybe");
    auto invalid_result = net::HostSessionCommandResultTextCodec::decode(invalid_result_payload);
    assert(!invalid_result);
    assert(invalid_result.error().code == "host_session_result.invalid_status");

    auto disconnected = transport.disconnect_client(client.value());
    assert(disconnected);
    assert(!transport.is_client_connected(client.value()));
    assert(transport.connected_client_count() == 0);
    assert(transport.connected_client_ids().empty());
    assert(!transport.send_client_to_server(client.value(),
                                            net::make_command_transport_message(outgoing)));

    assert(net::transport_channel_name(net::TransportChannel::unreliable) == "unreliable");
    assert(net::transport_message_kind_name(net::TransportMessageKind::replication) ==
           "replication");
}

void test_host_session() {
    using namespace heartstead;

    net::ReplicationBatch relevance_batch;
    relevance_batch.command_sequence = 42;
    relevance_batch.command_type = "debug.relevance";
    relevance_batch.events.push_back({"debug.changed", core::SaveId::from_value(100), "visible"});

    net::ReplicationRelevancePolicy relevance_policy;
    relevance_policy.broadcast_by_default = false;
    relevance_policy.client_rules.push_back(
        {core::NetId::from_value(7), {core::SaveId::from_value(100)}, true});
    relevance_policy.client_rules.push_back(
        {core::NetId::from_value(8), {core::SaveId::from_value(200)}, true});
    auto relevance_report = net::ReplicationRelevance::evaluate(
        relevance_policy, relevance_batch,
        {core::NetId::from_value(7), core::NetId::from_value(8), core::NetId::from_value(9)});
    assert(relevance_report.candidate_client_count == 3);
    assert(relevance_report.relevant_client_count == 1);
    assert(relevance_report.filtered_client_count == 2);
    assert(relevance_report.decisions.front().client_id == core::NetId::from_value(7));
    assert(relevance_report.decisions.front().relevant);
    assert(relevance_report.decisions.front().relevant_event_count == 1);
    auto relevance_inspection = debug::Inspector::inspect(relevance_report);
    assert(relevance_inspection.object_type == "replication_relevance_report");
    assert(relevance_inspection.state == "partial");
    assert(relevance_inspection.find_field("relevant_client_count")->value == "1");
    assert(relevance_inspection.find_field("filtered_client_count")->value == "2");
    assert(relevance_inspection.find_field("first_relevant_client_id")->value == "7");
    assert(relevance_inspection.find_field("first_filtered_reason")->value == "filtered_subject");
    assert(!relevance_inspection.has_errors());

    std::vector<net::ReplicationBatch> intake_batches;
    net::ReplicationBatch later_intake_batch;
    later_intake_batch.command_sequence = 2;
    later_intake_batch.command_type = "debug.global";
    later_intake_batch.events.push_back({"debug.global_changed", {}, "global"});
    later_intake_batch.reserved_ids.push_back(core::SaveId::from_value(500));
    intake_batches.push_back(later_intake_batch);
    net::ReplicationBatch earlier_intake_batch;
    earlier_intake_batch.command_sequence = 1;
    earlier_intake_batch.command_type = "debug.subject";
    earlier_intake_batch.events.push_back(
        {"debug.subject_changed", core::SaveId::from_value(100), "subject"});
    intake_batches.push_back(earlier_intake_batch);
    auto intake_report = net::ReplicationIntake::summarize(intake_batches);
    assert(intake_report.batch_count == 2);
    assert(intake_report.event_count == 2);
    assert(intake_report.reserved_id_count == 1);
    assert(!intake_report.strictly_increasing_sequences);
    assert(intake_report.has_global_events);
    assert(intake_report.has_subject_events);
    assert(intake_report.first_sequence == 2);
    assert(intake_report.last_sequence == 1);
    auto intake_inspection = debug::Inspector::inspect(intake_report);
    assert(intake_inspection.object_type == "replication_intake_report");
    assert(intake_inspection.state == "invalid");
    assert(intake_inspection.find_field("batch_count")->value == "2");
    assert(intake_inspection.find_field("event_count")->value == "2");
    assert(intake_inspection.find_field("first_batch_command_type")->value == "debug.global");
    assert(intake_inspection.has_errors());
    assert(std::ranges::any_of(intake_inspection.issues, [](const auto& issue) {
        return issue.code == "replication_intake.sequence_order";
    }));

    net::HostSession session(net::HostSessionConfig{
        net::TransportHostDesc{
            net::TransportBackend::in_memory,
            net::InMemoryTransportHostConfig{core::NetId::from_value(44), 1024},
        },
        net::ReplicationRelevancePolicy{}});

    assert(session.state() == net::HostSessionState::stopped);
    assert(!session.is_running());
    assert(session.server_id() == core::NetId::from_value(44));
    assert(!session.connect_client());

    net::TransportHostDesc external_host_desc;
    external_host_desc.backend = net::TransportBackend::external_library;
    external_host_desc.external = net::ExternalTransportHostConfig{
        core::NetId::from_value(55), net::TransportEndpoint{"127.0.0.1", 0}, 1024, 2, true};
    net::HostSession external_session(
        net::HostSessionConfig{external_host_desc, net::ReplicationRelevancePolicy{}});
    auto external_started = external_session.start();
    if (net::transport_backend_info(net::TransportBackend::external_library).available) {
        assert(external_started);
        assert(external_session.is_running());
        assert(external_session.server_id() == core::NetId::from_value(55));
        auto external_client = external_session.connect_client();
        assert(external_client);
        auto external_welcome = external_session.drain_client_messages(external_client.value());
        assert(external_welcome);
        assert(external_welcome.value().size() == 1);
        assert(external_welcome.value().front().message.payload_type ==
               net::transport_server_welcome_payload_type);
        assert(external_session.stop());
    } else {
        assert(!external_started);
        assert(external_started.error().code == "transport.external_unavailable");
    }

    auto started = session.start();
    assert(started);
    assert(session.is_running());
    assert(session.server_id() == core::NetId::from_value(44));
    assert(net::host_session_state_name(session.state()) == "running");
    assert(!session.start());

    auto client = session.connect_client();
    assert(client);
    auto observer_client = session.connect_client();
    assert(observer_client);
    assert(session.connected_client_count() == 2);

    auto welcome_messages = session.drain_client_messages(client.value());
    assert(welcome_messages);
    assert(welcome_messages.value().size() == 1);
    assert(welcome_messages.value().front().message.kind == net::TransportMessageKind::control);
    assert(welcome_messages.value().front().message.payload_type ==
           net::transport_server_welcome_payload_type);
    net::ClientSession client_protocol(client.value());
    auto session_client = client_protocol.receive_server_message(welcome_messages.value().front());
    assert(session_client);
    assert(client_protocol.server_id() == core::NetId::from_value(44));
    assert(client_protocol.client_id() == client.value());
    assert(client_protocol.transport_session()->max_payload_bytes == 1024);
    assert(client_protocol.transport_session()->supports_unreliable);
    assert(client_protocol.transport_session()->enforces_reliable_command_order);

    auto observer_welcome_messages = session.drain_client_messages(observer_client.value());
    assert(observer_welcome_messages);
    assert(observer_welcome_messages.value().size() == 1);
    net::ClientSession observer_protocol(observer_client.value());
    auto observer_session =
        observer_protocol.receive_server_message(observer_welcome_messages.value().front());
    assert(observer_session);
    assert(observer_protocol.client_id() == observer_client.value());

    net::ServerCommandDispatcher dispatcher;
    auto registered = dispatcher.register_command(net::CommandDescriptor{
        "debug.mutate",
        true,
        true,
        [](const net::CommandEnvelope& envelope, const net::CommandExecutionContext&,
           world::WorldOperation& operation) {
            if (envelope.payload.empty()) {
                return core::Status::failure("host_session_test.empty_payload",
                                             "payload is required");
            }

            auto mutation = operation.record_mutation("host session mutation");
            if (!mutation) {
                return mutation;
            }
            operation.emit_event({"debug.changed", core::SaveId::from_value(1), envelope.payload});
            operation.mark_replication_dirty();
            operation.mark_save_dirty();
            return core::Status::ok();
        },
    });
    assert(registered);
    auto fail_after_mutation_registered = dispatcher.register_command(net::CommandDescriptor{
        "debug.fail_after_mutation",
        true,
        true,
        [](const net::CommandEnvelope& envelope, const net::CommandExecutionContext&,
           world::WorldOperation& operation) {
            auto mutation = operation.record_mutation("host partial mutation " + envelope.payload);
            if (!mutation) {
                return mutation;
            }
            operation.record_derived_update("HostDebug");
            operation.emit_event(
                {"debug.partial_failure", core::SaveId::from_value(2), envelope.payload});
            return core::Status::failure("host_session_test.intentional_failure",
                                         "intentional host command failure");
        },
    });
    assert(fail_after_mutation_registered);

    net::CommandEnvelope sender_mismatch;
    sender_mismatch.sequence = 1;
    sender_mismatch.sender = core::NetId::from_value(99);
    sender_mismatch.type = "debug.mutate";
    sender_mismatch.payload = "bad sender";
    assert(!session.send_client_command(client.value(), sender_mismatch));

    auto mutate = client_protocol.create_command("debug.mutate", "accepted");
    assert(mutate);
    assert(mutate.value().sequence == 1);
    assert(client_protocol.stats().pending_command_count == 1);
    auto sent = session.send_client_command(client.value(), mutate.value());
    assert(sent);
    auto duplicate_sent = session.send_client_command(client.value(), mutate.value());
    assert(!duplicate_sent);
    assert(duplicate_sent.error().code == "transport.reliable_command_replayed");

    auto tick = session.tick(dispatcher, net::CommandExecutionContext{});
    assert(tick);
    assert(tick.value().transport_retransmission_count == 0);
    assert(tick.value().transport_dropped_reliable_message_count == 0);
    assert(tick.value().transport_message_count == 1);
    assert(tick.value().command_message_count == 1);
    assert(tick.value().response_message_count == 1);
    assert(tick.value().replication_message_count == 2);
    assert(tick.value().command_reports.size() == 1);
    assert(tick.value().command_reports.front().success);
    assert(tick.value().command_reports.front().committed_world_mutation);
    assert(tick.value().command_reports.front().events.size() == 1);
    assert(tick.value().command_reports.front().operation_trace.stages.back() ==
           world::OperationStage::committed);
    assert(tick.value().command_reports.front().operation_trace.mutations.size() == 1);
    assert(tick.value().command_reports.front().operation_trace.mutations.front() ==
           "host session mutation");
    assert(tick.value().command_reports.front().operation_trace.replication_dirty);
    assert(tick.value().command_reports.front().operation_trace.save_dirty);
    assert(tick.value().replication_relevance_reports.size() == 1);
    auto tick_inspection = debug::Inspector::inspect(tick.value());
    assert(tick_inspection.object_type == "host_session_tick_result");
    assert(tick_inspection.state == "replicated");
    assert(tick_inspection.find_field("command_report_count")->value == "1");
    assert(tick_inspection.find_field("replication_relevance_report_count")->value == "1");
    assert(tick_inspection.find_field("replication_relevant_client_count")->value == "2");
    assert(tick_inspection.find_field("replication_filtered_client_count")->value == "0");
    assert(tick_inspection.find_field("accepted_command_count")->value == "1");
    assert(tick_inspection.find_field("committed_command_count")->value == "1");
    assert(!tick_inspection.has_errors());

    auto client_messages = session.drain_client_messages(client.value());
    assert(client_messages);
    assert(client_messages.value().size() == 2);
    assert(client_messages.value().front().message.kind ==
           net::TransportMessageKind::command_result);
    assert(client_messages.value().front().message.payload_type == "debug.mutate.result");
    for (const auto& message : client_messages.value()) {
        auto received = client_protocol.receive_server_message(message);
        assert(received);
    }
    auto command_results = client_protocol.drain_command_results();
    assert(command_results.size() == 1);
    assert(command_results.front().sequence == 1);
    assert(command_results.front().command_type == "debug.mutate");
    assert(command_results.front().success);
    assert(command_results.front().committed_world_mutation);
    assert(command_results.front().event_count == 1);
    assert(command_results.front().reserved_id_count == 0);
    assert(client_protocol.stats().pending_command_count == 0);
    assert(client_messages.value()[1].message.kind == net::TransportMessageKind::replication);
    assert(client_messages.value()[1].message.payload_type ==
           net::replication_world_events_payload_type);
    auto queued_intake = client_protocol.replication_intake_report();
    assert(queued_intake.batch_count == 1);
    assert(queued_intake.event_count == 1);
    assert(queued_intake.reserved_id_count == 0);
    assert(queued_intake.strictly_increasing_sequences);
    assert(!queued_intake.has_global_events);
    assert(queued_intake.has_subject_events);
    assert(queued_intake.first_sequence == 1);
    assert(queued_intake.last_sequence == 1);
    auto queued_intake_inspection = debug::Inspector::inspect(queued_intake);
    assert(queued_intake_inspection.object_type == "replication_intake_report");
    assert(queued_intake_inspection.state == "subject_events");
    assert(queued_intake_inspection.find_field("first_batch_command_type")->value ==
           "debug.mutate");
    assert(!queued_intake_inspection.has_errors());
    auto client_session_inspection = debug::Inspector::inspect(client_protocol);
    assert(client_session_inspection.object_type == "client_session");
    assert(client_session_inspection.find_field("queued_replication_batch_count")->value == "1");
    assert(client_session_inspection.find_field("queued_replication_event_count")->value == "1");
    assert(client_session_inspection.find_field("queued_replication_strictly_increasing")->value ==
           "true");
    assert(client_session_inspection.find_field("queued_replication_has_subject_events")->value ==
           "true");
    assert(!client_session_inspection.has_errors());
    auto replications = client_protocol.drain_replication_batches();
    assert(replications.size() == 1);
    assert(replications.front().command_sequence == 1);
    assert(replications.front().command_type == "debug.mutate");
    assert(replications.front().events.size() == 1);
    assert(replications.front().events.front().type == "debug.changed");
    assert(replications.front().events.front().message == "accepted");
    auto replication_batch_inspection = debug::Inspector::inspect(replications.front());
    assert(replication_batch_inspection.object_type == "replication_batch");
    assert(replication_batch_inspection.state == "events");
    assert(replication_batch_inspection.find_field("event_count")->value == "1");
    assert(replication_batch_inspection.find_field("first_event_type")->value == "debug.changed");
    assert(!replication_batch_inspection.has_errors());
    assert(client_protocol.stats().has_replication_sequence);
    assert(client_protocol.stats().last_replication_sequence == 1);
    assert(client_protocol.replication_intake_report().batch_count == 0);

    auto duplicate_replication = client_protocol.receive_server_message(client_messages.value()[1]);
    assert(!duplicate_replication);
    assert(duplicate_replication.error().code == "client_session.replayed_replication");

    net::ReplicationBatch older_batch;
    older_batch.command_sequence = 0;
    older_batch.command_type = "debug.older";
    older_batch.events.push_back({"debug.old", core::SaveId::from_value(1), "old"});
    auto older_replication = client_protocol.receive_server_message(net::TransportEnvelope{
        core::NetId::from_value(44),
        client.value(),
        net::make_replication_transport_message(older_batch, 0),
    });
    assert(!older_replication);
    assert(older_replication.error().code == "client_session.replayed_replication");
    assert(client_protocol.drain_replication_batches().empty());

    auto observer_messages = session.drain_client_messages(observer_client.value());
    assert(observer_messages);
    assert(observer_messages.value().size() == 1);
    assert(observer_messages.value().front().message.kind ==
           net::TransportMessageKind::replication);
    auto observer_received =
        observer_protocol.receive_server_message(observer_messages.value().front());
    assert(observer_received);
    auto observer_intake = observer_protocol.replication_intake_report();
    assert(observer_intake.batch_count == 1);
    assert(observer_intake.event_count == 1);
    assert(observer_intake.strictly_increasing_sequences);
    assert(observer_intake.first_sequence == 1);
    assert(observer_intake.last_sequence == 1);
    auto observer_replications = observer_protocol.drain_replication_batches();
    assert(observer_replications.size() == 1);
    assert(observer_replications.front().events.front().message == "accepted");

    net::HostSessionConfig filtered_config;
    filtered_config.transport.backend = net::TransportBackend::in_memory;
    filtered_config.transport.in_memory =
        net::InMemoryTransportHostConfig{core::NetId::from_value(66), 1024};
    filtered_config.replication_relevance.broadcast_by_default = false;
    filtered_config.replication_relevance.client_rules.push_back(
        {core::NetId::from_value(2), {core::SaveId::from_value(1)}, true});
    net::HostSession filtered_session(filtered_config);
    assert(filtered_session.start());
    auto interested_client = filtered_session.connect_client();
    auto filtered_observer_client = filtered_session.connect_client();
    assert(interested_client);
    assert(filtered_observer_client);
    assert(interested_client.value() == core::NetId::from_value(2));
    assert(filtered_observer_client.value() == core::NetId::from_value(3));
    (void)filtered_session.drain_client_messages(interested_client.value());
    (void)filtered_session.drain_client_messages(filtered_observer_client.value());

    auto filtered_command =
        net::CommandEnvelope{1, interested_client.value(), "debug.mutate", "filtered", 0};
    assert(filtered_session.send_client_command(interested_client.value(), filtered_command));
    auto filtered_tick = filtered_session.tick(dispatcher, net::CommandExecutionContext{});
    assert(filtered_tick);
    assert(filtered_tick.value().replication_message_count == 1);
    assert(filtered_tick.value().replication_relevance_reports.size() == 1);
    assert(filtered_tick.value().replication_relevance_reports.front().candidate_client_count == 2);
    assert(filtered_tick.value().replication_relevance_reports.front().relevant_client_count == 1);
    assert(filtered_tick.value().replication_relevance_reports.front().filtered_client_count == 1);
    auto filtered_tick_inspection = debug::Inspector::inspect(filtered_tick.value());
    assert(filtered_tick_inspection.state == "replicated");
    assert(filtered_tick_inspection.find_field("replication_relevant_client_count")->value == "1");
    assert(filtered_tick_inspection.find_field("replication_filtered_client_count")->value == "1");
    assert(!filtered_tick_inspection.has_errors());
    auto filtered_relevance_inspection =
        debug::Inspector::inspect(filtered_tick.value().replication_relevance_reports.front());
    assert(filtered_relevance_inspection.state == "partial");
    assert(filtered_relevance_inspection.find_field("first_relevant_client_id")->value == "2");
    assert(filtered_relevance_inspection.find_field("first_filtered_client_id")->value == "3");
    auto interested_messages = filtered_session.drain_client_messages(interested_client.value());
    assert(interested_messages);
    assert(interested_messages.value().size() == 2);
    assert(interested_messages.value()[0].message.kind ==
           net::TransportMessageKind::command_result);
    assert(interested_messages.value()[1].message.kind == net::TransportMessageKind::replication);
    auto filtered_observer_messages =
        filtered_session.drain_client_messages(filtered_observer_client.value());
    assert(filtered_observer_messages);
    assert(filtered_observer_messages.value().empty());

    auto private_interest_policy = filtered_session.replication_relevance_policy();
    private_interest_policy.private_access_rules.push_back(
        {interested_client.value(), {core::SaveId::from_value(1)}});
    filtered_session.set_replication_relevance_policy(std::move(private_interest_policy));

    world::WorldState host_interest_world;
    build::BuildPieceRecord host_interest_piece;
    host_interest_piece.object_id = core::SaveId::from_value(1);
    const auto host_interest_prototype = core::PrototypeId::parse("base:build_pieces/watch_post");
    assert(host_interest_prototype);
    host_interest_piece.prototype_id = host_interest_prototype.value();
    host_interest_piece.transform.position = {0.0, 0.0, 0.0};
    host_interest_piece.construction_state = build::ConstructionState::complete;
    assert(host_interest_world.build_objects().insert(host_interest_piece));

    world::WorldReplicationInterestOptions host_interest_options;
    host_interest_options.viewers = {
        {interested_client.value(), {1000, 0, 0}},
        {filtered_observer_client.value(), {0, 0, 0}},
    };
    host_interest_options.policy.full_radius = 4;
    host_interest_options.policy.simplified_radius = 8;
    auto host_interest_report = world::refresh_host_session_replication_interest(
        filtered_session, host_interest_world, host_interest_options);
    assert(host_interest_report);
    assert(host_interest_report.value().viewer_reports.size() == 2);
    assert(host_interest_report.value().viewer_reports[0].viewer_id == interested_client.value());
    assert(host_interest_report.value().viewer_reports[0].visible_subject_count == 0);
    assert(host_interest_report.value().viewer_reports[0].excluded_lod_subject_count == 1);
    assert(host_interest_report.value().viewer_reports[1].viewer_id ==
           filtered_observer_client.value());
    assert(host_interest_report.value().viewer_reports[1].visible_subject_count == 1);
    assert(filtered_session.replication_relevance_policy().client_rules.size() == 2);
    assert(filtered_session.replication_relevance_policy().private_access_rules.size() == 1);
    assert(filtered_session.replication_relevance_policy().private_access_rules.front().client_id ==
           interested_client.value());
    assert(filtered_session.replication_relevance_policy()
               .private_access_rules.front()
               .private_subjects == std::vector{core::SaveId::from_value(1)});
    assert(filtered_session.replication_relevance_policy().client_rules[0].client_id ==
           interested_client.value());
    assert(
        filtered_session.replication_relevance_policy().client_rules[0].visible_subjects.empty());
    assert(filtered_session.replication_relevance_policy().client_rules[1].client_id ==
           filtered_observer_client.value());
    assert(
        filtered_session.replication_relevance_policy().client_rules[1].visible_subjects.size() ==
        1);
    assert(
        filtered_session.replication_relevance_policy().client_rules[1].visible_subjects.front() ==
        core::SaveId::from_value(1));

    auto moved_command =
        net::CommandEnvelope{2, interested_client.value(), "debug.mutate", "moved", 0};
    assert(filtered_session.send_client_command(interested_client.value(), moved_command));
    auto moved_tick = filtered_session.tick(dispatcher, net::CommandExecutionContext{});
    assert(moved_tick);
    assert(moved_tick.value().replication_message_count == 1);
    assert(moved_tick.value().replication_relevance_reports.size() == 1);
    assert(moved_tick.value().replication_relevance_reports.front().relevant_client_count == 1);
    assert(moved_tick.value().replication_relevance_reports.front().filtered_client_count == 1);
    auto moved_relevance_inspection =
        debug::Inspector::inspect(moved_tick.value().replication_relevance_reports.front());
    assert(moved_relevance_inspection.state == "partial");
    assert(moved_relevance_inspection.find_field("first_relevant_client_id")->value == "3");
    assert(moved_relevance_inspection.find_field("first_filtered_client_id")->value == "2");

    auto moved_interested_messages =
        filtered_session.drain_client_messages(interested_client.value());
    assert(moved_interested_messages);
    assert(moved_interested_messages.value().size() == 1);
    assert(moved_interested_messages.value().front().message.kind ==
           net::TransportMessageKind::command_result);
    auto moved_observer_messages =
        filtered_session.drain_client_messages(filtered_observer_client.value());
    assert(moved_observer_messages);
    assert(moved_observer_messages.value().size() == 1);
    assert(moved_observer_messages.value().front().message.kind ==
           net::TransportMessageKind::replication);
    auto moved_batch =
        net::replication_batch_from_transport(moved_observer_messages.value().front());
    assert(moved_batch);
    assert(moved_batch.value().command_sequence == 2);
    assert(moved_batch.value().events.size() == 1);
    assert(moved_batch.value().events.front().message == "moved");
    assert(filtered_session.stop());

    auto malformed_tick = filtered_tick.value();
    malformed_tick.response_message_count = 0;
    auto malformed_tick_inspection = debug::Inspector::inspect(malformed_tick);
    assert(malformed_tick_inspection.state == "invalid");
    assert(malformed_tick_inspection.has_errors());
    assert(std::ranges::any_of(malformed_tick_inspection.issues, [](const auto& issue) {
        return issue.code == "host_tick.response_count_mismatch";
    }));

    auto unknown = client_protocol.create_command("debug.unknown", "missing");
    assert(unknown);
    assert(unknown.value().sequence == 2);
    assert(session.send_client_command(client.value(), unknown.value()));

    auto failed_tick = session.tick(dispatcher, net::CommandExecutionContext{});
    assert(failed_tick);
    assert(failed_tick.value().transport_retransmission_count == 0);
    assert(failed_tick.value().transport_dropped_reliable_message_count == 0);
    assert(failed_tick.value().command_reports.size() == 1);
    assert(!failed_tick.value().command_reports.front().success);
    assert(failed_tick.value().command_reports.front().error_code == "command.unknown_type");

    auto failure_messages = session.drain_client_messages(client.value());
    assert(failure_messages);
    assert(failure_messages.value().size() == 1);
    assert(failure_messages.value().front().message.payload_type == "debug.unknown.result");
    auto failed_received = client_protocol.receive_server_message(failure_messages.value().front());
    assert(failed_received);
    auto failure_results = client_protocol.drain_command_results();
    assert(failure_results.size() == 1);
    assert(failure_results.front().sequence == 2);
    assert(failure_results.front().command_type == "debug.unknown");
    assert(!failure_results.front().success);
    assert(failure_results.front().error_code == "command.unknown_type");
    assert(!failure_results.front().error_message.empty());
    assert(client_protocol.stats().pending_command_count == 0);
    auto observer_failure_messages = session.drain_client_messages(observer_client.value());
    assert(observer_failure_messages);
    assert(observer_failure_messages.value().empty());

    auto partial_failure = client_protocol.create_command("debug.fail_after_mutation", "partial");
    assert(partial_failure);
    assert(partial_failure.value().sequence == 3);
    assert(session.send_client_command(client.value(), partial_failure.value()));

    auto partial_failure_tick = session.tick(dispatcher, net::CommandExecutionContext{});
    assert(partial_failure_tick);
    assert(partial_failure_tick.value().command_reports.size() == 1);
    const auto& partial_failure_report = partial_failure_tick.value().command_reports.front();
    assert(!partial_failure_report.success);
    assert(!partial_failure_report.committed_world_mutation);
    assert(partial_failure_report.error_code == "host_session_test.intentional_failure");
    assert(partial_failure_report.events.size() == 1);
    assert(partial_failure_report.events.front().type == "debug.partial_failure");
    assert(partial_failure_report.operation_trace.stages.back() ==
           world::OperationStage::rolled_back);
    assert(partial_failure_report.operation_trace.mutations.size() == 1);
    assert(partial_failure_report.operation_trace.mutations.front() ==
           "host partial mutation partial");
    assert(partial_failure_report.operation_trace.derived_updates.size() == 1);
    assert(partial_failure_report.operation_trace.derived_updates.front() == "HostDebug");
    assert(!partial_failure_report.operation_trace.replication_dirty);
    assert(!partial_failure_report.operation_trace.save_dirty);
    assert(partial_failure_tick.value().replication_message_count == 0);
    assert(partial_failure_tick.value().replication_relevance_reports.empty());

    auto partial_failure_messages = session.drain_client_messages(client.value());
    assert(partial_failure_messages);
    assert(partial_failure_messages.value().size() == 1);
    assert(partial_failure_messages.value().front().message.kind ==
           net::TransportMessageKind::command_result);
    auto partial_failure_received =
        client_protocol.receive_server_message(partial_failure_messages.value().front());
    assert(partial_failure_received);
    auto partial_failure_results = client_protocol.drain_command_results();
    assert(partial_failure_results.size() == 1);
    assert(partial_failure_results.front().sequence == 3);
    assert(partial_failure_results.front().command_type == "debug.fail_after_mutation");
    assert(!partial_failure_results.front().success);
    assert(partial_failure_results.front().error_code == "host_session_test.intentional_failure");
    assert(partial_failure_results.front().event_count == 1);
    auto observer_partial_failure_messages = session.drain_client_messages(observer_client.value());
    assert(observer_partial_failure_messages);
    assert(observer_partial_failure_messages.value().empty());

    auto observer_disconnect = session.disconnect_client(observer_client.value());
    assert(observer_disconnect);
    assert(session.connected_client_count() == 1);
    auto observer_disconnect_messages = session.drain_client_messages(observer_client.value());
    assert(observer_disconnect_messages);
    assert(observer_disconnect_messages.value().size() == 1);
    assert(observer_disconnect_messages.value().front().message.kind ==
           net::TransportMessageKind::control);
    assert(observer_disconnect_messages.value().front().message.payload_type ==
           net::transport_server_disconnect_payload_type);
    auto observer_closed =
        observer_protocol.receive_server_message(observer_disconnect_messages.value().front());
    assert(observer_closed);
    assert(!observer_protocol.is_connected());
    assert(observer_protocol.stats().disconnect_reason_code == "host_session.disconnect");
    assert(observer_protocol.stats().disconnect_reason_message == "server disconnected client");
    assert(observer_protocol.stats().pending_command_count == 0);
    auto observer_command_after_disconnect =
        observer_protocol.create_command("debug.mutate", "after disconnect");
    assert(!observer_command_after_disconnect);
    assert(observer_command_after_disconnect.error().code == "client_session.not_connected");

    auto stopped = session.stop();
    assert(stopped);
    assert(!session.is_running());
    assert(session.connected_client_count() == 0);
    assert(!session.tick(dispatcher, net::CommandExecutionContext{}));
}

void test_command_replay_codec_and_runner() {
    const auto wall_id = heartstead::core::PrototypeId::parse("base:build_pieces/wall_frame");
    assert(wall_id);

    heartstead::modding::GenericPrototype wall_prototype;
    wall_prototype.kind = std::string(heartstead::modding::PrototypeKinds::build_piece);
    wall_prototype.id = wall_id.value();
    wall_prototype.display_name = "Wall Frame";

    heartstead::modding::PrototypeRegistry registry;
    auto registry_result = registry.build({wall_prototype});
    assert(!registry_result.has_errors());

    heartstead::replay::CommandReplayLog log;
    log.scenario_id = "build smoke | escaped";
    log.world_seed = 1234567;
    log.commands.push_back({{1, heartstead::core::NetId::from_value(22), "debug.echo",
                             "payload|with=delimiters%and text", 1000},
                            1000,
                            {}});
    log.commands.push_back({{2, heartstead::core::NetId::from_value(22), "build.place_piece",
                             "base:build_pieces/wall_frame", 1100},
                            1200,
                            {}});
    log.commands.push_back({{3, heartstead::core::NetId::from_value(22),
                             "debug.fail_after_mutation", "partial_failure", 1300},
                            1300,
                            {}});
    log.commands.front().expectation.has_committed_world_mutation = true;
    log.commands.front().expectation.committed_world_mutation = false;
    log.commands.front().expectation.event_count = 0;
    log.commands.front().expectation.reserved_id_count = 0;
    log.commands.front().expectation.last_stage = heartstead::world::OperationStage::validated;
    log.commands[1].expectation.has_committed_world_mutation = true;
    log.commands[1].expectation.committed_world_mutation = true;
    log.commands[1].expectation.event_count = 1;
    log.commands[1].expectation.reserved_id_count = 1;
    log.commands[1].expectation.event_types = {"build_piece.placed"};
    log.commands[1].expectation.reserved_ids = {heartstead::core::SaveId::from_value(2000)};
    log.commands[1].expectation.mutations = {"replay place build piece"};
    log.commands[1].expectation.derived_updates = {"RoomGraph"};
    log.commands[1].expectation.replication_dirty = true;
    log.commands[1].expectation.save_dirty = true;
    log.commands[1].expectation.last_stage = heartstead::world::OperationStage::committed;
    log.commands[2].expectation.has_committed_world_mutation = true;
    log.commands[2].expectation.committed_world_mutation = false;
    log.commands[2].expectation.event_count = 1;
    log.commands[2].expectation.reserved_id_count = 0;
    log.commands[2].expectation.event_types = {"debug.partial_failure"};
    log.commands[2].expectation.mutations = {"replay partial mutation partial_failure"};
    log.commands[2].expectation.derived_updates = {"ReplayDebug"};
    log.commands[2].expectation.replication_dirty = false;
    log.commands[2].expectation.save_dirty = false;
    log.commands[2].expectation.last_stage = heartstead::world::OperationStage::rolled_back;
    log.commands[2].expectation.error_code = "replay.intentional_failure";
    log.commands[2].expectation.error_message = "intentional replay command failure";
    assert(log.validate());

    const auto encoded = heartstead::replay::CommandReplayCodec::encode(log);
    assert(encoded.find("expect=") != std::string::npos);
    auto decoded = heartstead::replay::CommandReplayCodec::decode(encoded);
    assert(decoded);
    assert(decoded.value().scenario_id == log.scenario_id);
    assert(decoded.value().commands.size() == 3);
    assert(decoded.value().commands.front().envelope.payload == "payload|with=delimiters%and text");
    assert(decoded.value().commands.front().expectation.has_committed_world_mutation);
    assert(!decoded.value().commands.front().expectation.committed_world_mutation);
    assert(decoded.value().commands.front().expectation.last_stage ==
           heartstead::world::OperationStage::validated);
    assert(decoded.value().commands[1].expectation.event_types.front() == "build_piece.placed");
    assert(decoded.value().commands[1].expectation.reserved_ids.front().value() == 2000);
    assert(decoded.value().commands[2].expectation.error_code == "replay.intentional_failure");
    assert(decoded.value().commands[2].expectation.error_message ==
           "intentional replay command failure");

    heartstead::net::ServerCommandDispatcher dispatcher;
    auto echo_registered = dispatcher.register_command(heartstead::net::CommandDescriptor{
        "debug.echo",
        false,
        false,
        [](const heartstead::net::CommandEnvelope&, const heartstead::net::CommandExecutionContext&,
           heartstead::world::WorldOperation&) { return heartstead::core::Status::ok(); },
    });
    assert(echo_registered);

    auto build_registered = dispatcher.register_command(heartstead::net::CommandDescriptor{
        "build.place_piece",
        true,
        true,
        [](const heartstead::net::CommandEnvelope& envelope,
           const heartstead::net::CommandExecutionContext& context,
           heartstead::world::WorldOperation& operation) {
            if (context.save_ids == nullptr || context.prototypes == nullptr) {
                return heartstead::core::Status::failure("command.missing_context",
                                                         "save ids and prototypes are required");
            }

            const auto prototype_id = heartstead::core::PrototypeId::parse(envelope.payload);
            if (!prototype_id) {
                return heartstead::core::Status::failure("command.invalid_payload",
                                                         "payload must be a prototype id");
            }

            auto prototype_status = context.prototypes->require_kind(
                prototype_id.value(), heartstead::modding::PrototypeKinds::build_piece);
            if (!prototype_status) {
                return prototype_status;
            }

            auto reserved_id = operation.reserve_save_id(*context.save_ids);
            if (!reserved_id) {
                return heartstead::core::Status::failure(reserved_id.error().code,
                                                         reserved_id.error().message);
            }
            auto mutation = operation.record_mutation("replay place build piece");
            if (!mutation) {
                return mutation;
            }
            operation.record_derived_update("RoomGraph");
            operation.emit_event({"build_piece.placed", reserved_id.value(), envelope.payload});
            operation.mark_replication_dirty();
            operation.mark_save_dirty();
            return heartstead::core::Status::ok();
        },
    });
    assert(build_registered);
    auto fail_after_mutation_registered =
        dispatcher.register_command(heartstead::net::CommandDescriptor{
            "debug.fail_after_mutation",
            true,
            true,
            [](const heartstead::net::CommandEnvelope& envelope,
               const heartstead::net::CommandExecutionContext&,
               heartstead::world::WorldOperation& operation) {
                auto mutation =
                    operation.record_mutation("replay partial mutation " + envelope.payload);
                if (!mutation) {
                    return mutation;
                }
                operation.record_derived_update("ReplayDebug");
                operation.emit_event({"debug.partial_failure", {}, envelope.payload});
                return heartstead::core::Status::failure("replay.intentional_failure",
                                                         "intentional replay command failure");
            },
        });
    assert(fail_after_mutation_registered);

    heartstead::save::SaveIdAllocator save_ids(2000);
    heartstead::net::CommandExecutionContext context{
        heartstead::net::CommandExecutorRole::client_prediction,
        0,
        &save_ids,
        &registry,
    };

    auto report =
        heartstead::replay::CommandReplayRunner::run(decoded.value(), dispatcher, context);
    assert(report);
    assert(report.value().steps.size() == 3);
    assert(report.value().steps.front().success);
    assert(!report.value().steps.front().committed_world_mutation);
    assert(report.value().steps.front().expectation_checked);
    assert(report.value().steps.front().operation_trace.stages.back() ==
           heartstead::world::OperationStage::validated);
    assert(report.value().steps.front().operation_trace.mutations.empty());
    assert(report.value().steps[1].success);
    assert(report.value().steps[1].committed_world_mutation);
    assert(report.value().steps[1].expectation_checked);
    assert(report.value().steps[1].reserved_ids.front().value() == 2000);
    assert(report.value().steps[1].operation_trace.stages.back() ==
           heartstead::world::OperationStage::committed);
    assert(report.value().steps[1].operation_trace.mutations.size() == 1);
    assert(report.value().steps[1].operation_trace.mutations.front() == "replay place build piece");
    assert(report.value().steps[1].operation_trace.derived_updates.size() == 1);
    assert(report.value().steps[1].operation_trace.derived_updates.front() == "RoomGraph");
    assert(report.value().steps[1].operation_trace.replication_dirty);
    assert(report.value().steps[1].operation_trace.save_dirty);
    assert(!report.value().steps.back().success);
    assert(!report.value().steps.back().committed_world_mutation);
    assert(report.value().steps.back().expectation_checked);
    assert(report.value().steps.back().error_code == "replay.intentional_failure");
    assert(report.value().steps.back().events.size() == 1);
    assert(report.value().steps.back().events.front().type == "debug.partial_failure");
    assert(report.value().steps.back().operation_trace.stages.back() ==
           heartstead::world::OperationStage::rolled_back);
    assert(report.value().steps.back().operation_trace.mutations.size() == 1);
    assert(report.value().steps.back().operation_trace.mutations.front() ==
           "replay partial mutation partial_failure");
    auto rejected_step_inspection =
        heartstead::debug::Inspector::inspect(report.value().steps.back());
    assert(rejected_step_inspection.object_type == "command_replay_step");
    assert(rejected_step_inspection.state == "rejected");
    assert(rejected_step_inspection.find_field("success")->value == "false");
    assert(rejected_step_inspection.find_field("error_code")->value ==
           "replay.intentional_failure");
    auto report_inspection = heartstead::debug::Inspector::inspect(report.value());
    assert(report_inspection.find_field("step_count")->value == "3");
    assert(report_inspection.find_field("succeeded_step_count")->value == "2");
    assert(report_inspection.find_field("rejected_step_count")->value == "1");
    assert(report_inspection.find_field("committed_step_count")->value == "1");

    auto mismatch_log = decoded.value();
    mismatch_log.commands[1].expectation.reserved_ids = {
        heartstead::core::SaveId::from_value(2001)};
    heartstead::save::SaveIdAllocator mismatch_save_ids(2000);
    context.save_ids = &mismatch_save_ids;
    auto mismatch = heartstead::replay::CommandReplayRunner::run(mismatch_log, dispatcher, context);
    assert(!mismatch);
    assert(mismatch.error().code == "replay.expectation_mismatch");

    auto invalid = log;
    invalid.commands.back().envelope.sequence = 1;
    assert(!invalid.validate());
}

void test_item_stacks() {
    const auto clay_id = heartstead::core::PrototypeId::parse("base:items/raw_clay");
    const auto nail_id = heartstead::core::PrototypeId::parse("base:items/nails");
    assert(clay_id);
    assert(nail_id);

    auto clay_stack_result = heartstead::items::ItemStack::create(clay_id.value(), 16, 64);
    auto source_result = heartstead::items::ItemStack::create(clay_id.value(), 20, 64);
    assert(clay_stack_result);
    assert(source_result);

    auto clay_stack = std::move(clay_stack_result).value();
    auto source_stack = std::move(source_result).value();
    assert(clay_stack.add_from(source_stack, 10));
    assert(clay_stack.count == 26);
    assert(source_stack.count == 10);

    auto split = clay_stack.split(6);
    assert(split);
    assert(split.value().count == 6);
    assert(clay_stack.count == 20);

    auto nails = heartstead::items::ItemStack::create(nail_id.value(), 4, 64);
    assert(nails);
    assert(!clay_stack.can_merge_with(nails.value()));

    auto source_inventory_stack = heartstead::items::ItemStack::create(clay_id.value(), 16, 64);
    auto destination_inventory_stack = heartstead::items::ItemStack::create(clay_id.value(), 3, 64);
    assert(source_inventory_stack);
    assert(destination_inventory_stack);
    heartstead::world::InventoryRecord source_inventory{heartstead::core::SaveId::from_value(11),
                                                        {source_inventory_stack.value()}};
    heartstead::world::InventoryRecord destination_inventory{
        heartstead::core::SaveId::from_value(12), {destination_inventory_stack.value()}};
    assert(heartstead::world::transfer_inventory_items(source_inventory, destination_inventory,
                                                       {heartstead::core::SaveId::from_value(11),
                                                        heartstead::core::SaveId::from_value(12), 0,
                                                        0, 5}));
    assert(source_inventory.stacks.front().count == 11);
    assert(destination_inventory.stacks.front().count == 8);
    assert(heartstead::world::transfer_inventory_items(source_inventory, destination_inventory,
                                                       {heartstead::core::SaveId::from_value(11),
                                                        heartstead::core::SaveId::from_value(12), 0,
                                                        1, 6}));
    assert(source_inventory.stacks.front().count == 5);
    assert(destination_inventory.stacks.size() == 2);
    assert(destination_inventory.stacks.back().count == 6);
    const auto source_count_before_failure = source_inventory.stacks.front().count;
    const auto destination_before_failure = destination_inventory.stacks;
    assert(!heartstead::world::transfer_inventory_items(source_inventory, destination_inventory,
                                                        {heartstead::core::SaveId::from_value(11),
                                                         heartstead::core::SaveId::from_value(12),
                                                         0, 0, 99}));
    assert(source_inventory.stacks.front().count == source_count_before_failure);
    assert(destination_inventory.stacks.size() == destination_before_failure.size());
    assert(destination_inventory.stacks.front().count == destination_before_failure.front().count);
    assert(destination_inventory.stacks.back().count == destination_before_failure.back().count);

    const auto self_owner = heartstead::core::SaveId::from_value(21);
    heartstead::world::InventoryRecord self_inventory{
        self_owner, {source_inventory_stack.value(), destination_inventory_stack.value()}};
    assert(heartstead::world::transfer_inventory_items(self_inventory, self_inventory,
                                                       {self_owner, self_owner, 0, 0, 5}));
    assert(self_inventory.stacks[0].count == 16);
    assert(self_inventory.stacks[1].count == 3);

    assert(heartstead::world::transfer_inventory_items(self_inventory, self_inventory,
                                                       {self_owner, self_owner, 0, 2, 4}));
    assert(self_inventory.stacks.size() == 3);
    assert(self_inventory.stacks[0].count == 12);
    assert(self_inventory.stacks[2].count == 4);
    assert(heartstead::world::transfer_inventory_items(self_inventory, self_inventory,
                                                       {self_owner, self_owner, 0, 1, 5}));
    assert(self_inventory.stacks[0].count == 7);
    assert(self_inventory.stacks[1].count == 8);
    assert(heartstead::world::transfer_inventory_items(self_inventory, self_inventory,
                                                       {self_owner, self_owner, 0, 1, 7}));
    assert(self_inventory.stacks.size() == 2);
    assert(self_inventory.stacks[0].count == 15);
    assert(self_inventory.stacks[1].count == 4);

    heartstead::world::InventoryRecord duplicate_owner_a{self_owner,
                                                         {source_inventory_stack.value()}};
    heartstead::world::InventoryRecord duplicate_owner_b{self_owner,
                                                         {destination_inventory_stack.value()}};
    auto ambiguous_self_transfer = heartstead::world::transfer_inventory_items(
        duplicate_owner_a, duplicate_owner_b, {self_owner, self_owner, 0, 0, 1});
    assert(!ambiguous_self_transfer);
    assert(ambiguous_self_transfer.error().code == "inventory_transfer.owner_alias_mismatch");
    assert(duplicate_owner_a.stacks.front().count == 16);
    assert(duplicate_owner_b.stacks.front().count == 3);

    auto empty_stack_inventory = destination_inventory;
    empty_stack_inventory.stacks.front().count = 0;
    auto empty_stack_status = empty_stack_inventory.validate();
    assert(!empty_stack_status);
    assert(empty_stack_status.error().code == "world_state.invalid_inventory_stack_count");
}

void test_item_prototype_materialization() {
    const auto clay_id = heartstead::core::PrototypeId::parse("base:items/raw_clay");
    assert(clay_id);

    heartstead::modding::GenericPrototype prototype;
    prototype.kind = std::string(heartstead::modding::PrototypeKinds::item);
    prototype.id = clay_id.value();
    prototype.display_name = "Raw Clay";
    prototype.fields.emplace("stack_limit", "64");
    prototype.fields.emplace("tags", "clay,crafting,unfired");

    auto definition = heartstead::items::item_definition_from_prototype(prototype);
    assert(definition);
    assert(definition.value().prototype_id == clay_id.value());
    assert(definition.value().stack_limit == 64);
    assert(definition.value().tags.size() == 3);

    auto stack = definition.value().create_stack(12);
    assert(stack);
    assert(stack.value().prototype_id == clay_id.value());
    assert(stack.value().max_count == 64);

    prototype.fields["stack_limit"] = "0";
    auto invalid_limit = heartstead::items::item_definition_from_prototype(prototype);
    assert(!invalid_limit);

    prototype.fields["stack_limit"] = "64";
    prototype.fields["tags"] = "bad tag";
    auto invalid_tag = heartstead::items::item_definition_from_prototype(prototype);
    assert(!invalid_tag);
}

void test_cargo_records() {
    const auto cargo_id = heartstead::core::PrototypeId::parse("base:cargo/heavy_log");
    assert(cargo_id);

    heartstead::cargo::CargoRecord record;
    record.cargo_id = heartstead::core::SaveId::from_value(500);
    record.prototype_id = cargo_id.value();
    record.position = {1.0, 2.0, 3.0};
    record.mass_grams = 90000;
    record.volume_milliliters = 180000;
    record.allowed_transport_modes =
        heartstead::cargo::CargoTransportModes::of({heartstead::cargo::CargoTransportMode::cart,
                                                    heartstead::cargo::CargoTransportMode::wagon});
    record.hazard_tags.push_back("crush");

    assert(record.validate());
    assert(record.is_hazardous());
    assert(record.position.approximate_global().y == 2.0);
    assert(record.allowed_transport_modes.allows(heartstead::cargo::CargoTransportMode::cart));
    assert(!record.allowed_transport_modes.allows(heartstead::cargo::CargoTransportMode::hand));

    record.stability_per_mille = 1001;
    assert(!record.validate());

    auto invalid_hazard = record;
    invalid_hazard.stability_per_mille = 1000;
    invalid_hazard.hazard_tags = {"bad tag"};
    auto invalid_hazard_status = invalid_hazard.validate();
    assert(!invalid_hazard_status);
    assert(invalid_hazard_status.error().code == "cargo.invalid_hazard_tag");

    auto duplicate_hazard = record;
    duplicate_hazard.stability_per_mille = 1000;
    duplicate_hazard.hazard_tags = {"crush", "crush"};
    auto duplicate_hazard_status = duplicate_hazard.validate();
    assert(!duplicate_hazard_status);
    assert(duplicate_hazard_status.error().code == "cargo.duplicate_hazard_tag");

    auto unknown_transport = record;
    unknown_transport.stability_per_mille = 1000;
    unknown_transport.allowed_transport_modes = heartstead::cargo::CargoTransportModes::from_bits(
        static_cast<std::uint32_t>(heartstead::cargo::CargoTransportMode::cart) | (1u << 10u));
    auto unknown_transport_status = unknown_transport.validate();
    assert(!unknown_transport_status);
    assert(unknown_transport_status.error().code == "cargo.invalid_transport_mode");

    auto invalid_position = record;
    invalid_position.stability_per_mille = 1000;
    invalid_position.position.local_offset.x = std::numeric_limits<double>::infinity();
    auto invalid_position_status = invalid_position.validate();
    assert(!invalid_position_status);
    assert(invalid_position_status.error().code == "cargo.invalid_position");
}

void test_cargo_prototype_materialization() {
    const auto heavy_log_id = heartstead::core::PrototypeId::parse("base:cargo/heavy_log");
    assert(heavy_log_id);

    heartstead::modding::GenericPrototype prototype;
    prototype.kind = std::string(heartstead::modding::PrototypeKinds::cargo);
    prototype.id = heavy_log_id.value();
    prototype.display_name = "Heavy Log";
    prototype.fields.emplace("mass_grams", "90000");
    prototype.fields.emplace("volume_milliliters", "180000");
    prototype.fields.emplace("stability_per_mille", "850");
    prototype.fields.emplace("transport_modes", "cart,wagon,animal");
    prototype.fields.emplace("hazard_tags", "crush");

    auto definition = heartstead::cargo::cargo_definition_from_prototype(prototype);
    assert(definition);
    assert(definition.value().prototype_id == heavy_log_id.value());
    assert(definition.value().mass_grams == 90000);
    assert(definition.value().volume_milliliters == 180000);
    assert(definition.value().stability_per_mille == 850);
    assert(definition.value().is_hazardous());
    assert(definition.value().allowed_transport_modes.allows(
        heartstead::cargo::CargoTransportMode::wagon));
    assert(heartstead::cargo::cargo_transport_mode_name(
               heartstead::cargo::CargoTransportMode::animal) == "animal");

    auto record = definition.value().create_record(heartstead::core::SaveId::from_value(501));
    assert(record);
    assert(record.value().cargo_id == heartstead::core::SaveId::from_value(501));
    assert(record.value().mass_grams == 90000);
    assert(
        record.value().allowed_transport_modes.allows(heartstead::cargo::CargoTransportMode::cart));

    prototype.fields["transport_modes"] = "cart,teleporter";
    auto invalid_mode = heartstead::cargo::cargo_definition_from_prototype(prototype);
    assert(!invalid_mode);

    prototype.fields["transport_modes"] = "cart";
    prototype.fields["hazard_tags"] = "bad tag";
    auto invalid_tag = heartstead::cargo::cargo_definition_from_prototype(prototype);
    assert(!invalid_tag);
}

void test_entity_prototype_materialization() {
    const auto hand_cart_id = heartstead::core::PrototypeId::parse("base:entities/hand_cart");
    assert(hand_cart_id);

    heartstead::modding::GenericPrototype prototype;
    prototype.kind = std::string(heartstead::modding::PrototypeKinds::entity);
    prototype.id = hand_cart_id.value();
    prototype.display_name = "Hand Cart";
    prototype.fields.emplace("entity_kind", "cart");
    prototype.fields.emplace("persistent", "true");

    auto definition = heartstead::entities::entity_definition_from_prototype(prototype);
    assert(definition);
    assert(definition.value().prototype_id == hand_cart_id.value());
    assert(definition.value().kind == heartstead::entities::EntityKind::cart);
    assert(definition.value().persistent);
    assert(heartstead::entities::entity_kind_name(definition.value().kind) == "cart");

    heartstead::entities::EntityNetIdAllocator net_allocator(80);
    auto allocated_net_id = net_allocator.reserve();
    assert(allocated_net_id);
    assert(allocated_net_id.value() == heartstead::core::NetId::from_value(80));
    assert(net_allocator.peek_next() == heartstead::core::NetId::from_value(81));

    auto record = definition.value().create_record(heartstead::core::RuntimeHandle::from_value(7),
                                                   heartstead::core::NetId::from_value(8),
                                                   heartstead::core::SaveId::from_value(9));
    assert(record);
    assert(record.value().prototype_id == hand_cart_id.value());
    assert(record.value().save_id == heartstead::core::SaveId::from_value(9));
    assert(record.value().persistent);
    assert(record.value().transform.scale.x == 1.0);

    heartstead::entities::Transform spawn_transform;
    spawn_transform.position = {2.0, 3.0, 4.0};
    spawn_transform.rotation_degrees = {0.0, 90.0, 0.0};
    auto transformed_record = definition.value().create_record(
        heartstead::core::RuntimeHandle::from_value(12), heartstead::core::NetId::from_value(13),
        heartstead::core::SaveId::from_value(14), spawn_transform);
    assert(transformed_record);
    assert(transformed_record.value().transform.position.approximate_global().z == 4.0);
    assert(transformed_record.value().transform.rotation_degrees.y == 90.0);

    auto missing_save = definition.value().create_record(
        heartstead::core::RuntimeHandle::from_value(7), heartstead::core::NetId::from_value(8));
    assert(!missing_save);

    prototype.fields["persistent"] = "false";
    auto transient = heartstead::entities::entity_definition_from_prototype(prototype);
    assert(transient);
    assert(!transient.value().persistent);
    auto transient_record = transient.value().create_record(
        heartstead::core::RuntimeHandle::from_value(10), heartstead::core::NetId::from_value(11));
    assert(transient_record);
    assert(!transient_record.value().save_id.is_valid());

    prototype.fields["entity_kind"] = "vehicle";
    auto invalid_kind = heartstead::entities::entity_definition_from_prototype(prototype);
    assert(!invalid_kind);

    prototype.fields["entity_kind"] = "cart";
    prototype.fields["persistent"] = "maybe";
    auto invalid_bool = heartstead::entities::entity_definition_from_prototype(prototype);
    assert(!invalid_bool);
}

void test_scenario_prototype_materialization() {
    const auto scenario_id = heartstead::core::PrototypeId::parse("base:scenarios/homestead");
    const auto clay_id = heartstead::core::PrototypeId::parse("base:items/raw_clay");
    const auto heavy_log_id = heartstead::core::PrototypeId::parse("base:cargo/heavy_log");
    const auto showcase_id =
        heartstead::core::PrototypeId::parse("base:entities/foundation_material_showcase");
    assert(scenario_id);
    assert(clay_id);
    assert(heavy_log_id);
    assert(showcase_id);

    heartstead::modding::GenericPrototype prototype;
    prototype.kind = std::string(heartstead::modding::PrototypeKinds::scenario);
    prototype.id = scenario_id.value();
    prototype.display_name = "Homestead";
    prototype.fields.emplace("start_region", "temperate_valley");
    prototype.fields.emplace("spawn_mode", "homestead");
    prototype.fields.emplace("starting_items", "base:items/raw_clay");
    prototype.fields.emplace("starting_cargo", "base:cargo/heavy_log");
    prototype.fields.emplace("scene_entities",
                             "base:entities/foundation_material_showcase@8.5:1.0:12.5:180.0:0.75");
    prototype.fields.emplace("tags", "co_op,settlement_start");

    auto definition = heartstead::scenarios::scenario_definition_from_prototype(prototype);
    assert(definition);
    assert(definition.value().prototype_id == scenario_id.value());
    assert(definition.value().start_region == "temperate_valley");
    assert(definition.value().spawn_mode == heartstead::scenarios::ScenarioSpawnMode::homestead);
    assert(heartstead::scenarios::scenario_spawn_mode_name(definition.value().spawn_mode) ==
           "homestead");
    assert(definition.value().starting_items.size() == 1);
    assert(definition.value().starting_items.front() == clay_id.value());
    assert(definition.value().starting_cargo.size() == 1);
    assert(definition.value().starting_cargo.front() == heavy_log_id.value());
    assert(definition.value().scene_entities.size() == 1);
    assert(definition.value().scene_entities.front().prototype_id == showcase_id.value());
    assert(definition.value().scene_entities.front().transform.position ==
           (heartstead::world::WorldPosition{8.5, 1.0, 12.5}));
    assert(definition.value().scene_entities.front().transform.rotation_degrees.y == 180.0);
    assert(definition.value().scene_entities.front().transform.scale ==
           (heartstead::math::Vec3d{0.75, 0.75, 0.75}));
    assert(definition.value().tags.size() == 2);

    prototype.fields["spawn_mode"] = "somewhere";
    auto invalid_spawn = heartstead::scenarios::scenario_definition_from_prototype(prototype);
    assert(!invalid_spawn);

    prototype.fields["spawn_mode"] = "homestead";
    prototype.fields["start_region"] = "bad region";
    auto invalid_region = heartstead::scenarios::scenario_definition_from_prototype(prototype);
    assert(!invalid_region);

    prototype.fields["start_region"] = "temperate_valley";
    prototype.fields["starting_items"] = "not-a-prototype";
    auto invalid_item = heartstead::scenarios::scenario_definition_from_prototype(prototype);
    assert(!invalid_item);

    prototype.fields["starting_items"] = "base:items/raw_clay";
    prototype.fields["scene_entities"] =
        "base:entities/foundation_material_showcase@8.5:bad:12.5:0.0:1.0";
    auto invalid_scene_entity =
        heartstead::scenarios::scenario_definition_from_prototype(prototype);
    assert(!invalid_scene_entity);
    assert(invalid_scene_entity.error().code == "scenario_prototype.invalid_scene_entity");
}

void test_entity_identity() {
    const auto cart_id = heartstead::core::PrototypeId::parse("base:entities/hand_cart");
    assert(cart_id);

    heartstead::entities::RuntimeHandleAllocator allocator(7);
    auto runtime_handle = allocator.reserve();
    assert(runtime_handle);

    heartstead::entities::EntityRecord cart;
    cart.runtime_handle = runtime_handle.value();
    cart.net_id = heartstead::core::NetId::from_value(99);
    cart.save_id = heartstead::core::SaveId::from_value(700);
    cart.prototype_id = cart_id.value();
    cart.kind = heartstead::entities::EntityKind::cart;
    cart.persistent = true;
    cart.transform.position = {1.0, 2.0, 3.0};
    assert(cart.validate());

    cart.persistent = false;
    assert(!cart.validate());

    cart.persistent = true;
    cart.kind = static_cast<heartstead::entities::EntityKind>(99);
    auto status = cart.validate();
    assert(!status);
    assert(status.error().code == "entity.invalid_kind");

    cart.kind = heartstead::entities::EntityKind::cart;
    cart.transform.scale.x = 0.0;
    status = cart.validate();
    assert(!status);
    assert(status.error().code == "entity.invalid_transform_scale");
}

void test_build_piece_record() {
    const auto wall_id = heartstead::core::PrototypeId::parse("base:build_pieces/wall_frame");
    assert(wall_id);

    heartstead::build::BuildPieceRecord wall;
    wall.object_id = heartstead::core::SaveId::from_value(800);
    wall.prototype_id = wall_id.value();
    wall.construction_state = heartstead::build::ConstructionState::complete;
    wall.sockets.push_back({"top", {0.0, 1.0, 0.0}, "roof"});
    wall.network_ports.push_back(
        {"storage_access", heartstead::networks::NetworkKind::storage_access, 1});
    wall.material_tags.push_back("wood");
    wall.room_contribution_tags.push_back("wall");

    assert(wall.validate());
    assert(wall.contributes_to_rooms());
    assert(wall.exposes_network_ports());

    auto invalid_socket = wall;
    invalid_socket.sockets.push_back({"top", {0.0, 2.0, 0.0}, "duplicate"});
    auto status = invalid_socket.validate();
    assert(!status);
    assert(status.error().code == "build_piece.invalid_socket");

    invalid_socket = wall;
    invalid_socket.sockets.front().name = "Bad Socket";
    status = invalid_socket.validate();
    assert(!status);
    assert(status.error().code == "build_piece.invalid_socket_name");

    invalid_socket = wall;
    invalid_socket.sockets.front().tag = "bad tag";
    status = invalid_socket.validate();
    assert(!status);
    assert(status.error().code == "build_piece.invalid_socket_tag");

    auto invalid_port = wall;
    invalid_port.network_ports.front().name = "Bad Port";
    status = invalid_port.validate();
    assert(!status);
    assert(status.error().code == "build_piece.invalid_port_name");

    invalid_port = wall;
    invalid_port.network_ports.front().kind = static_cast<heartstead::networks::NetworkKind>(99);
    status = invalid_port.validate();
    assert(!status);
    assert(status.error().code == "build_piece.invalid_port_kind");

    auto invalid_material = wall;
    invalid_material.material_tags.push_back("wood");
    status = invalid_material.validate();
    assert(!status);
    assert(status.error().code == "build_piece.invalid_material_tag");

    auto invalid_room_tag = wall;
    invalid_room_tag.room_contribution_tags.front() = "bad tag";
    status = invalid_room_tag.validate();
    assert(!status);
    assert(status.error().code == "build_piece.invalid_room_contribution_tag");

    auto invalid_state = wall;
    invalid_state.construction_state = static_cast<heartstead::build::ConstructionState>(99);
    status = invalid_state.validate();
    assert(!status);
    assert(status.error().code == "build_piece.invalid_construction_state");

    auto invalid_transform = wall;
    invalid_transform.transform.position.local_offset.x = std::numeric_limits<double>::infinity();
    status = invalid_transform.validate();
    assert(!status);
    assert(status.error().code == "build_piece.invalid_transform");
}

void test_build_piece_prototype_materialization() {
    const auto wall_id = heartstead::core::PrototypeId::parse("base:build_pieces/wall_frame");
    assert(wall_id);

    heartstead::modding::GenericPrototype wall;
    wall.kind = std::string(heartstead::modding::PrototypeKinds::build_piece);
    wall.id = wall_id.value();
    wall.display_name = "Wall Frame";
    wall.fields.emplace("material_tags", "wood,frame");
    wall.fields.emplace("room_contribution_tags", "wall,enclosure");
    wall.fields.emplace("network_ports", "storage_access,smoke_output,fuel_input");

    auto record = heartstead::build::build_piece_record_from_prototype(
        wall, heartstead::core::SaveId::from_value(810));
    assert(record);
    assert(record.value().object_id == heartstead::core::SaveId::from_value(810));
    assert(record.value().prototype_id == wall_id.value());
    assert(record.value().material_tags.size() == 2);
    assert(record.value().room_contribution_tags.size() == 2);
    assert(record.value().network_ports.size() == 3);
    assert(record.value().network_ports[0].kind ==
           heartstead::networks::NetworkKind::storage_access);
    assert(record.value().network_ports[1].kind ==
           heartstead::networks::NetworkKind::smoke_ventilation);
    assert(record.value().network_ports[2].kind == heartstead::networks::NetworkKind::logistics);

    wall.fields["material_tags"] = "bad token";
    auto invalid = heartstead::build::build_piece_record_from_prototype(
        wall, heartstead::core::SaveId::from_value(811));
    assert(!invalid);
}

void test_assembly_prototype_materialization() {
    const auto kiln_id = heartstead::core::PrototypeId::parse("base:assemblies/clay_kiln");
    const auto firebox_id = heartstead::core::PrototypeId::parse("base:build_pieces/kiln_firebox");
    const auto chimney_id = heartstead::core::PrototypeId::parse("base:build_pieces/chimney");
    const auto shelf_id = heartstead::core::PrototypeId::parse("base:build_pieces/drying_shelf");
    assert(kiln_id);
    assert(firebox_id);
    assert(chimney_id);
    assert(shelf_id);

    heartstead::modding::GenericPrototype kiln;
    kiln.kind = std::string(heartstead::modding::PrototypeKinds::assembly);
    kiln.id = kiln_id.value();
    kiln.display_name = "Clay Kiln";
    kiln.fields.emplace("required_parts", "firebox:base:build_pieces/kiln_firebox,"
                                          "chimney:base:build_pieces/chimney");
    kiln.fields.emplace("optional_parts", "shelf:base:build_pieces/drying_shelf");
    kiln.fields.emplace("required_ports", "fuel_input,smoke_output,water_input");

    auto definition = heartstead::assemblies::assembly_definition_from_prototype(kiln);
    assert(definition);
    assert(definition.value().prototype_id == kiln_id.value());
    assert(definition.value().part_requirements.size() == 3);
    assert(definition.value().part_requirements[0].name == "firebox");
    assert(!definition.value().part_requirements[0].optional);
    assert(definition.value().part_requirements[2].name == "shelf");
    assert(definition.value().part_requirements[2].optional);
    assert(definition.value().required_ports.size() == 3);
    assert(definition.value().required_ports[0].kind ==
           heartstead::networks::NetworkKind::logistics);
    assert(definition.value().required_ports[1].kind ==
           heartstead::networks::NetworkKind::smoke_ventilation);
    assert(definition.value().required_ports[2].kind == heartstead::networks::NetworkKind::water);

    kiln.fields.erase("required_parts");
    auto missing_required = heartstead::assemblies::assembly_definition_from_prototype(kiln);
    assert(!missing_required);

    kiln.fields.emplace("required_parts", "firebox:base:build_pieces/kiln_firebox");
    kiln.fields["optional_parts"] = "firebox:base:build_pieces/drying_shelf";
    auto duplicate_name = heartstead::assemblies::assembly_definition_from_prototype(kiln);
    assert(!duplicate_name);
}

void test_assembly_validation() {
    const auto kiln_id = heartstead::core::PrototypeId::parse("base:assemblies/clay_kiln");
    const auto firebox_id = heartstead::core::PrototypeId::parse("base:build_pieces/kiln_firebox");
    const auto chimney_id = heartstead::core::PrototypeId::parse("base:build_pieces/chimney");
    assert(kiln_id);
    assert(firebox_id);
    assert(chimney_id);

    heartstead::assemblies::AssemblyDefinition definition;
    definition.prototype_id = kiln_id.value();
    definition.part_requirements.push_back({"firebox", firebox_id.value(), false});
    definition.part_requirements.push_back({"chimney", chimney_id.value(), false});
    definition.required_ports.push_back(
        {"fuel_input", heartstead::networks::NetworkKind::logistics, {}, 1});
    definition.required_ports.push_back(
        {"smoke_output", heartstead::networks::NetworkKind::smoke_ventilation, {}, 1});
    assert(definition.validate());

    heartstead::assemblies::AssemblyRecord record;
    record.assembly_id = heartstead::core::SaveId::from_value(900);
    record.root_build_piece_id = heartstead::core::SaveId::from_value(901);
    record.prototype_id = kiln_id.value();
    record.parts.push_back(
        {"firebox", heartstead::core::SaveId::from_value(901), firebox_id.value()});
    record.parts.push_back(
        {"chimney", heartstead::core::SaveId::from_value(902), chimney_id.value()});
    record.ports.push_back({"fuel_input", heartstead::networks::NetworkKind::logistics,
                            heartstead::core::SaveId::from_value(901), 1});
    record.ports.push_back({"smoke_output", heartstead::networks::NetworkKind::smoke_ventilation,
                            heartstead::core::SaveId::from_value(902), 1});

    auto validation = heartstead::assemblies::AssemblyValidator::validate(definition, record);
    assert(validation.valid);
    auto inspection = heartstead::debug::Inspector::inspect(record);
    assert(inspection.object_type == "assembly");
    assert(!inspection.has_errors());
    assert(inspection.find_field("port_sources") != nullptr);
    assert(inspection.find_field("port_sources")->value == "fuel_input:901:1,smoke_output:902:1");
    assert(inspection.find_field("total_port_capacity") != nullptr);
    assert(inspection.find_field("total_port_capacity")->value == "2");

    auto duplicate_part_record = record;
    duplicate_part_record.parts.push_back(
        {"firebox", heartstead::core::SaveId::from_value(903), firebox_id.value()});
    auto duplicate_part_status = duplicate_part_record.validate_record();
    assert(!duplicate_part_status);
    assert(duplicate_part_status.error().code == "assembly.duplicate_part");

    auto duplicate_port_record = record;
    duplicate_port_record.ports.push_back({"fuel_input", heartstead::networks::NetworkKind::power,
                                           heartstead::core::SaveId::from_value(901), 1});
    auto duplicate_port_status = duplicate_port_record.validate_record();
    assert(!duplicate_port_status);
    assert(duplicate_port_status.error().code == "assembly.duplicate_port");

    auto unknown_state_record = record;
    unknown_state_record.state = static_cast<heartstead::assemblies::AssemblyState>(255);
    auto unknown_state_status = unknown_state_record.validate_record();
    assert(!unknown_state_status);
    assert(unknown_state_status.error().code == "assembly.invalid_state");

    record.parts.pop_back();
    validation = heartstead::assemblies::AssemblyValidator::validate(definition, record);
    assert(!validation.valid);
    assert(validation.missing_required_parts.size() == 1);
}

void test_region_graph() {
    const auto clay = heartstead::core::PrototypeId::parse("base:voxels/clay");
    const auto iron = heartstead::core::PrototypeId::parse("base:voxels/iron_ore");
    assert(clay);
    assert(iron);

    heartstead::world::RegionDescriptor valley;
    valley.id = "temperate_valley";
    valley.age = "settlement_age";
    valley.biome_cluster = "temperate";
    valley.sub_biomes = {"meadow", "clay_banks"};
    valley.resource_rules.push_back({clay.value(), "surface_deposit", 0.45});
    valley.danger_gradient = 0.15;
    valley.magic_gradient = 0.10;
    valley.future_tool_layers = {"iron_pick"};
    valley.mastery_return_layers = {"deep_clay"};
    valley.ecology_parameters.emplace("rainfall", 0.70);
    valley.ecology_parameters.emplace("soil_fertility", 0.80);
    assert(valley.validate());

    heartstead::world::RegionDescriptor uplands;
    uplands.id = "stony_uplands";
    uplands.age = "settlement_age";
    uplands.biome_cluster = "temperate";
    uplands.sub_biomes = {"pine_slope"};
    uplands.resource_rules.push_back({iron.value(), "deep_vein", 0.20});
    uplands.danger_gradient = 0.40;
    uplands.magic_gradient = 0.25;
    uplands.ecology_parameters.emplace("rainfall", 0.45);
    assert(uplands.validate());

    heartstead::world::RegionGraph graph;
    assert(graph.add_region(valley));
    assert(graph.add_region(uplands));
    assert(!graph.add_region(valley));
    assert(graph.region_count() == 2);
    assert(graph.find("temperate_valley") != nullptr);
    assert(graph.ecology_parameter("temperate_valley", "rainfall").value() == 0.70);
    assert(!graph.ecology_parameter("temperate_valley", "snowpack"));

    heartstead::world::RegionConnection connection;
    connection.from_region = "temperate_valley";
    connection.to_region = "stony_uplands";
    connection.connection_kind = "road_candidate";
    connection.traversal_cost = 1.5;
    connection.capacity = 0.75;
    assert(graph.connect(connection));
    assert(!graph.connect(connection));
    assert(graph.connection_count() == 1);
    assert(graph.are_connected("temperate_valley", "stony_uplands"));
    assert(graph.are_connected("stony_uplands", "temperate_valley"));
    assert(graph.connections_for("temperate_valley").size() == 1);

    connection.to_region = "missing_region";
    assert(!graph.connect(connection));

    heartstead::world::RegionDescriptor invalid;
    invalid.id = "bad region";
    invalid.age = "settlement_age";
    invalid.biome_cluster = "temperate";
    assert(!invalid.validate());
}

void test_terrain_generation() {
    const auto clay = heartstead::core::PrototypeId::parse("base:voxels/clay");
    assert(clay);

    heartstead::world::VoxelPalette palette;
    heartstead::world::VoxelDefinition clay_definition;
    clay_definition.type = 1;
    clay_definition.prototype_id = clay.value();
    clay_definition.display_name = "Clay";
    clay_definition.terrain_material = "clay";
    clay_definition.mining_tool = "shovel";
    assert(palette.add(clay_definition));

    heartstead::world::RegionDescriptor valley;
    valley.id = "temperate_valley";
    valley.age = "settlement_age";
    valley.biome_cluster = "temperate";
    valley.resource_rules.push_back({clay.value(), "surface_deposit", 1.0});
    assert(valley.validate());

    heartstead::world::RegionGraph regions;
    assert(regions.add_region(valley));

    heartstead::world::TerrainGenerationConfig config;
    config.world_seed = 12345;
    config.region_id = "temperate_valley";
    config.base_surface_y = 8;

    auto generated = heartstead::world::DeterministicTerrainGenerator::generate_chunk(
        {0, 0, 0}, config, regions, palette);
    assert(generated);
    assert(generated.value().dirty().contains(heartstead::world::ChunkDirtyFlag::mesh));
    assert(generated.value().dirty().contains(heartstead::world::ChunkDirtyFlag::collision));
    assert(generated.value().dirty().contains(heartstead::world::ChunkDirtyFlag::lighting));
    assert(!generated.value().dirty().contains(heartstead::world::ChunkDirtyFlag::save));
    assert(!generated.value().dirty().contains(heartstead::world::ChunkDirtyFlag::replication));
    auto solid = generated.value().get({0, 0, 0});
    auto air = generated.value().get({0, 9, 0});
    assert(solid);
    assert(air);
    assert(solid.value().type == 1);
    assert(air.value().is_air());
    assert(air.value().light == 255);

    auto generated_again = heartstead::world::DeterministicTerrainGenerator::generate_chunk(
        {0, 0, 0}, config, regions, palette);
    assert(generated_again);
    assert(generated_again.value().get({3, 4, 5}).value() ==
           generated.value().get({3, 4, 5}).value());

    heartstead::world::TerrainGenerationConfig varied = config;
    varied.surface_variation = 3;
    const auto height_a =
        heartstead::world::DeterministicTerrainGenerator::surface_height_at(varied, 12, 34);
    const auto height_b =
        heartstead::world::DeterministicTerrainGenerator::surface_height_at(varied, 12, 34);
    assert(height_a == height_b);
    assert(height_a >= 5);
    assert(height_a <= 11);

    heartstead::dirty::DirtyRegionTracker dirty_regions;
    heartstead::world::ChunkDatabase chunks;
    assert(chunks.insert_generated(std::move(generated).value(), dirty_regions));
    assert(!chunks.insert_generated(std::move(generated_again).value()));
    assert(chunks.edit_log().empty());
    assert(chunks.stats().dirty_mesh_count == 1);
    assert(chunks.stats().dirty_save_count == 0);
    assert(chunks.stats().dirty_replication_count == 0);
    assert(dirty_regions.count(heartstead::dirty::DirtyRegionKind::chunk_mesh) == 1);

    auto edit_status =
        chunks.set({0, 0, 0}, {0, 9, 0}, heartstead::world::VoxelCell{1, 0}, dirty_regions);
    assert(edit_status);
    assert(chunks.edit_log().size() == 1);
    assert(chunks.stats().dirty_save_count == 1);
    assert(chunks.stats().dirty_replication_count == 1);

    heartstead::world::TerrainGenerationConfig missing_region = config;
    missing_region.region_id = "missing";
    assert(!heartstead::world::DeterministicTerrainGenerator::generate_chunk(
        {0, 0, 0}, missing_region, regions, palette));

    heartstead::world::VoxelChunk invalid_load({2, 0, 0});
    assert(!invalid_load.load_generated_cells({}));
}

void test_chunk_streamer() {
    const auto clay = heartstead::core::PrototypeId::parse("base:voxels/clay");
    assert(clay);

    heartstead::world::VoxelPalette palette;
    heartstead::world::VoxelDefinition clay_definition;
    clay_definition.type = 1;
    clay_definition.prototype_id = clay.value();
    clay_definition.display_name = "Clay";
    clay_definition.terrain_material = "clay";
    clay_definition.mining_tool = "shovel";
    assert(palette.add(clay_definition));

    heartstead::world::RegionDescriptor valley;
    valley.id = "temperate_valley";
    valley.age = "settlement_age";
    valley.biome_cluster = "temperate";
    valley.resource_rules.push_back({clay.value(), "surface_deposit", 1.0});
    assert(valley.validate());

    heartstead::world::RegionGraph regions;
    assert(regions.add_region(valley));

    heartstead::world::TerrainGenerationConfig config;
    config.world_seed = 12345;
    config.region_id = "temperate_valley";
    config.base_surface_y = 8;

    heartstead::world::WorldState state;
    auto generated =
        heartstead::world::ChunkStreamer::load_chunk(state, {0, 0, 0}, config, regions, palette);
    assert(generated);
    assert(generated.value().source == heartstead::world::ChunkStreamLoadSource::generated);
    assert(heartstead::world::chunk_stream_load_source_name(generated.value().source) ==
           std::string_view("generated"));
    assert(generated.value().generated_chunk_inserted);
    assert(!generated.value().saved_delta_applied);
    assert(generated.value().identity.is_valid());
    assert(generated.value().identity.coordinate == heartstead::world::ChunkCoord(0, 0, 0));
    assert(state.chunks().contains({0, 0, 0}));
    assert(state.chunks().stats().dirty_save_count == 0);
    assert(state.chunks().stats().dirty_replication_count == 0);

    auto already_loaded =
        heartstead::world::ChunkStreamer::load_chunk(state, {0, 0, 0}, config, regions, palette);
    assert(already_loaded);
    assert(already_loaded.value().source ==
           heartstead::world::ChunkStreamLoadSource::already_loaded);
    assert(already_loaded.value().chunk_was_already_loaded);
    assert(already_loaded.value().identity == generated.value().identity);
    assert(state.chunks().chunk_count() == 1);

    struct TestDeltaSource final : heartstead::world::IChunkEditDeltaSource {
        heartstead::save::ChunkEditSaveRecord record;
        bool fail = false;

        [[nodiscard]] heartstead::core::Result<std::optional<heartstead::save::ChunkEditSaveRecord>>
        read_chunk_delta(heartstead::world::ChunkCoord coord) const override {
            if (fail) {
                return heartstead::core::
                    Result<std::optional<heartstead::save::ChunkEditSaveRecord>>::failure(
                        "test.delta_failed", "test delta source failed");
            }
            if (coord != record.coord) {
                return heartstead::core::Result<
                    std::optional<heartstead::save::ChunkEditSaveRecord>>::success(std::nullopt);
            }
            return heartstead::core::Result<
                std::optional<heartstead::save::ChunkEditSaveRecord>>::success(record);
        }
    };
    struct TestReplicationSink final : heartstead::world::IChunkReplicationDeltaSink {
        mutable std::vector<heartstead::save::ChunkEditSaveRecord> records;
        bool fail = false;

        [[nodiscard]] heartstead::core::Status replicate_chunk_delta(
            const heartstead::save::ChunkEditSaveRecord& chunk_delta) const override {
            if (fail) {
                return heartstead::core::Status::failure("test.replication_failed",
                                                         "test replication sink failed");
            }
            records.push_back(chunk_delta);
            return heartstead::core::Status::ok();
        }
    };
    struct TestSaveSink final : heartstead::world::IChunkEditDeltaSink {
        mutable std::vector<heartstead::save::ChunkEditSaveRecord> records;
        bool fail = false;

        [[nodiscard]] heartstead::core::Status
        write_chunk_delta(const heartstead::save::ChunkEditSaveRecord& chunk_delta) const override {
            if (fail) {
                return heartstead::core::Status::failure("test.save_failed",
                                                         "test save sink failed");
            }
            records.push_back(chunk_delta);
            return heartstead::core::Status::ok();
        }
    };

    const heartstead::world::VoxelEditRecord streamed_edit{
        {2, 0, 0},
        {1, 2, 3},
        heartstead::world::VoxelCell{1, 0},
        heartstead::world::VoxelCell{9, 4},
    };
    const std::vector<const heartstead::world::VoxelEditRecord*> streamed_edits{&streamed_edit};
    TestDeltaSource source;
    source.record = {streamed_edit.chunk_coord, heartstead::world::ChunkEditDeltaTextCodec::encode(
                                                    streamed_edit.chunk_coord, streamed_edits)};

    auto generated_with_delta = heartstead::world::ChunkStreamer::load_chunk(
        state, streamed_edit.chunk_coord, config, regions, palette, &source);
    assert(generated_with_delta);
    assert(generated_with_delta.value().source ==
           heartstead::world::ChunkStreamLoadSource::generated_with_saved_delta);
    assert(generated_with_delta.value().saved_delta_applied);
    assert(generated_with_delta.value().saved_edit_count == 1);
    auto streamed_cell = state.chunks().get(streamed_edit.chunk_coord, streamed_edit.voxel_coord);
    assert(streamed_cell);
    assert(streamed_cell.value().type == 9);
    assert(streamed_cell.value().light == 4);
    assert(state.chunks().edit_log().size() == 1);
    assert(state.chunks().stats().dirty_save_count == 0);
    assert(state.chunks().stats().dirty_replication_count == 0);

    source.fail = true;
    auto failed_source = heartstead::world::ChunkStreamer::load_chunk(state, {3, 0, 0}, config,
                                                                      regions, palette, &source);
    assert(!failed_source);
    assert(failed_source.error().code == "test.delta_failed");

    const auto temp_root = make_temp_root();
    heartstead::save::FileSaveDatabase database(temp_root);
    heartstead::world::FileSaveChunkEditDeltaSource file_source(database);
    heartstead::world::FileSaveChunkEditDeltaSink file_sink(database);
    auto missing_delta = file_source.read_chunk_delta({99, 0, 0});
    assert(missing_delta);
    assert(!missing_delta.value().has_value());

    const heartstead::world::VoxelEditRecord file_edit{
        {4, 0, 0},
        {2, 3, 4},
        heartstead::world::VoxelCell{1, 0},
        heartstead::world::VoxelCell{10, 5},
    };
    const std::vector<const heartstead::world::VoxelEditRecord*> file_edits{&file_edit};
    assert(database.write_chunk_delta(
        {file_edit.chunk_coord,
         heartstead::world::ChunkEditDeltaTextCodec::encode(file_edit.chunk_coord, file_edits)}));

    heartstead::world::WorldState file_state;
    auto file_loaded = heartstead::world::ChunkStreamer::load_chunk(
        file_state, file_edit.chunk_coord, config, regions, palette, &file_source);
    assert(file_loaded);
    assert(file_loaded.value().source ==
           heartstead::world::ChunkStreamLoadSource::generated_with_saved_delta);
    assert(file_loaded.value().saved_edit_count == 1);
    auto file_cell = file_state.chunks().get(file_edit.chunk_coord, file_edit.voxel_coord);
    assert(file_cell);
    assert(file_cell.value().type == 10);
    assert(file_state.chunks().stats().dirty_save_count == 0);

    const auto negative_chunk = heartstead::world::chunk_coord_for_simulation_coord({-1, -32, 32});
    assert(negative_chunk.x == -1);
    assert(negative_chunk.y == -1);
    assert(negative_chunk.z == 1);

    auto& clean_far_chunk = state.chunks().get_or_create({5, 0, 0});
    clean_far_chunk.clear_all_dirty();
    const auto clean_far_identity = clean_far_chunk.identity();
    auto& retained_shell_chunk = state.chunks().get_or_create({2, 2, 0});
    retained_shell_chunk.clear_all_dirty();
    auto& vertical_outside_chunk = state.chunks().get_or_create({0, 3, 0});
    vertical_outside_chunk.clear_all_dirty();
    assert(state.chunks().set({6, 0, 0}, {0, 0, 0}, heartstead::world::VoxelCell{13, 0}));
    heartstead::world::ChunkStreamInterestPolicy interest_policy;
    interest_policy.load_horizontal_radius_chunks = 1;
    interest_policy.load_vertical_radius_chunks = 1;
    interest_policy.retain_horizontal_radius_chunks = 2;
    interest_policy.retain_vertical_radius_chunks = 2;
    const std::vector<heartstead::simulation::SimulationViewer> viewers{
        {heartstead::core::NetId::from_value(1), {0, 0, 0}},
    };
    auto interest =
        heartstead::world::ChunkStreamer::plan_interest(state, viewers, interest_policy);
    assert(interest);
    assert(interest.value().viewer_count == 1);
    assert(interest.value().desired_chunk_count == 15);
    assert(interest.value().load_requests.size() == 14);
    assert(std::ranges::find(interest.value().load_requests,
                             heartstead::world::ChunkCoord{-1, 0, 0}) !=
           interest.value().load_requests.end());
    assert(
        std::ranges::find(interest.value().load_requests, heartstead::world::ChunkCoord{1, 0, 1}) ==
        interest.value().load_requests.end());
    assert(
        std::ranges::find(interest.value().load_requests, heartstead::world::ChunkCoord{0, 0, 0}) ==
        interest.value().load_requests.end());
    assert(std::ranges::find(interest.value().retained_chunks,
                             heartstead::world::ChunkCoord{0, 0, 0}) !=
           interest.value().retained_chunks.end());
    assert(std::ranges::find(interest.value().retained_chunks,
                             heartstead::world::ChunkCoord{2, 2, 0}) !=
           interest.value().retained_chunks.end());
    assert(std::ranges::find(interest.value().evictable_chunks,
                             heartstead::world::ChunkCoord{0, 3, 0}) !=
           interest.value().evictable_chunks.end());
    assert(std::ranges::find(interest.value().evictable_chunks,
                             heartstead::world::ChunkCoord{5, 0, 0}) !=
           interest.value().evictable_chunks.end());
    assert(std::ranges::find(interest.value().pinned_dirty_chunks,
                             heartstead::world::ChunkCoord{6, 0, 0}) !=
           interest.value().pinned_dirty_chunks.end());

    const std::vector<heartstead::world::ChunkCoord> eviction_requests{
        {5, 0, 0},
        {6, 0, 0},
        {99, 0, 0},
        {5, 0, 0},
    };
    auto eviction = heartstead::world::ChunkStreamer::evict_chunks(state, eviction_requests);
    assert(eviction.requested_count == eviction_requests.size());
    assert(eviction.evicted_count() == 1);
    const heartstead::world::ChunkCoord clean_evicted_coord{5, 0, 0};
    assert(eviction.evicted_chunks.front() == clean_evicted_coord);
    assert(eviction.evicted_identities.size() == 1);
    assert(eviction.evicted_identities.front() == clean_far_identity);
    assert(eviction.missing_chunks.size() == 1);
    const heartstead::world::ChunkCoord missing_eviction_coord{99, 0, 0};
    assert(eviction.missing_chunks.front() == missing_eviction_coord);
    assert(eviction.retained_dirty_chunks.size() == 1);
    const heartstead::world::ChunkCoord dirty_retained_coord{6, 0, 0};
    assert(eviction.retained_dirty_chunks.front() == dirty_retained_coord);
    assert(!state.chunks().contains({5, 0, 0}));
    assert(state.chunks().contains({6, 0, 0}));

    auto& dirty_without_delta_chunk = state.chunks().get_or_create({8, 0, 0});
    dirty_without_delta_chunk.mark_dirty(heartstead::world::ChunkDirtyFlag::save);
    const std::vector<heartstead::world::ChunkCoord> save_flush_requests{
        {6, 0, 0},
        {5, 0, 0},
        {0, 0, 0},
        {8, 0, 0},
    };
    auto save_flush =
        heartstead::world::ChunkStreamer::flush_save_deltas(state, save_flush_requests, file_sink);
    assert(save_flush);
    assert(save_flush.value().requested_count == save_flush_requests.size());
    assert(save_flush.value().written_count() == 1);
    assert(save_flush.value().written_chunks.front() == dirty_retained_coord);
    assert(save_flush.value().missing_chunks.size() == 1);
    assert(save_flush.value().missing_chunks.front() == clean_evicted_coord);
    assert(save_flush.value().clean_chunks.size() == 1);
    const heartstead::world::ChunkCoord clean_loaded_coord{0, 0, 0};
    assert(save_flush.value().clean_chunks.front() == clean_loaded_coord);
    assert(save_flush.value().dirty_without_delta_chunks.size() == 1);
    const heartstead::world::ChunkCoord dirty_without_delta_coord{8, 0, 0};
    assert(save_flush.value().dirty_without_delta_chunks.front() == dirty_without_delta_coord);
    const auto* flushed_chunk = state.chunks().find(dirty_retained_coord);
    assert(flushed_chunk != nullptr);
    assert(!flushed_chunk->dirty().contains(heartstead::world::ChunkDirtyFlag::save));
    assert(flushed_chunk->dirty().contains(heartstead::world::ChunkDirtyFlag::replication));
    auto flushed_delta = database.read_chunk_delta(dirty_retained_coord);
    assert(flushed_delta);
    assert(flushed_delta.value().encoded_edit_delta.find("heartstead.chunk_edit_delta.v2") !=
           std::string::npos);

    const std::vector<heartstead::world::ChunkCoord> replication_pinned_requests{
        dirty_retained_coord};
    auto replication_pinned_eviction =
        heartstead::world::ChunkStreamer::evict_chunks(state, replication_pinned_requests);
    assert(replication_pinned_eviction.evicted_chunks.empty());
    assert(replication_pinned_eviction.retained_dirty_chunks.size() == 1);

    TestReplicationSink failing_replication_sink;
    failing_replication_sink.fail = true;
    auto failed_replication_flush = heartstead::world::ChunkStreamer::flush_replication_deltas(
        state, replication_pinned_requests, failing_replication_sink);
    assert(!failed_replication_flush);
    assert(failed_replication_flush.error().code == "test.replication_failed");
    flushed_chunk = state.chunks().find(dirty_retained_coord);
    assert(flushed_chunk != nullptr);
    assert(flushed_chunk->dirty().contains(heartstead::world::ChunkDirtyFlag::replication));

    auto& replication_without_delta_chunk = state.chunks().get_or_create({9, 0, 0});
    replication_without_delta_chunk.mark_dirty(heartstead::world::ChunkDirtyFlag::replication);
    const heartstead::world::ChunkCoord replication_without_delta_coord{9, 0, 0};
    TestReplicationSink replication_sink;
    const std::vector<heartstead::world::ChunkCoord> replication_flush_requests{
        dirty_retained_coord,
        clean_evicted_coord,
        clean_loaded_coord,
        replication_without_delta_coord,
    };
    auto replication_flush = heartstead::world::ChunkStreamer::flush_replication_deltas(
        state, replication_flush_requests, replication_sink);
    assert(replication_flush);
    assert(replication_flush.value().requested_count == replication_flush_requests.size());
    assert(replication_flush.value().replicated_count() == 1);
    assert(replication_flush.value().replicated_chunks.front() == dirty_retained_coord);
    assert(replication_flush.value().missing_chunks.size() == 1);
    assert(replication_flush.value().missing_chunks.front() == clean_evicted_coord);
    assert(replication_flush.value().clean_chunks.size() == 1);
    assert(replication_flush.value().clean_chunks.front() == clean_loaded_coord);
    assert(replication_flush.value().dirty_without_delta_chunks.size() == 1);
    assert(replication_flush.value().dirty_without_delta_chunks.front() ==
           replication_without_delta_coord);
    assert(replication_sink.records.size() == 1);
    assert(replication_sink.records.front().encoded_edit_delta.find(
               "heartstead.chunk_edit_delta.v2") != std::string::npos);
    flushed_chunk = state.chunks().find(dirty_retained_coord);
    assert(flushed_chunk != nullptr);
    assert(!flushed_chunk->dirty().contains(heartstead::world::ChunkDirtyFlag::save));
    assert(!flushed_chunk->dirty().contains(heartstead::world::ChunkDirtyFlag::replication));

    const std::vector<heartstead::world::ChunkCoord> flushed_eviction_requests{{6, 0, 0}};
    auto flushed_eviction =
        heartstead::world::ChunkStreamer::evict_chunks(state, flushed_eviction_requests);
    assert(flushed_eviction.evicted_count() == 1);
    assert(!state.chunks().contains({6, 0, 0}));

    heartstead::world::WorldState maintenance_state;
    auto& maintenance_retained = maintenance_state.chunks().get_or_create({0, 0, 0});
    maintenance_retained.clear_all_dirty();
    auto& maintenance_clean_far = maintenance_state.chunks().get_or_create({5, 0, 0});
    maintenance_clean_far.clear_all_dirty();
    assert(
        maintenance_state.chunks().set({6, 0, 0}, {0, 0, 0}, heartstead::world::VoxelCell{14, 0}));
    heartstead::world::ChunkStreamInterestPolicy maintenance_policy;
    maintenance_policy.load_horizontal_radius_chunks = 0;
    maintenance_policy.load_vertical_radius_chunks = 0;
    maintenance_policy.retain_horizontal_radius_chunks = 1;
    maintenance_policy.retain_vertical_radius_chunks = 1;
    TestSaveSink maintenance_save_sink;
    TestReplicationSink maintenance_replication_sink;
    auto maintenance = heartstead::world::ChunkStreamer::maintain_interest(
        maintenance_state, viewers, maintenance_policy, &maintenance_save_sink,
        &maintenance_replication_sink);
    assert(maintenance);
    assert(maintenance.value().interest.evictable_chunks.size() == 1);
    assert(maintenance.value().interest.pinned_dirty_chunks.size() == 1);
    assert(maintenance.value().save_flush.has_value());
    assert(maintenance.value().save_flush->written_count() == 1);
    assert(maintenance.value().replication_flush.has_value());
    assert(maintenance.value().replication_flush->replicated_count() == 1);
    assert(maintenance_save_sink.records.size() == 1);
    assert(maintenance_replication_sink.records.size() == 1);
    assert(maintenance.value().eviction.evicted_count() == 2);
    assert(maintenance.value().eviction.retained_dirty_chunks.empty());
    assert(!maintenance_state.chunks().contains({5, 0, 0}));
    assert(!maintenance_state.chunks().contains({6, 0, 0}));

    heartstead::world::WorldState no_sink_maintenance_state;
    auto& no_sink_retained = no_sink_maintenance_state.chunks().get_or_create({0, 0, 0});
    no_sink_retained.clear_all_dirty();
    auto& no_sink_clean_far = no_sink_maintenance_state.chunks().get_or_create({5, 0, 0});
    no_sink_clean_far.clear_all_dirty();
    assert(no_sink_maintenance_state.chunks().set({6, 0, 0}, {0, 0, 0},
                                                  heartstead::world::VoxelCell{15, 0}));
    auto no_sink_maintenance = heartstead::world::ChunkStreamer::maintain_interest(
        no_sink_maintenance_state, viewers, maintenance_policy);
    assert(no_sink_maintenance);
    assert(!no_sink_maintenance.value().save_flush.has_value());
    assert(!no_sink_maintenance.value().replication_flush.has_value());
    assert(no_sink_maintenance.value().eviction.evicted_count() == 1);
    assert(no_sink_maintenance.value().eviction.retained_dirty_chunks.size() == 1);
    assert(!no_sink_maintenance_state.chunks().contains({5, 0, 0}));
    assert(no_sink_maintenance_state.chunks().contains({6, 0, 0}));

    heartstead::world::WorldState failed_maintenance_state;
    auto& failed_retained = failed_maintenance_state.chunks().get_or_create({0, 0, 0});
    failed_retained.clear_all_dirty();
    assert(failed_maintenance_state.chunks().set({6, 0, 0}, {0, 0, 0},
                                                 heartstead::world::VoxelCell{16, 0}));
    TestSaveSink failing_save_sink;
    failing_save_sink.fail = true;
    auto failed_maintenance = heartstead::world::ChunkStreamer::maintain_interest(
        failed_maintenance_state, viewers, maintenance_policy, &failing_save_sink);
    assert(!failed_maintenance);
    assert(failed_maintenance.error().code == "test.save_failed");
    const heartstead::world::ChunkCoord failed_dirty_coord{6, 0, 0};
    const auto* failed_dirty_chunk = failed_maintenance_state.chunks().find(failed_dirty_coord);
    assert(failed_dirty_chunk != nullptr);
    assert(failed_dirty_chunk->dirty().contains(heartstead::world::ChunkDirtyFlag::save));
    assert(failed_dirty_chunk->dirty().contains(heartstead::world::ChunkDirtyFlag::replication));

    const heartstead::world::VoxelEditRecord maintenance_loaded_edit{
        {0, 0, 0},
        {1, 1, 1},
        heartstead::world::VoxelCell{1, 0},
        heartstead::world::VoxelCell{17, 0},
    };
    const std::vector<const heartstead::world::VoxelEditRecord*> maintenance_loaded_edits{
        &maintenance_loaded_edit};
    TestDeltaSource maintenance_loaded_source;
    maintenance_loaded_source.record = {
        maintenance_loaded_edit.chunk_coord,
        heartstead::world::ChunkEditDeltaTextCodec::encode(maintenance_loaded_edit.chunk_coord,
                                                           maintenance_loaded_edits)};
    heartstead::world::WorldState loaded_maintenance_state;
    auto& loaded_clean_far = loaded_maintenance_state.chunks().get_or_create({5, 0, 0});
    loaded_clean_far.clear_all_dirty();
    assert(loaded_maintenance_state.chunks().set({6, 0, 0}, {0, 0, 0},
                                                 heartstead::world::VoxelCell{18, 0}));
    TestSaveSink loaded_maintenance_save_sink;
    TestReplicationSink loaded_maintenance_replication_sink;
    auto loaded_maintenance = heartstead::world::ChunkStreamer::maintain_loaded_interest(
        loaded_maintenance_state, viewers, maintenance_policy, config, regions, palette,
        &maintenance_loaded_source, &loaded_maintenance_save_sink,
        &loaded_maintenance_replication_sink);
    assert(loaded_maintenance);
    assert(loaded_maintenance.value().loaded_count() == 1);
    assert(loaded_maintenance.value().loads.front().source ==
           heartstead::world::ChunkStreamLoadSource::generated_with_saved_delta);
    assert(loaded_maintenance.value().loads.front().saved_edit_count == 1);
    const auto loaded_maintenance_cell =
        loaded_maintenance_state.chunks().get({0, 0, 0}, {1, 1, 1});
    assert(loaded_maintenance_cell);
    assert(loaded_maintenance_cell.value().type == 17);
    assert(loaded_maintenance.value().save_flush.has_value());
    assert(loaded_maintenance.value().save_flush->written_count() == 1);
    assert(loaded_maintenance.value().replication_flush.has_value());
    assert(loaded_maintenance.value().replication_flush->replicated_count() == 1);
    assert(loaded_maintenance.value().eviction.evicted_count() == 2);
    assert(loaded_maintenance_state.chunks().contains({0, 0, 0}));
    assert(!loaded_maintenance_state.chunks().contains({5, 0, 0}));
    assert(!loaded_maintenance_state.chunks().contains({6, 0, 0}));

    heartstead::world::WorldState failed_load_maintenance_state;
    auto& failed_load_clean_far = failed_load_maintenance_state.chunks().get_or_create({5, 0, 0});
    failed_load_clean_far.clear_all_dirty();
    TestDeltaSource failed_load_source;
    failed_load_source.fail = true;
    auto failed_load_maintenance = heartstead::world::ChunkStreamer::maintain_loaded_interest(
        failed_load_maintenance_state, viewers, maintenance_policy, config, regions, palette,
        &failed_load_source);
    assert(!failed_load_maintenance);
    assert(failed_load_maintenance.error().code == "test.delta_failed");
    assert(failed_load_maintenance_state.chunks().contains({5, 0, 0}));

    interest_policy.load_horizontal_radius_chunks = 3;
    interest_policy.retain_horizontal_radius_chunks = 2;
    auto invalid_interest =
        heartstead::world::ChunkStreamer::plan_interest(state, viewers, interest_policy);
    assert(!invalid_interest);
    assert(invalid_interest.error().code == "chunk_stream.invalid_interest_radius");

    std::filesystem::remove_all(temp_root);
}

void test_chunk_meshing() {
    heartstead::world::VoxelChunk empty({0, 0, 0});
    auto empty_mesh = heartstead::world::ChunkMesher::build_surface_mesh(empty);
    assert(empty_mesh);
    assert(empty_mesh.value().empty());
    assert(empty_mesh.value().validate());

    heartstead::world::VoxelChunk single({0, 0, 0});
    assert(single.set({0, 0, 0}, heartstead::world::VoxelCell{7, 12}));
    auto single_mesh = heartstead::world::ChunkMesher::build_surface_mesh(single);
    assert(single_mesh);
    assert(single_mesh.value().face_count == 6);
    assert(single_mesh.value().vertices.size() == 24);
    assert(single_mesh.value().indices.size() == 36);
    const heartstead::math::Vec3f single_min{0.0F, 0.0F, 0.0F};
    const heartstead::math::Vec3f single_max{1.0F, 1.0F, 1.0F};
    assert(single_mesh.value().local_bounds.min == single_min);
    assert(single_mesh.value().local_bounds.max == single_max);
    assert(single_mesh.value().vertices.front().voxel_type == 7);
    assert(single_mesh.value().vertices.front().light == 12);
    assert(single_mesh.value().validate());

    heartstead::world::VoxelChunk adjacent({1, 0, 0});
    assert(adjacent.set({0, 0, 0}, heartstead::world::VoxelCell{7, 0}));
    assert(adjacent.set({1, 0, 0}, heartstead::world::VoxelCell{7, 0}));
    auto adjacent_mesh = heartstead::world::ChunkMesher::build_surface_mesh(adjacent);
    assert(adjacent_mesh);
    assert(adjacent_mesh.value().face_count == 10);
    assert(adjacent_mesh.value().vertices.size() == 40);
    assert(adjacent_mesh.value().indices.size() == 60);
    const heartstead::math::Vec3f adjacent_max{2.0F, 1.0F, 1.0F};
    assert(adjacent_mesh.value().local_bounds.max == adjacent_max);

    const auto clay = heartstead::core::PrototypeId::parse("base:voxels/clay");
    assert(clay);
    heartstead::world::VoxelPalette palette;
    heartstead::world::VoxelDefinition clay_definition;
    clay_definition.type = 1;
    clay_definition.prototype_id = clay.value();
    clay_definition.display_name = "Clay";
    clay_definition.terrain_material = "clay";
    clay_definition.mining_tool = "shovel";
    assert(palette.add(clay_definition));

    heartstead::world::RegionDescriptor region;
    region.id = "temperate_valley";
    region.age = "settlement_age";
    region.biome_cluster = "temperate";
    region.resource_rules.push_back({clay.value(), "surface_deposit", 1.0});
    heartstead::world::RegionGraph regions;
    assert(regions.add_region(region));

    heartstead::world::TerrainGenerationConfig generation;
    generation.world_seed = 77;
    generation.region_id = "temperate_valley";
    generation.base_surface_y = 0;
    auto generated = heartstead::world::DeterministicTerrainGenerator::generate_chunk(
        {0, 0, 0}, generation, regions, palette);
    assert(generated);
    auto generated_mesh = heartstead::world::ChunkMesher::build_surface_mesh(generated.value());
    assert(generated_mesh);
    assert(generated_mesh.value().face_count > 0);
    assert(generated_mesh.value().validate());

    auto invalid_mesh = generated_mesh.value();
    invalid_mesh.indices.push_back(static_cast<std::uint32_t>(invalid_mesh.vertices.size() + 1));
    assert(!invalid_mesh.validate());
}

void test_world_state_databases() {
    heartstead::world::WorldStateDesc desc;
    desc.metadata.game_version = "engine_test";
    desc.metadata.world_seed = 99;
    desc.next_save_id = 1000;
    desc.next_runtime_handle = 50;
    heartstead::world::WorldState state(desc);

    auto build_id = state.save_ids().reserve();
    auto cargo_id = state.save_ids().reserve();
    auto entity_save_id = state.save_ids().reserve();
    auto inventory_owner_id = state.save_ids().reserve();
    auto assembly_id = state.save_ids().reserve();
    auto process_owner_id = state.save_ids().reserve();
    assert(build_id);
    assert(cargo_id);
    assert(entity_save_id);
    assert(inventory_owner_id);
    assert(assembly_id);
    assert(process_owner_id);

    auto build_prototype = heartstead::core::PrototypeId::parse("base:build_pieces/test_wall");
    auto cargo_prototype = heartstead::core::PrototypeId::parse("base:cargo/test_log");
    auto entity_prototype = heartstead::core::PrototypeId::parse("base:entities/test_cart");
    auto item_prototype = heartstead::core::PrototypeId::parse("base:items/test_seed");
    auto workpiece_prototype = heartstead::core::PrototypeId::parse("base:workpieces/test_clay");
    auto assembly_prototype = heartstead::core::PrototypeId::parse("base:assemblies/test_kiln");
    auto process_prototype = heartstead::core::PrototypeId::parse("base:processes/test_drying");
    assert(build_prototype);
    assert(cargo_prototype);
    assert(entity_prototype);
    assert(item_prototype);
    assert(workpiece_prototype);
    assert(assembly_prototype);
    assert(process_prototype);

    auto voxel_status = state.chunks().set({0, 0, 0}, {1, 2, 3}, heartstead::world::VoxelCell{4, 0},
                                           state.dirty_regions());
    assert(voxel_status);
    assert(state.chunks().chunk_count() == 1);
    assert(!state.dirty_regions().empty());

    auto clay_region_resource = heartstead::core::PrototypeId::parse("base:voxels/clay");
    assert(clay_region_resource);
    heartstead::world::RegionDescriptor region;
    region.id = "temperate_valley";
    region.age = "settlement_age";
    region.biome_cluster = "temperate";
    region.sub_biomes = {"meadow"};
    region.resource_rules.push_back({clay_region_resource.value(), "surface_deposit", 0.5});
    region.ecology_parameters.emplace("soil_fertility", 0.8);
    assert(state.regions().add_region(std::move(region)));

    heartstead::rooms::RoomRecord room;
    room.id = heartstead::rooms::RoomId::from_value(5);
    room.label = "Workshop";
    room.volume_cells = 16;
    room.source_build_piece_ids.push_back(process_owner_id.value());
    room.metrics.enclosure_per_mille = 900;
    room.metrics.roof_coverage_per_mille = 850;
    room.metrics.wall_coverage_per_mille = 850;
    room.metrics.dryness = 300;
    room.descriptors = heartstead::rooms::RoomEvaluator::evaluate(room.metrics);
    assert(state.rooms().add_or_replace(room));
    assert(state.rooms().find(heartstead::rooms::RoomId::from_value(5)) != nullptr);
    assert(state.rooms().room_count() == 1);

    heartstead::build::BuildPieceRecord build_piece;
    build_piece.object_id = build_id.value();
    build_piece.prototype_id = build_prototype.value();
    build_piece.construction_state = heartstead::build::ConstructionState::complete;
    build_piece.room_contribution_tags.push_back("wall");
    build_piece.network_ports.push_back(
        {"storage", heartstead::networks::NetworkKind::storage_access, 2});
    assert(state.build_objects().insert(build_piece));
    assert(state.build_objects().find(build_id.value()) != nullptr);
    assert(!state.build_objects().insert(build_piece));

    auto runtime_handle = state.runtime_handles().reserve();
    assert(runtime_handle);
    heartstead::entities::EntityRecord entity;
    entity.runtime_handle = runtime_handle.value();
    entity.net_id = heartstead::core::NetId::from_value(7);
    entity.save_id = entity_save_id.value();
    entity.prototype_id = entity_prototype.value();
    entity.kind = heartstead::entities::EntityKind::cart;
    entity.persistent = true;
    assert(state.entities().insert(entity));
    assert(state.entities().find(runtime_handle.value()) != nullptr);
    assert(state.entities().find_by_net_id(entity.net_id) != nullptr);
    assert(state.entities().find_by_save_id(entity_save_id.value()) != nullptr);
    assert(!state.entities().insert(entity));

    heartstead::cargo::CargoRecord cargo;
    cargo.cargo_id = cargo_id.value();
    cargo.prototype_id = cargo_prototype.value();
    cargo.position = {-4.0, 0.0, 8.0};
    cargo.mass_grams = 120000;
    cargo.volume_milliliters = 200000;
    cargo.allowed_transport_modes =
        heartstead::cargo::CargoTransportModes::of({heartstead::cargo::CargoTransportMode::cart});
    assert(state.cargo().insert(cargo));
    assert(state.cargo().find(cargo_id.value()) != nullptr);

    auto stack = heartstead::items::ItemStack::create(item_prototype.value(), 4, 64);
    assert(stack);
    heartstead::world::InventoryRecord inventory;
    inventory.owner_id = inventory_owner_id.value();
    inventory.stacks.push_back(stack.value());
    assert(state.inventories().insert(inventory));
    assert(state.inventories().find(inventory_owner_id.value()) != nullptr);

    auto grid = heartstead::workpieces::WorkpieceGrid::create({4, 4, 4});
    assert(grid);
    assert(grid.value().apply({heartstead::workpieces::WorkpieceOperationKind::add_cell,
                               {1, 1, 1},
                               heartstead::workpieces::WorkpieceCell::solid(2)}));
    heartstead::world::WorkpieceRecord workpiece{
        heartstead::core::WorkpieceId::from_value(3),
        workpiece_prototype.value(),
        std::move(grid).value(),
    };
    assert(state.workpieces().insert(std::move(workpiece)));
    assert(state.workpieces().find(heartstead::core::WorkpieceId::from_value(3)) != nullptr);

    heartstead::assemblies::AssemblyRecord assembly;
    assembly.assembly_id = assembly_id.value();
    assembly.root_build_piece_id = build_id.value();
    assembly.prototype_id = assembly_prototype.value();
    assembly.ports.push_back(
        {"storage", heartstead::networks::NetworkKind::storage_access, build_piece.object_id, 1});
    assert(state.assemblies().insert(assembly));
    assert(state.assemblies().find(assembly_id.value()) != nullptr);
    assert(state.contains_saved_object(build_id.value()));
    assert(state.contains_saved_object(cargo_id.value()));
    assert(state.contains_saved_object(entity_save_id.value()));
    assert(state.contains_saved_object(assembly_id.value()));
    assert(!state.contains_saved_object(inventory_owner_id.value()));
    assert(!state.contains_saved_object(process_owner_id.value()));
    assert(!state.contains_saved_object(heartstead::core::SaveId{}));
    assert(!state.contains_saved_object(heartstead::core::SaveId::from_value(999999)));

    auto rebuilt_networks =
        state.networks().rebuild_from_ports(state.build_objects(), state.assemblies());
    assert(rebuilt_networks);
    assert(rebuilt_networks.value().network_count == 1);
    assert(rebuilt_networks.value().build_piece_port_count == 1);
    assert(rebuilt_networks.value().assembly_port_count == 1);
    assert(rebuilt_networks.value().generated_node_count == 2);
    assert(rebuilt_networks.value().generated_edge_count == 1);
    const auto* storage_network =
        state.networks().find(heartstead::networks::NetworkKind::storage_access);
    assert(storage_network != nullptr);
    assert(!storage_network->is_dirty());
    assert(storage_network->node_count() == 2);
    assert(storage_network->edge_count() == 1);
    assert(storage_network->port_count() == 2);
    assert(storage_network->total_port_capacity() == 3);

    auto process = heartstead::processes::ProcessRuntime::create(
        heartstead::core::ProcessId::from_value(12), process_owner_id.value(),
        process_prototype.value(), 100, 500);
    assert(process);
    assert(state.processes().insert(std::move(process).value()));
    assert(state.processes().find(heartstead::core::ProcessId::from_value(12)) != nullptr);
    assert(state.processes().find_by_owner(process_owner_id.value()).size() == 1);
    auto advanced = state.processes().advance_all(600, heartstead::processes::ProcessModifiers{});
    assert(advanced);
    assert(advanced.value() == 1);
    assert(state.processes().find(heartstead::core::ProcessId::from_value(12))->is_complete());

    auto& road = state.networks().get_or_create(heartstead::networks::NetworkKind::road);
    assert(road.add_node(heartstead::networks::NetworkNode{
        heartstead::networks::NetworkNodeId::from_value(1), {0, 0, 0}, 1, "road start"}));
    assert(state.networks().find(heartstead::networks::NetworkKind::road) != nullptr);

    assert(state.mod_states().insert({"base", "tutorial_flags", "encoded"}));
    assert(state.mod_states().find("base", "tutorial_flags") != nullptr);
    assert(!state.mod_states().insert({"base", "tutorial_flags", "duplicate"}));

    const auto stats = state.stats();
    assert(stats.chunk_count == 1);
    assert(stats.region_count == 1);
    assert(stats.region_connection_count == 0);
    assert(stats.dirty_region_count > 0);
    assert(stats.build_object_count == 1);
    assert(stats.entity_count == 1);
    assert(stats.persistent_entity_count == 1);
    assert(stats.cargo_count == 1);
    assert(stats.inventory_count == 1);
    assert(stats.workpiece_count == 1);
    assert(stats.assembly_count == 1);
    assert(stats.process_count == 1);
    assert(stats.room_count == 1);
    assert(stats.network_count == 2);
    assert(stats.mod_state_count == 1);
}

void test_world_snapshot_bridge() {
    heartstead::world::WorldStateDesc desc;
    desc.metadata.game_version = "snapshot_bridge_test";
    desc.metadata.world_seed = 12345;
    desc.metadata.enabled_mods.push_back({"base", "0.0.1", "hash"});
    desc.next_save_id = 200;
    desc.next_runtime_handle = 20;
    heartstead::world::WorldState state(desc);

    auto wall_id = state.save_ids().reserve();
    auto cargo_id = state.save_ids().reserve();
    auto entity_id = state.save_ids().reserve();
    auto assembly_id = state.save_ids().reserve();
    assert(wall_id);
    assert(cargo_id);
    assert(entity_id);
    assert(assembly_id);

    const auto wall_prototype = heartstead::core::PrototypeId::parse("base:build_pieces/wall");
    const auto cargo_prototype = heartstead::core::PrototypeId::parse("base:cargo/heavy_log");
    const auto entity_prototype = heartstead::core::PrototypeId::parse("base:entities/cart");
    const auto item_prototype = heartstead::core::PrototypeId::parse("base:items/seed");
    const auto workpiece_prototype = heartstead::core::PrototypeId::parse("base:workpieces/clay");
    const auto assembly_prototype = heartstead::core::PrototypeId::parse("base:assemblies/kiln");
    const auto process_prototype = heartstead::core::PrototypeId::parse("base:processes/drying");
    assert(wall_prototype);
    assert(cargo_prototype);
    assert(entity_prototype);
    assert(item_prototype);
    assert(workpiece_prototype);
    assert(assembly_prototype);
    assert(process_prototype);

    const auto make_bridge_registry = [&]() {
        using heartstead::modding::GenericPrototype;
        using heartstead::modding::PrototypeKinds;

        auto make_prototype = [](std::string_view kind, const heartstead::core::PrototypeId& id) {
            GenericPrototype prototype;
            prototype.kind = std::string(kind);
            prototype.id = id;
            prototype.display_name = id.value();
            return prototype;
        };

        std::vector<GenericPrototype> prototypes;
        prototypes.push_back(make_prototype(PrototypeKinds::build_piece, wall_prototype.value()));
        prototypes.push_back(make_prototype(PrototypeKinds::cargo, cargo_prototype.value()));
        prototypes.push_back(make_prototype(PrototypeKinds::entity, entity_prototype.value()));
        prototypes.push_back(make_prototype(PrototypeKinds::item, item_prototype.value()));
        prototypes.push_back(
            make_prototype(PrototypeKinds::workpiece, workpiece_prototype.value()));
        prototypes.push_back(make_prototype(PrototypeKinds::assembly, assembly_prototype.value()));
        prototypes.push_back(make_prototype(PrototypeKinds::process, process_prototype.value()));

        heartstead::modding::PrototypeRegistry registry;
        auto result = registry.build(std::move(prototypes));
        assert(!result.has_errors());
        return registry;
    };

    assert(state.chunks().set({2, 0, 0}, {1, 1, 1}, heartstead::world::VoxelCell{8, 2},
                              state.dirty_regions()));

    heartstead::build::BuildPieceRecord wall;
    wall.object_id = wall_id.value();
    wall.prototype_id = wall_prototype.value();
    wall.room_contribution_tags.push_back("wall");
    assert(state.build_objects().insert(wall));

    heartstead::cargo::CargoRecord cargo;
    cargo.cargo_id = cargo_id.value();
    cargo.prototype_id = cargo_prototype.value();
    cargo.position = {-2.0, 1.0, 9.0};
    cargo.mass_grams = 40000;
    cargo.volume_milliliters = 120000;
    cargo.allowed_transport_modes =
        heartstead::cargo::CargoTransportModes::of({heartstead::cargo::CargoTransportMode::cart});
    assert(state.cargo().insert(cargo));

    auto runtime_handle = state.runtime_handles().reserve();
    assert(runtime_handle);
    heartstead::entities::EntityRecord entity;
    entity.runtime_handle = runtime_handle.value();
    entity.net_id = heartstead::core::NetId::from_value(77);
    entity.save_id = entity_id.value();
    entity.prototype_id = entity_prototype.value();
    entity.kind = heartstead::entities::EntityKind::cart;
    entity.persistent = true;
    entity.sleeping = true;
    entity.transform.position = {6.0, 1.0, -3.0};
    entity.transform.rotation_degrees = {0.0, 270.0, 0.0};
    assert(state.entities().insert(entity));

    auto stack = heartstead::items::ItemStack::create(item_prototype.value(), 3, 64);
    assert(stack);
    assert(state.inventories().insert({wall_id.value(), {stack.value()}}));

    auto grid = heartstead::workpieces::WorkpieceGrid::create({3, 3, 3});
    assert(grid);
    assert(grid.value().apply({heartstead::workpieces::WorkpieceOperationKind::add_cell,
                               {0, 1, 2},
                               heartstead::workpieces::WorkpieceCell::solid(9)}));
    heartstead::world::WorkpieceRecord saved_workpiece{
        heartstead::core::WorkpieceId::from_value(42), workpiece_prototype.value(),
        std::move(grid).value()};
    saved_workpiece.owner_session = heartstead::core::NetId::from_value(123);
    assert(state.workpieces().insert(std::move(saved_workpiece)));

    heartstead::assemblies::AssemblyRecord assembly;
    assembly.assembly_id = assembly_id.value();
    assembly.root_build_piece_id = wall_id.value();
    assembly.prototype_id = assembly_prototype.value();
    assert(state.assemblies().insert(assembly));

    auto process = heartstead::processes::ProcessRuntime::create(
        heartstead::core::ProcessId::from_value(5), wall_id.value(), process_prototype.value(), 10,
        1000);
    assert(process);
    assert(state.processes().insert(std::move(process).value()));
    assert(state.mod_states().insert({"base", "bridge_test", "state"}));

    auto snapshot = heartstead::world::WorldSnapshotBridge::export_snapshot(state);
    assert(snapshot);
    assert(snapshot.value().metadata.world_seed == 12345);
    assert(snapshot.value().chunk_edits.size() == 1);
    assert(snapshot.value().chunk_edits.front().encoded_edit_delta.find(
               "heartstead.chunk_edit_delta.v2") != std::string::npos);
    assert(snapshot.value().build_pieces.size() == 1);
    assert(snapshot.value().cargo_records.size() == 1);
    assert(snapshot.value().cargo_records.front().position.approximate_global().z == 9.0);
    assert(snapshot.value().entities.size() == 1);
    assert(snapshot.value().entities.front().sleeping);
    assert(snapshot.value().entities.front().transform.position.approximate_global().x == 6.0);
    assert(snapshot.value().entities.front().transform.rotation_degrees.y == 270.0);
    assert(snapshot.value().inventories.size() == 1);
    assert(snapshot.value().workpieces.size() == 1);
    assert(!snapshot.value().workpieces.front().encoded_cells.empty());
    assert(!snapshot.value().workpieces.front().owner_session.is_valid());
    assert(snapshot.value().assemblies.size() == 1);
    assert(snapshot.value().processes.size() == 1);
    assert(snapshot.value().mod_states.size() == 1);

    heartstead::world::WorldSnapshotLoadConfig load_config;
    load_config.next_save_id = 1;
    load_config.next_runtime_handle = 500;
    load_config.next_entity_net_id = 9000;
    auto bridge_registry = make_bridge_registry();
    // Legacy snapshots may contain a connection-local NetId. Import must never treat it as a
    // stable owner after a server restart.
    snapshot.value().workpieces.front().owner_session = heartstead::core::NetId::from_value(999);
    auto imported = heartstead::world::WorldSnapshotBridge::import_validated_snapshot(
        snapshot.value(), bridge_registry, load_config);
    assert(imported);
    assert(imported.value().metadata().world_seed == 12345);
    assert(imported.value().save_ids().peek_next() == heartstead::core::SaveId::from_value(204));
    assert(imported.value().process_ids().peek_next() ==
           heartstead::core::ProcessId::from_value(6));
    assert(imported.value().stats().chunk_count == 1);
    assert(imported.value().chunks().edit_log().size() == 1);
    assert(imported.value().chunks().stats().dirty_save_count == 0);
    assert(imported.value().chunks().stats().dirty_replication_count == 0);
    assert(imported.value().stats().build_object_count == 1);
    assert(imported.value().stats().cargo_count == 1);
    const auto* imported_cargo = imported.value().cargo().find(cargo_id.value());
    assert(imported_cargo != nullptr);
    assert(imported_cargo->position.approximate_global().x == -2.0);
    assert(imported_cargo->position.approximate_global().z == 9.0);
    assert(imported.value().stats().entity_count == 1);
    assert(imported.value().stats().inventory_count == 1);
    assert(imported.value().stats().workpiece_count == 1);
    assert(imported.value().stats().assembly_count == 1);
    assert(imported.value().stats().process_count == 1);
    assert(imported.value().stats().mod_state_count == 1);

    auto restored_voxel = imported.value().chunks().get({2, 0, 0}, {1, 1, 1});
    assert(restored_voxel);
    assert(restored_voxel.value().type == 8);
    assert(restored_voxel.value().light == 2);

    const auto* restored_entity = imported.value().entities().find_by_save_id(entity_id.value());
    assert(restored_entity != nullptr);
    assert(restored_entity->runtime_handle == heartstead::core::RuntimeHandle::from_value(500));
    assert(restored_entity->net_id == heartstead::core::NetId::from_value(9000));
    assert(restored_entity->sleeping);
    assert(restored_entity->transform.position.approximate_global().x == 6.0);
    assert(restored_entity->transform.rotation_degrees.y == 270.0);
    assert(imported.value().entity_net_ids().peek_next() ==
           heartstead::core::NetId::from_value(9001));

    const auto* restored_workpiece =
        imported.value().workpieces().find(heartstead::core::WorkpieceId::from_value(42));
    assert(restored_workpiece != nullptr);
    assert(restored_workpiece->grid.occupied_count() == 1);
    assert(!restored_workpiece->owner_session.is_valid());

    auto reexported = heartstead::world::WorldSnapshotBridge::export_snapshot(imported.value());
    assert(reexported);
    assert(reexported.value().chunk_edits.size() == 1);
    assert(reexported.value().entities.size() == 1);
    assert(reexported.value().entities.front().transform.position.approximate_global().x == 6.0);

    heartstead::modding::PrototypeRegistry empty_registry;
    assert(!empty_registry.build({}).has_errors());
    auto missing_prototype_import =
        heartstead::world::WorldSnapshotBridge::import_validated_snapshot(
            snapshot.value(), empty_registry, load_config);
    assert(missing_prototype_import);
    assert(!missing_prototype_import.value().missing_prototypes().empty());

    auto wrong_item_kind = snapshot.value();
    wrong_item_kind.inventories.front().stacks.front().prototype_id = cargo_prototype.value();
    auto wrong_item_kind_import = heartstead::world::WorldSnapshotBridge::import_validated_snapshot(
        wrong_item_kind, bridge_registry, load_config);
    assert(!wrong_item_kind_import);
    assert(wrong_item_kind_import.error().code == "prototype_registry.kind_mismatch");

    auto duplicate = snapshot.value();
    duplicate.cargo_records.front().cargo_id = wall_id.value();
    assert(!heartstead::world::WorldSnapshotBridge::import_snapshot(duplicate, load_config));

    auto duplicate_workpiece = snapshot.value();
    duplicate_workpiece.workpieces.front().workpiece_id =
        heartstead::core::WorkpieceId::from_value(wall_id.value().value());
    const auto duplicate_workpiece_validation =
        heartstead::save::SaveSnapshotValidator::validate(duplicate_workpiece, bridge_registry);
    assert(!duplicate_workpiece_validation.valid());
    assert(std::ranges::any_of(duplicate_workpiece_validation.issues, [](const auto& issue) {
        return issue.code == "save_snapshot.duplicate_save_id";
    }));
    auto duplicate_workpiece_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(duplicate_workpiece, load_config);
    assert(!duplicate_workpiece_import);
    assert(duplicate_workpiece_import.error().code == "world_snapshot.duplicate_save_id");

    auto invalid_cargo_hazard = snapshot.value();
    invalid_cargo_hazard.cargo_records.front().hazard_tags = {"bad tag"};
    auto invalid_cargo_hazard_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(invalid_cargo_hazard, load_config);
    assert(!invalid_cargo_hazard_import);
    assert(invalid_cargo_hazard_import.error().code == "cargo.invalid_hazard_tag");

    auto invalid_metadata = snapshot.value();
    invalid_metadata.metadata.game_version.clear();
    auto invalid_metadata_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(invalid_metadata, load_config);
    assert(!invalid_metadata_import);
    assert(invalid_metadata_import.error().code == "save.missing_game_version");

    auto missing_inventory_owner = snapshot.value();
    missing_inventory_owner.inventories.front().owner_id =
        heartstead::core::SaveId::from_value(9999);
    auto missing_inventory_owner_import = heartstead::world::WorldSnapshotBridge::import_snapshot(
        missing_inventory_owner, load_config);
    assert(!missing_inventory_owner_import);
    assert(missing_inventory_owner_import.error().code == "world_snapshot.missing_owner");

    auto empty_inventory_stack = snapshot.value();
    empty_inventory_stack.inventories.front().stacks.front().count = 0;
    auto empty_inventory_stack_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(empty_inventory_stack, load_config);
    assert(!empty_inventory_stack_import);
    assert(empty_inventory_stack_import.error().code ==
           "world_state.invalid_inventory_stack_count");

    auto invalid_inventory_stack = snapshot.value();
    invalid_inventory_stack.inventories.front().stacks.front().count =
        invalid_inventory_stack.inventories.front().stacks.front().max_count + 1;
    auto invalid_inventory_stack_import = heartstead::world::WorldSnapshotBridge::import_snapshot(
        invalid_inventory_stack, load_config);
    assert(!invalid_inventory_stack_import);
    assert(invalid_inventory_stack_import.error().code ==
           "world_state.invalid_inventory_stack_count");

    auto missing_process_owner = snapshot.value();
    missing_process_owner.processes.front().owner_id = heartstead::core::SaveId::from_value(9999);
    auto missing_process_owner_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(missing_process_owner, load_config);
    assert(!missing_process_owner_import);
    assert(missing_process_owner_import.error().code == "world_snapshot.missing_owner");

    auto empty_process_slot = snapshot.value();
    empty_process_slot.processes.front().input_slots.push_back({item_prototype.value(), 0});
    auto empty_process_slot_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(empty_process_slot, load_config);
    assert(!empty_process_slot_import);
    assert(empty_process_slot_import.error().code == "process_slot.invalid_count");

    auto invalid_process_state = snapshot.value();
    invalid_process_state.processes.front().state =
        static_cast<heartstead::processes::ProcessState>(99);
    auto invalid_process_state_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(invalid_process_state, load_config);
    assert(!invalid_process_state_import);
    assert(invalid_process_state_import.error().code == "process.invalid_state");

    auto invalid_complete_process = snapshot.value();
    invalid_complete_process.processes.front().state =
        heartstead::processes::ProcessState::complete;
    auto invalid_complete_process_import = heartstead::world::WorldSnapshotBridge::import_snapshot(
        invalid_complete_process, load_config);
    assert(!invalid_complete_process_import);
    assert(invalid_complete_process_import.error().code == "process.invalid_complete_work");

    auto missing_assembly_root = snapshot.value();
    missing_assembly_root.assemblies.front().root_build_piece_id =
        heartstead::core::SaveId::from_value(9999);
    auto missing_assembly_root_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(missing_assembly_root, load_config);
    assert(!missing_assembly_root_import);
    assert(missing_assembly_root_import.error().code == "world_snapshot.missing_assembly_root");

    auto missing_assembly_part = snapshot.value();
    missing_assembly_part.assemblies.front().parts.push_back(
        {"missing_wall", heartstead::core::SaveId::from_value(9999), wall_prototype.value()});
    auto missing_assembly_part_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(missing_assembly_part, load_config);
    assert(!missing_assembly_part_import);
    assert(missing_assembly_part_import.error().code == "world_snapshot.missing_assembly_part");

    auto missing_assembly_port_source = snapshot.value();
    missing_assembly_port_source.assemblies.front().ports.push_back(
        {"fuel_input", heartstead::networks::NetworkKind::logistics,
         heartstead::core::SaveId::from_value(9999), 1});
    auto missing_assembly_port_source_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(missing_assembly_port_source,
                                                                load_config);
    assert(!missing_assembly_port_source_import);
    assert(missing_assembly_port_source_import.error().code ==
           "world_snapshot.missing_assembly_port_source");

    auto duplicate_assembly_part = snapshot.value();
    duplicate_assembly_part.assemblies.front().parts.push_back(
        {"duplicate", wall_id.value(), wall_prototype.value()});
    duplicate_assembly_part.assemblies.front().parts.push_back(
        {"duplicate", wall_id.value(), wall_prototype.value()});
    auto duplicate_assembly_part_import = heartstead::world::WorldSnapshotBridge::import_snapshot(
        duplicate_assembly_part, load_config);
    assert(!duplicate_assembly_part_import);
    assert(duplicate_assembly_part_import.error().code == "assembly.duplicate_part");

    auto duplicate_assembly_port = snapshot.value();
    duplicate_assembly_port.assemblies.front().ports.push_back(
        {"fuel_input", heartstead::networks::NetworkKind::logistics, wall_id.value(), 1});
    duplicate_assembly_port.assemblies.front().ports.push_back(
        {"fuel_input", heartstead::networks::NetworkKind::power, wall_id.value(), 1});
    auto duplicate_assembly_port_import = heartstead::world::WorldSnapshotBridge::import_snapshot(
        duplicate_assembly_port, load_config);
    assert(!duplicate_assembly_port_import);
    assert(duplicate_assembly_port_import.error().code == "assembly.duplicate_port");

    auto duplicate_chunk = snapshot.value();
    duplicate_chunk.chunk_edits.push_back(snapshot.value().chunk_edits.front());
    auto duplicate_chunk_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(duplicate_chunk, load_config);
    assert(!duplicate_chunk_import);
    assert(duplicate_chunk_import.error().code == "world_snapshot.duplicate_chunk_edit");

    auto invalid_build_piece = snapshot.value();
    invalid_build_piece.build_pieces.front().material_tags.push_back("bad tag");
    auto invalid_build_piece_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(invalid_build_piece, load_config);
    assert(!invalid_build_piece_import);
    assert(invalid_build_piece_import.error().code == "build_piece.invalid_material_tag");

    auto invalid_entity_kind = snapshot.value();
    invalid_entity_kind.entities.front().kind = static_cast<heartstead::entities::EntityKind>(99);
    auto invalid_entity_kind_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(invalid_entity_kind, load_config);
    assert(!invalid_entity_kind_import);
    assert(invalid_entity_kind_import.error().code == "entity.invalid_kind");

    auto duplicate_mod_state = snapshot.value();
    duplicate_mod_state.mod_states.push_back(snapshot.value().mod_states.front());
    auto duplicate_mod_state_import =
        heartstead::world::WorldSnapshotBridge::import_snapshot(duplicate_mod_state, load_config);
    assert(!duplicate_mod_state_import);
    assert(duplicate_mod_state_import.error().code == "world_state.duplicate_mod_state");
}

} // namespace

int main() {
    test_prototype_ids();
    test_stable_hash64();
    test_flat_manifest_parser();
    test_virtual_file_system();
    test_math_primitives();
    test_resource_pack_discovery_and_asset_catalog();
    test_filtered_model_dependency_cooking();
    test_terrain_material_asset_loading();
    test_voxel_surface_state_encoding();
    test_headless_platform();
    test_renderer_rhi();
    test_physics_world();
    test_physical_resource_lifecycle();
    test_scripting_runtime();
    test_script_module_loading_from_mod_lifecycle();
    test_job_system();
    test_mod_discovery_and_prototypes();
    test_mod_dependency_discovery();
    test_mod_lifecycle_plan();
    test_mod_prototype_fingerprints();
    test_mod_validation_applies_prototype_patches();
    test_mod_validation_report();
    test_prototype_registry();
    test_voxel_palette();
    test_world_voxel_chunk();
    test_chunk_database();
    test_chunk_database_records();
    test_dirty_region_tracker();
    test_workpiece_grid();
    test_workpiece_prototype_materialization();
    test_workpiece_template_and_codec();
    test_save_metadata_and_ids();
    test_save_compatibility_checker();
    test_save_text_codec();
    test_save_migration_registry();
    test_save_snapshot_validation();
    test_file_save_database();
    test_file_save_database_safety();
    test_file_save_database_migration();
    test_file_save_slot_catalog();
    test_debug_inspection();
    test_process_prototype_materialization();
    test_room_descriptor_prototype_materialization();
    test_process_runtime();
    test_process_environment_resolver();
    test_simulation_lod_planner();
    test_world_simulation_subject_derivation();
    test_world_replication_delta_planning();
    test_spatial_network();
    test_spatial_network_derivation();
    test_room_graph_descriptors();
    test_room_extraction();
    test_world_operation_transaction();
    test_server_command_dispatcher();
    test_command_payload_codec();
    test_world_command_registry();
    test_network_transport();
    test_host_session();
    test_command_replay_codec_and_runner();
    test_item_prototype_materialization();
    test_item_stacks();
    test_cargo_prototype_materialization();
    test_cargo_records();
    test_entity_prototype_materialization();
    test_scenario_prototype_materialization();
    test_entity_identity();
    test_build_piece_record();
    test_build_piece_prototype_materialization();
    test_assembly_prototype_materialization();
    test_assembly_validation();
    test_region_graph();
    test_terrain_generation();
    test_chunk_streamer();
    test_chunk_meshing();
    test_world_state_databases();
    test_world_snapshot_bridge();
    return 0;
}
