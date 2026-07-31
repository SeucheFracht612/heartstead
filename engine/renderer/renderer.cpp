#include "engine/renderer/renderer.hpp"

#include "engine/core/hash.hpp"
#include "engine/renderer/terrain/gpu_chunk_vertex.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <map>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>

namespace heartstead::renderer {

namespace {

const ChunkRenderStats empty_chunk_stats{};
constexpr std::uint32_t terrain_material_tile_size = 128;

[[nodiscard]] ShaderProgramDesc
make_sky_shader_program(std::span<const std::uint32_t> vertex_spirv,
                        std::span<const std::uint32_t> fragment_spirv) {
    ShaderProgramDesc shader_program;
    shader_program.id = "sky_gradient";
    shader_program.stages = {
        {rhi::RenderShaderStage::vertex,
         "main",
         {vertex_spirv.begin(), vertex_spirv.end()},
         "sky.vert.spv"},
        {rhi::RenderShaderStage::fragment,
         "main",
         {fragment_spirv.begin(), fragment_spirv.end()},
         "sky.frag.spv"},
    };
    shader_program.interface.vertex_stride = sizeof(GpuSkyVertex);
    for (const auto& attribute : gpu_sky_vertex_attributes) {
        shader_program.interface.vertex_inputs.push_back({attribute.location, attribute.format});
    }
    shader_program.interface.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    shader_program.dependencies = {"gpu_sky_vertex_v1", "chunk_push_constants_v2"};
    return shader_program;
}

[[nodiscard]] std::vector<std::byte> make_terrain_tile(std::array<std::uint8_t, 3> color,
                                                       bool error = false) {
    std::vector<std::byte> pixels(terrain_material_tile_size * terrain_material_tile_size * 4U);
    for (std::uint32_t y = 0; y < terrain_material_tile_size; ++y) {
        for (std::uint32_t x = 0; x < terrain_material_tile_size; ++x) {
            const auto offset = static_cast<std::size_t>(y * terrain_material_tile_size + x) * 4U;
            const bool alternate = ((x / 16U) + (y / 16U)) % 2U != 0;
            auto noise = x * 0x9e3779b9U ^ y * 0x85ebca6bU;
            noise ^= noise >> 16U;
            if (error) {
                pixels[offset] = static_cast<std::byte>(alternate ? 20U : 255U);
                pixels[offset + 1] = static_cast<std::byte>(0U);
                pixels[offset + 2] = static_cast<std::byte>(alternate ? 20U : 255U);
            } else {
                const auto scale =
                    std::clamp(0.90F + static_cast<float>(noise & 31U) / 310.0F, 0.90F, 1.0F);
                for (std::size_t channel = 0; channel < color.size(); ++channel) {
                    pixels[offset + channel] = static_cast<std::byte>(
                        static_cast<std::uint8_t>(static_cast<float>(color[channel]) * scale));
                }
            }
            pixels[offset + 3] = static_cast<std::byte>(255U);
        }
    }
    return pixels;
}

[[nodiscard]] std::vector<std::byte> make_flat_terrain_normal_tile() {
    std::vector<std::byte> pixels(terrain_material_tile_size * terrain_material_tile_size * 4U);
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4U) {
        pixels[offset] = static_cast<std::byte>(128U);
        pixels[offset + 1U] = static_cast<std::byte>(128U);
        pixels[offset + 2U] = static_cast<std::byte>(255U);
        pixels[offset + 3U] = static_cast<std::byte>(255U);
    }
    return pixels;
}

[[nodiscard]] std::vector<std::byte>
derive_terrain_normal_map(std::span<const std::byte> base_color) {
    const auto side = terrain_material_tile_size;
    std::vector<std::byte> result(base_color.size());
    const auto luminance = [&base_color](std::uint32_t x, std::uint32_t y) {
        x %= side;
        y %= side;
        const auto offset = static_cast<std::size_t>(y * side + x) * 4U;
        const auto red = static_cast<float>(std::to_integer<std::uint8_t>(base_color[offset]));
        const auto green =
            static_cast<float>(std::to_integer<std::uint8_t>(base_color[offset + 1U]));
        const auto blue =
            static_cast<float>(std::to_integer<std::uint8_t>(base_color[offset + 2U]));
        return (red * 0.2126F + green * 0.7152F + blue * 0.0722F) / 255.0F;
    };
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            const auto left = luminance((x + side - 1U) % side, y);
            const auto right = luminance((x + 1U) % side, y);
            const auto down = luminance(x, (y + side - 1U) % side);
            const auto up = luminance(x, (y + 1U) % side);
            auto normal = math::Vec3f{(left - right) * 2.0F, (down - up) * 2.0F, 1.0F};
            normal = normal / static_cast<float>(math::length(normal));
            const auto offset = static_cast<std::size_t>(y * side + x) * 4U;
            result[offset] = static_cast<std::byte>(
                static_cast<std::uint8_t>(std::lround((normal.x * 0.5F + 0.5F) * 255.0F)));
            result[offset + 1U] = static_cast<std::byte>(
                static_cast<std::uint8_t>(std::lround((normal.y * 0.5F + 0.5F) * 255.0F)));
            result[offset + 2U] = static_cast<std::byte>(
                static_cast<std::uint8_t>(std::lround((normal.z * 0.5F + 0.5F) * 255.0F)));
            result[offset + 3U] = static_cast<std::byte>(255U);
        }
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> make_terrain_surface_tile() {
    std::vector<std::byte> pixels(terrain_material_tile_size * terrain_material_tile_size * 4U);
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4U) {
        pixels[offset] = static_cast<std::byte>(255U);
        pixels[offset + 1U] = static_cast<std::byte>(255U);
        pixels[offset + 2U] = static_cast<std::byte>(255U);
        pixels[offset + 3U] = static_cast<std::byte>(128U);
    }
    return pixels;
}

void apply_default_terrain_surface_layers(MaterialRuntimeDesc& material) noexcept {
    const auto set = [&material](std::size_t index, std::array<float, 4> tint, float roughness,
                                 float emissive = 0.0F) {
        auto& layer = material.terrain_surface_layers[index];
        layer.tint = tint;
        layer.strength = 1.0F;
        layer.roughness = roughness;
        layer.emissive_strength = emissive;
    };
    set(0, {0.48F, 0.54F, 0.60F, 0.72F}, 0.12F);
    set(1, {0.92F, 0.96F, 1.0F, 0.95F}, 0.78F);
    set(2, {0.76F, 0.90F, 1.0F, 0.72F}, 0.58F);
    set(3, {0.24F, 0.14F, 0.075F, 0.76F}, 0.86F);
    set(4, {0.20F, 0.38F, 0.12F, 0.68F}, 0.92F);
    set(5, {0.055F, 0.045F, 0.04F, 0.86F}, 0.96F);
    set(6, {1.0F, 0.18F, 0.025F, 0.58F}, 0.42F, 2.5F);
    set(7, {0.30F, 0.035F, 0.45F, 0.72F}, 0.48F, 0.35F);
    set(8, {0.03F, 0.68F, 0.92F, 0.64F}, 0.32F, 1.2F);
}

[[nodiscard]] ShaderProgramDesc
make_terrain_shader_program(std::span<const std::uint32_t> vertex_spirv,
                            std::span<const std::uint32_t> fragment_spirv) {
    ShaderProgramDesc shader_program;
    shader_program.id = "terrain";
    shader_program.stages = {
        {rhi::RenderShaderStage::vertex,
         "main",
         {vertex_spirv.begin(), vertex_spirv.end()},
         "terrain.vert.spv"},
        {rhi::RenderShaderStage::fragment,
         "main",
         {fragment_spirv.begin(), fragment_spirv.end()},
         "terrain.frag.spv"},
    };
    shader_program.interface.vertex_stride = sizeof(terrain::GpuChunkVertex);
    for (const auto& attribute : terrain::gpu_chunk_vertex_attributes) {
        shader_program.interface.vertex_inputs.push_back({attribute.location, attribute.format});
    }
    shader_program.interface.descriptors = {
        {"terrain_textures", rhi::RenderDescriptorKind::sampled_texture, 0, true,
         rhi::RenderShaderStageFlags::fragment},
        {"voxel_materials", rhi::RenderDescriptorKind::storage_buffer, 1, true,
         rhi::RenderShaderStageFlags::fragment},
        {"local_lights", rhi::RenderDescriptorKind::storage_buffer, 2, true,
         rhi::RenderShaderStageFlags::fragment},
        {"light_grid", rhi::RenderDescriptorKind::storage_buffer, 3, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_data", rhi::RenderDescriptorKind::storage_buffer, 4, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_0", rhi::RenderDescriptorKind::sampled_texture, 5, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_1", rhi::RenderDescriptorKind::sampled_texture, 6, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_2", rhi::RenderDescriptorKind::sampled_texture, 7, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_3", rhi::RenderDescriptorKind::sampled_texture, 8, true,
         rhi::RenderShaderStageFlags::fragment},
        {"environment_map", rhi::RenderDescriptorKind::sampled_texture, 9, true,
         rhi::RenderShaderStageFlags::fragment},
        {"local_shadow_0", rhi::RenderDescriptorKind::sampled_texture, 10, true,
         rhi::RenderShaderStageFlags::fragment},
        {"local_shadow_1", rhi::RenderDescriptorKind::sampled_texture, 11, true,
         rhi::RenderShaderStageFlags::fragment},
        {"terrain_normal_textures", rhi::RenderDescriptorKind::sampled_texture, 12, true,
         rhi::RenderShaderStageFlags::fragment},
        {"terrain_surface_textures", rhi::RenderDescriptorKind::sampled_texture, 13, true,
         rhi::RenderShaderStageFlags::fragment},
        // Supplied by the transparent pass so every material in that mixed pass shares a graph
        // binding shape. Terrain performs fixed-function depth testing; particle meshes consume
        // the copied depth through their dedicated transparent layout.
        {"scene_depth", rhi::RenderDescriptorKind::sampled_texture, 14, false,
         rhi::RenderShaderStageFlags::fragment},
    };
    shader_program.interface.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    shader_program.dependencies = {"gpu_chunk_vertex_v1", "gpu_voxel_material_v3",
                                   "chunk_push_constants_v2"};
    return shader_program;
}

[[nodiscard]] ShaderProgramDesc
make_static_mesh_shader_program(std::span<const std::uint32_t> vertex_spirv,
                                std::span<const std::uint32_t> fragment_spirv) {
    ShaderProgramDesc shader_program;
    shader_program.id = "static_mesh";
    shader_program.stages = {
        {rhi::RenderShaderStage::vertex,
         "main",
         {vertex_spirv.begin(), vertex_spirv.end()},
         "static_mesh.vert.spv"},
        {rhi::RenderShaderStage::fragment,
         "main",
         {fragment_spirv.begin(), fragment_spirv.end()},
         "static_mesh.frag.spv"},
    };
    shader_program.interface.vertex_stride = sizeof(GpuStaticMeshVertex);
    for (const auto& attribute : gpu_static_mesh_vertex_attributes) {
        shader_program.interface.vertex_inputs.push_back({attribute.location, attribute.format});
    }
    shader_program.interface.descriptors = {
        {"object_instances", rhi::RenderDescriptorKind::storage_buffer, 0, true,
         rhi::RenderShaderStageFlags::vertex},
        {"skin_matrices", rhi::RenderDescriptorKind::storage_buffer, 1, true,
         rhi::RenderShaderStageFlags::vertex},
        {"surface_textures", rhi::RenderDescriptorKind::sampled_texture, 2, true,
         rhi::RenderShaderStageFlags::fragment},
        {"surface_materials", rhi::RenderDescriptorKind::storage_buffer, 3, true,
         rhi::RenderShaderStageFlags::fragment},
        {"surface_data_textures", rhi::RenderDescriptorKind::sampled_texture, 4, true,
         rhi::RenderShaderStageFlags::fragment},
        {"morph_deltas", rhi::RenderDescriptorKind::storage_buffer, 5, true,
         rhi::RenderShaderStageFlags::vertex},
        {"morph_weights", rhi::RenderDescriptorKind::storage_buffer, 6, true,
         rhi::RenderShaderStageFlags::vertex},
        {"local_lights", rhi::RenderDescriptorKind::storage_buffer, 7, true,
         rhi::RenderShaderStageFlags::fragment},
        {"light_grid", rhi::RenderDescriptorKind::storage_buffer, 8, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_data", rhi::RenderDescriptorKind::storage_buffer, 9, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_0", rhi::RenderDescriptorKind::sampled_texture, 10, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_1", rhi::RenderDescriptorKind::sampled_texture, 11, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_2", rhi::RenderDescriptorKind::sampled_texture, 12, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_3", rhi::RenderDescriptorKind::sampled_texture, 13, true,
         rhi::RenderShaderStageFlags::fragment},
        {"environment_map", rhi::RenderDescriptorKind::sampled_texture, 14, true,
         rhi::RenderShaderStageFlags::fragment},
        {"local_shadow_0", rhi::RenderDescriptorKind::sampled_texture, 15, true,
         rhi::RenderShaderStageFlags::fragment},
        {"local_shadow_1", rhi::RenderDescriptorKind::sampled_texture, 16, true,
         rhi::RenderShaderStageFlags::fragment},
        {"scene_depth", rhi::RenderDescriptorKind::sampled_texture, 17, true,
         rhi::RenderShaderStageFlags::fragment},
    };
    shader_program.interface.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    shader_program.dependencies = {"gpu_static_mesh_vertex_v3", "gpu_object_instance_v4",
                                   "gpu_surface_material_v2", "gpu_morph_delta_v1",
                                   "chunk_push_constants_v2"};
    return shader_program;
}

[[nodiscard]] ShaderProgramDesc
make_terrain_shadow_shader_program(std::span<const std::uint32_t> vertex_spirv,
                                   std::span<const std::uint32_t> fragment_spirv) {
    auto program = make_terrain_shader_program(vertex_spirv, fragment_spirv);
    program.id = "terrain_shadow";
    program.stages[1].source_name = "shadow_terrain.frag.spv";
    program.interface.descriptors.resize(2);
    program.dependencies = {"gpu_chunk_vertex_v1", "gpu_voxel_material_v3",
                            "chunk_push_constants_v2", "depth_only_v1"};
    return program;
}

[[nodiscard]] ShaderProgramDesc
make_static_shadow_shader_program(std::span<const std::uint32_t> vertex_spirv,
                                  std::span<const std::uint32_t> fragment_spirv) {
    auto program = make_static_mesh_shader_program(vertex_spirv, fragment_spirv);
    program.id = "static_shadow";
    program.stages[1].source_name = "shadow_static.frag.spv";
    program.interface.descriptors.resize(7);
    program.dependencies = {"gpu_static_mesh_vertex_v3", "gpu_object_instance_v4",
                            "gpu_surface_material_v2",   "gpu_morph_delta_v1",
                            "chunk_push_constants_v2",   "depth_only_v1"};
    return program;
}

[[nodiscard]] ShaderProgramDesc
make_post_shader_program(std::string id, std::span<const std::uint32_t> vertex_spirv,
                         std::span<const std::uint32_t> fragment_spirv,
                         std::vector<rhi::RenderDescriptorBinding> descriptors) {
    ShaderProgramDesc program;
    program.id = std::move(id);
    program.stages = {
        {rhi::RenderShaderStage::vertex,
         "main",
         {vertex_spirv.begin(), vertex_spirv.end()},
         "tone_map.vert.spv"},
        {rhi::RenderShaderStage::fragment,
         "main",
         {fragment_spirv.begin(), fragment_spirv.end()},
         program.id + ".frag.spv"},
    };
    program.interface.descriptors.reserve(descriptors.size());
    for (auto& descriptor : descriptors) {
        program.interface.descriptors.push_back({std::move(descriptor.name), descriptor.kind,
                                                 descriptor.slot, descriptor.required,
                                                 descriptor.stages});
    }
    program.dependencies = {"fullscreen_triangle_v1"};
    return program;
}

[[nodiscard]] ShaderProgramDesc
make_debug_shader_program(std::span<const std::uint32_t> vertex_spirv,
                          std::span<const std::uint32_t> fragment_spirv) {
    ShaderProgramDesc shader_program;
    shader_program.id = "debug_lines";
    shader_program.stages = {
        {rhi::RenderShaderStage::vertex,
         "main",
         {vertex_spirv.begin(), vertex_spirv.end()},
         "debug_line.vert.spv"},
        {rhi::RenderShaderStage::fragment,
         "main",
         {fragment_spirv.begin(), fragment_spirv.end()},
         "debug_line.frag.spv"},
    };
    shader_program.interface.vertex_stride = sizeof(GpuDebugVertex);
    for (const auto& attribute : gpu_debug_vertex_attributes) {
        shader_program.interface.vertex_inputs.push_back({attribute.location, attribute.format});
    }
    shader_program.interface.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    shader_program.dependencies = {"gpu_debug_vertex_v1", "chunk_push_constants_v2"};
    return shader_program;
}

[[nodiscard]] ShaderProgramDesc
make_ui_shader_program(std::span<const std::uint32_t> vertex_spirv,
                       std::span<const std::uint32_t> fragment_spirv) {
    ShaderProgramDesc shader_program;
    shader_program.id = "ui";
    shader_program.stages = {
        {rhi::RenderShaderStage::vertex,
         "main",
         {vertex_spirv.begin(), vertex_spirv.end()},
         "ui.vert.spv"},
        {rhi::RenderShaderStage::fragment,
         "main",
         {fragment_spirv.begin(), fragment_spirv.end()},
         "ui.frag.spv"},
    };
    shader_program.interface.vertex_stride = sizeof(GpuUiVertex);
    for (const auto& attribute : gpu_ui_vertex_attributes) {
        shader_program.interface.vertex_inputs.push_back({attribute.location, attribute.format});
    }
    shader_program.interface.descriptors = {
        {"ui_atlas", rhi::RenderDescriptorKind::sampled_texture, 0, true,
         rhi::RenderShaderStageFlags::fragment},
    };
    shader_program.interface.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    shader_program.dependencies = {"gpu_ui_vertex_v1", "ui_atlas_v1"};
    return shader_program;
}

[[nodiscard]] ShaderProgramDesc
make_tone_map_shader_program(std::span<const std::uint32_t> vertex_spirv,
                             std::span<const std::uint32_t> fragment_spirv) {
    ShaderProgramDesc shader_program;
    shader_program.id = "tone_map";
    shader_program.stages = {
        {rhi::RenderShaderStage::vertex,
         "main",
         {vertex_spirv.begin(), vertex_spirv.end()},
         "tone_map.vert.spv"},
        {rhi::RenderShaderStage::fragment,
         "main",
         {fragment_spirv.begin(), fragment_spirv.end()},
         "tone_map.frag.spv"},
    };
    // No vertex inputs at all: the fullscreen triangle comes from gl_VertexIndex.
    shader_program.interface.vertex_stride = 0;
    shader_program.interface.descriptors = {
        {"scene_hdr", rhi::RenderDescriptorKind::sampled_texture, 0, true,
         rhi::RenderShaderStageFlags::fragment},
        {"bloom_hdr", rhi::RenderDescriptorKind::sampled_texture, 1, true,
         rhi::RenderShaderStageFlags::fragment},
    };
    shader_program.interface.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::fragment, 0, sizeof(rhi::ToneMapPushConstants)});
    shader_program.dependencies = {"scene_hdr_v1"};
    return shader_program;
}

[[nodiscard]] std::array<std::uint8_t, 7> fallback_glyph_rows(unsigned char character) {
    if (character >= 'a' && character <= 'z') {
        character = static_cast<unsigned char>(character - 'a' + 'A');
    }
    switch (character) {
    case '0':
        return {14, 17, 19, 21, 25, 17, 14};
    case '1':
        return {4, 12, 4, 4, 4, 4, 14};
    case '2':
        return {14, 17, 1, 2, 4, 8, 31};
    case '3':
        return {30, 1, 1, 14, 1, 1, 30};
    case '4':
        return {2, 6, 10, 18, 31, 2, 2};
    case '5':
        return {31, 16, 16, 30, 1, 1, 30};
    case '6':
        return {14, 16, 16, 30, 17, 17, 14};
    case '7':
        return {31, 1, 2, 4, 8, 8, 8};
    case '8':
        return {14, 17, 17, 14, 17, 17, 14};
    case '9':
        return {14, 17, 17, 15, 1, 1, 14};
    case 'A':
        return {14, 17, 17, 31, 17, 17, 17};
    case 'B':
        return {30, 17, 17, 30, 17, 17, 30};
    case 'C':
        return {15, 16, 16, 16, 16, 16, 15};
    case 'D':
        return {30, 17, 17, 17, 17, 17, 30};
    case 'E':
        return {31, 16, 16, 30, 16, 16, 31};
    case 'F':
        return {31, 16, 16, 30, 16, 16, 16};
    case 'G':
        return {15, 16, 16, 23, 17, 17, 14};
    case 'H':
        return {17, 17, 17, 31, 17, 17, 17};
    case 'I':
        return {14, 4, 4, 4, 4, 4, 14};
    case 'J':
        return {7, 2, 2, 2, 18, 18, 12};
    case 'K':
        return {17, 18, 20, 24, 20, 18, 17};
    case 'L':
        return {16, 16, 16, 16, 16, 16, 31};
    case 'M':
        return {17, 27, 21, 21, 17, 17, 17};
    case 'N':
        return {17, 25, 21, 19, 17, 17, 17};
    case 'O':
        return {14, 17, 17, 17, 17, 17, 14};
    case 'P':
        return {30, 17, 17, 30, 16, 16, 16};
    case 'Q':
        return {14, 17, 17, 17, 21, 18, 13};
    case 'R':
        return {30, 17, 17, 30, 20, 18, 17};
    case 'S':
        return {15, 16, 16, 14, 1, 1, 30};
    case 'T':
        return {31, 4, 4, 4, 4, 4, 4};
    case 'U':
        return {17, 17, 17, 17, 17, 17, 14};
    case 'V':
        return {17, 17, 17, 17, 10, 10, 4};
    case 'W':
        return {17, 17, 17, 21, 21, 27, 17};
    case 'X':
        return {17, 17, 10, 4, 10, 17, 17};
    case 'Y':
        return {17, 17, 10, 4, 4, 4, 4};
    case 'Z':
        return {31, 1, 2, 4, 8, 16, 31};
    case '-':
        return {0, 0, 0, 31, 0, 0, 0};
    case '_':
        return {0, 0, 0, 0, 0, 0, 31};
    case '.':
        return {0, 0, 0, 0, 0, 12, 12};
    case ':':
        return {0, 12, 12, 0, 12, 12, 0};
    case '/':
        return {1, 2, 2, 4, 8, 8, 16};
    case '?':
        return {14, 17, 1, 2, 4, 0, 4};
    default:
        return {};
    }
}

[[nodiscard]] std::vector<std::byte> make_ui_atlas() {
    constexpr std::size_t width = 128;
    constexpr std::size_t height = 64;
    constexpr std::size_t layer_size = width * height * 4U;
    std::vector<std::byte> pixels(layer_size * 2U, std::byte{0});
    std::fill_n(pixels.begin(), static_cast<std::ptrdiff_t>(layer_size), std::byte{0xff});
    for (std::uint32_t character = 0; character < 128U; ++character) {
        const auto rows = fallback_glyph_rows(static_cast<unsigned char>(character));
        const auto cell_x = (character % 16U) * 8U;
        const auto cell_y = (character / 16U) * 8U;
        for (std::uint32_t row = 0; row < rows.size(); ++row) {
            for (std::uint32_t column = 0; column < 5U; ++column) {
                if ((rows[row] & (1U << (4U - column))) == 0U) {
                    continue;
                }
                const auto offset =
                    layer_size +
                    (static_cast<std::size_t>(cell_y + row) * width + cell_x + column + 1U) * 4U;
                pixels[offset] = std::byte{0xff};
                pixels[offset + 1U] = std::byte{0xff};
                pixels[offset + 2U] = std::byte{0xff};
                pixels[offset + 3U] = std::byte{0xff};
            }
        }
    }
    return pixels;
}

} // namespace

Renderer::~Renderer() {
    (void)shutdown();
}

core::Status Renderer::initialize(RendererInitDesc desc) {
    if (is_initialized()) {
        return core::Status::failure("renderer.already_initialized",
                                     "renderer cannot be initialized twice");
    }
    if (desc.device == nullptr) {
        return core::Status::failure("renderer.missing_device",
                                     "renderer initialization requires a render device");
    }
    owner_thread_ = std::this_thread::get_id();
    auto config_status = desc.chunk_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = desc.chunk_gpu_cache_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = desc.mesh_manager_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = desc.scene_render_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = desc.clustered_lighting_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = desc.directional_shadow_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = desc.debug_renderer_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = desc.ui_renderer_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = rhi::validate_render_environment(desc.environment);
    if (!config_status) {
        return config_status;
    }

    device_ = std::move(desc.device);
    shader_manager_ = std::make_unique<ShaderManager>(*device_, desc.development_shader_hot_reload);
    sampler_cache_ = std::make_unique<SamplerCache>(*device_);
    texture_manager_ = std::make_unique<TextureManager>(*device_);
    material_cache_ = std::make_unique<MaterialRuntimeCache>(*device_);
    pipeline_cache_ = std::make_unique<PipelineCache>(*device_, *shader_manager_);
    auto fallback_status = texture_manager_->initialize_fallbacks();
    if (!fallback_status) {
        const auto error = fallback_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    surface_texture_array_ = std::make_unique<SurfaceTextureArray>(*texture_manager_);
    fallback_status = surface_texture_array_->initialize();
    if (!fallback_status) {
        const auto error = fallback_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    surface_data_texture_array_ = std::make_unique<SurfaceTextureArray>(*texture_manager_);
    SurfaceTextureArrayConfig data_texture_config;
    data_texture_config.color_space = TextureColorSpace::linear;
    data_texture_config.texture_id = "__surface_data_texture_array";
    fallback_status = surface_data_texture_array_->initialize(std::move(data_texture_config));
    if (!fallback_status) {
        const auto error = fallback_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    const auto fallback_material_id = core::PrototypeId::parse("base:materials/error");
    if (!fallback_material_id) {
        (void)shutdown();
        return core::Status::failure("renderer.invalid_fallback_material_id",
                                     "internal fallback material id is invalid");
    }
    MaterialRuntimeDesc fallback_material;
    fallback_material.id = fallback_material_id.value();
    fallback_material.domain = MaterialRuntimeDomain::surface;
    fallback_material.surface_texture = surface_texture_array_->fallbacks().error;
    auto created_fallback_material = material_cache_->upsert(std::move(fallback_material));
    if (!created_fallback_material) {
        const auto error = created_fallback_material.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    fallback_material_ = created_fallback_material.value();
    auto pipeline_status = create_sky_pipeline(desc.sky_vertex_spirv, desc.sky_fragment_spirv);
    if (!pipeline_status) {
        const auto error = pipeline_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    pipeline_status =
        create_terrain_pipeline(desc.terrain_vertex_spirv, desc.terrain_fragment_spirv,
                                desc.voxel_palette, desc.terrain_material_assets);
    if (!pipeline_status) {
        const auto error = pipeline_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    pipeline_status =
        create_scene_pipelines(desc.static_mesh_vertex_spirv, desc.static_mesh_fragment_spirv);
    if (!pipeline_status) {
        const auto error = pipeline_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    const auto shadow_terrain_fragment =
        desc.shadow_terrain_fragment_spirv.empty()
            ? std::span<const std::uint32_t>{desc.terrain_fragment_spirv}
            : std::span<const std::uint32_t>{desc.shadow_terrain_fragment_spirv};
    const auto shadow_static_fragment =
        desc.shadow_static_fragment_spirv.empty()
            ? std::span<const std::uint32_t>{desc.static_mesh_fragment_spirv}
            : std::span<const std::uint32_t>{desc.shadow_static_fragment_spirv};
    pipeline_status =
        create_shadow_pipelines(desc.terrain_vertex_spirv, shadow_terrain_fragment,
                                desc.static_mesh_vertex_spirv, shadow_static_fragment);
    if (!pipeline_status) {
        const auto error = pipeline_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    pipeline_status = create_debug_pipelines(desc.debug_vertex_spirv, desc.debug_fragment_spirv);
    if (!pipeline_status) {
        const auto error = pipeline_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    pipeline_status = create_ui_pipeline(desc.ui_vertex_spirv, desc.ui_fragment_spirv);
    if (!pipeline_status) {
        const auto error = pipeline_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    if (!desc.tone_map_vertex_spirv.empty() && !desc.tone_map_fragment_spirv.empty()) {
        const auto ssao_fragment =
            desc.ssao_fragment_spirv.empty()
                ? std::span<const std::uint32_t>{desc.tone_map_fragment_spirv}
                : std::span<const std::uint32_t>{desc.ssao_fragment_spirv};
        const auto ao_fragment =
            desc.ao_composite_fragment_spirv.empty()
                ? std::span<const std::uint32_t>{desc.tone_map_fragment_spirv}
                : std::span<const std::uint32_t>{desc.ao_composite_fragment_spirv};
        const auto fxaa_fragment =
            desc.fxaa_fragment_spirv.empty()
                ? std::span<const std::uint32_t>{desc.tone_map_fragment_spirv}
                : std::span<const std::uint32_t>{desc.fxaa_fragment_spirv};
        const auto bloom_fragment =
            desc.bloom_fragment_spirv.empty()
                ? std::span<const std::uint32_t>{desc.tone_map_fragment_spirv}
                : std::span<const std::uint32_t>{desc.bloom_fragment_spirv};
        pipeline_status = create_image_quality_pipelines(
            desc.tone_map_vertex_spirv, ssao_fragment, ao_fragment, fxaa_fragment, bloom_fragment);
        if (!pipeline_status) {
            const auto error = pipeline_status.error();
            (void)shutdown();
            return core::Status::failure(error.code, error.message);
        }
        pipeline_status =
            create_tone_map_pipeline(desc.tone_map_vertex_spirv, desc.tone_map_fragment_spirv);
        if (!pipeline_status) {
            const auto error = pipeline_status.error();
            (void)shutdown();
            return core::Status::failure(error.code, error.message);
        }
    }
    if (!tone_map_pipeline_.is_valid()) {
        (void)shutdown();
        return core::Status::failure(
            "renderer.tone_map_shaders_required",
            "the frame graph resolves the scene target through tone mapping, so tone map shaders "
            "are required");
    }
    pipeline_cache_->seal();

    sky_renderer_ = std::make_unique<SkyRenderer>(*device_, sky_pipeline_);
    auto sky_status = sky_renderer_->initialize();
    if (!sky_status) {
        const auto error = sky_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    mesh_manager_ = std::make_unique<MeshManager>(*device_);
    auto mesh_status = mesh_manager_->initialize(desc.mesh_manager_config);
    if (!mesh_status) {
        const auto error = mesh_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    const auto scene_material = core::PrototypeId::parse("base:materials/static_instances");
    const auto transparent_material =
        core::PrototypeId::parse("base:materials/transparent_instances");
    if (!scene_material || !transparent_material) {
        (void)shutdown();
        return core::Status::failure("renderer.invalid_scene_material",
                                     "internal static-instance material id is invalid");
    }
    scene_render_system_ = std::make_unique<SceneRenderSystem>(
        *device_, *mesh_manager_, *material_cache_, fallback_material_, scene_pipelines_,
        scene_material.value());
    auto scene_status = scene_render_system_->initialize(desc.scene_render_config);
    if (!scene_status) {
        const auto error = scene_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    const std::array transparent_geometry_writes{
        rhi::RenderDescriptorWrite{
            transparent_material.value(), "object_instances",
            scene_render_system_->instance_buffer(), 0,
            static_cast<std::size_t>(scene_render_system_->stats().instance_buffer_bytes)},
        rhi::RenderDescriptorWrite{
            transparent_material.value(), "skin_matrices",
            scene_render_system_->skin_matrix_buffer(), 0,
            static_cast<std::size_t>(scene_render_system_->stats().skin_matrix_buffer_bytes)},
        rhi::RenderDescriptorWrite{
            transparent_material.value(), "morph_deltas", mesh_manager_->morph_delta_buffer(), 0,
            static_cast<std::size_t>(mesh_manager_->stats().morph_arena.capacity_bytes)},
        rhi::RenderDescriptorWrite{
            transparent_material.value(), "morph_weights",
            scene_render_system_->morph_weight_buffer(), 0,
            static_cast<std::size_t>(scene_render_system_->stats().morph_weight_buffer_bytes)},
    };
    auto transparent_geometry = device_->write_descriptors(transparent_geometry_writes);
    if (!transparent_geometry) {
        const auto error = transparent_geometry.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    auto shadow_resource_status = bind_shadow_resources();
    if (!shadow_resource_status) {
        const auto error = shadow_resource_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    const auto terrain_lighting_material =
        core::PrototypeId::parse("base:materials/milestone_terrain");
    if (!terrain_lighting_material) {
        (void)shutdown();
        return core::Status::failure("renderer.invalid_terrain_material",
                                     "internal terrain material prototype id is invalid");
    }
    environment_lighting_ = std::make_unique<EnvironmentLighting>(*device_);
    auto environment_status = environment_lighting_->initialize();
    if (!environment_status) {
        const auto error = environment_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    environment_status =
        environment_lighting_->bind(terrain_lighting_material.value(), "environment_map");
    if (environment_status) {
        environment_status = environment_lighting_->bind(scene_material.value(), "environment_map");
    }
    if (environment_status) {
        environment_status =
            environment_lighting_->bind(transparent_material.value(), "environment_map");
    }
    if (!environment_status) {
        const auto error = environment_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    cascaded_shadows_ = std::make_unique<CascadedShadowSystem>(*device_);
    shadow_resource_status = cascaded_shadows_->initialize(desc.directional_shadow_config);
    if (!shadow_resource_status) {
        const auto error = shadow_resource_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    clustered_lighting_ = std::make_unique<ClusteredLightingSystem>(*device_);
    auto lighting_status =
        clustered_lighting_->initialize(device_->current_extent(), desc.clustered_lighting_config);
    if (!lighting_status) {
        const auto error = lighting_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    lighting_status =
        clustered_lighting_->bind(terrain_lighting_material.value(), "local_lights", "light_grid");
    if (lighting_status) {
        lighting_status =
            clustered_lighting_->bind(scene_material.value(), "local_lights", "light_grid");
    }
    if (lighting_status) {
        lighting_status =
            clustered_lighting_->bind(transparent_material.value(), "local_lights", "light_grid");
    }
    if (!lighting_status) {
        const auto error = lighting_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    shadow_resource_status =
        cascaded_shadows_->bind(terrain_lighting_material.value(), "shadow_data");
    if (shadow_resource_status) {
        shadow_resource_status = cascaded_shadows_->bind(scene_material.value(), "shadow_data");
    }
    if (shadow_resource_status) {
        shadow_resource_status =
            cascaded_shadows_->bind(transparent_material.value(), "shadow_data");
    }
    if (!shadow_resource_status) {
        const auto error = shadow_resource_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    debug_renderer_ = std::make_unique<DebugRenderer>(*device_, debug_pipelines_);
    auto debug_status = debug_renderer_->initialize(desc.debug_renderer_config);
    if (!debug_status) {
        const auto error = debug_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    ui_renderer_ = std::make_unique<UiRenderer>(*device_, ui_pipeline_);
    auto ui_status = ui_renderer_->initialize(device_->current_extent(), desc.ui_renderer_config);
    if (!ui_status) {
        const auto error = ui_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }

    chunk_cache_ = std::make_unique<ChunkGpuCache>(*device_);
    auto cache_status = chunk_cache_->initialize(desc.chunk_gpu_cache_config);
    if (!cache_status) {
        const auto error = cache_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    chunk_system_ = std::make_unique<ChunkRenderSystem>(*chunk_cache_, terrain_pipelines_,
                                                        desc.voxel_palette, desc.chunk_config);
    auto chunk_system_status = chunk_system_->initialize();
    if (!chunk_system_status) {
        const auto error = chunk_system_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    frame_builder_ = std::make_unique<FrameBuilder>(device_->current_extent(), desc.clear_color);
    frame_builder_->set_tone_map_pipeline(tone_map_pipeline_);
    frame_builder_->set_image_quality_pipelines(
        image_quality_pipelines_[0], image_quality_pipelines_[1], image_quality_pipelines_[2],
        image_quality_pipelines_[3]);
    auto shadow_resolution_status =
        frame_builder_->set_shadow_resolution(desc.directional_shadow_config.resolution);
    if (!shadow_resolution_status) {
        const auto error = shadow_resolution_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    auto exposure_status = frame_builder_->set_exposure(desc.exposure);
    if (!exposure_status) {
        const auto error = exposure_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    environment_ = desc.environment;
    return core::Status::ok();
}

core::Status Renderer::shutdown() {
    core::Status first_failure = core::Status::ok();
    const auto remember_failure = [&first_failure](core::Status status) {
        if (!status && first_failure) {
            first_failure = status;
        }
    };
    if (device_ != nullptr) {
        remember_failure(device_->wait_idle());
    }
    chunk_draw_scratch_.clear();
    draw_command_scratch_ = {};
    scene_draw_scratch_ = {};
    debug_frame_scratch_ = {};
    ui_frame_scratch_ = {};
    debug_text_labels_.clear();
    scene_.clear();
    frame_builder_.reset();
    if (clustered_lighting_ != nullptr) {
        remember_failure(clustered_lighting_->shutdown());
        clustered_lighting_.reset();
    }
    if (cascaded_shadows_ != nullptr) {
        remember_failure(cascaded_shadows_->shutdown());
        cascaded_shadows_.reset();
    }
    if (environment_lighting_ != nullptr) {
        remember_failure(environment_lighting_->shutdown());
        environment_lighting_.reset();
    }
    if (sky_renderer_ != nullptr) {
        remember_failure(sky_renderer_->shutdown());
        sky_renderer_.reset();
    }
    if (ui_renderer_ != nullptr) {
        remember_failure(ui_renderer_->shutdown());
        ui_renderer_.reset();
    }
    if (debug_renderer_ != nullptr) {
        remember_failure(debug_renderer_->shutdown());
        debug_renderer_.reset();
    }
    if (scene_render_system_ != nullptr) {
        remember_failure(scene_render_system_->shutdown());
        scene_render_system_.reset();
    }
    chunk_system_.reset();
    if (chunk_cache_ != nullptr) {
        auto status = chunk_cache_->clear();
        if (!status) {
            first_failure = status;
        }
        chunk_cache_.reset();
    }
    if (mesh_manager_ != nullptr) {
        remember_failure(mesh_manager_->shutdown());
        mesh_manager_.reset();
    }
    if (pipeline_cache_ != nullptr) {
        remember_failure(pipeline_cache_->shutdown());
        pipeline_cache_.reset();
    }
    if (material_cache_ != nullptr) {
        remember_failure(material_cache_->shutdown());
        material_cache_.reset();
    }
    if (surface_texture_array_ != nullptr) {
        remember_failure(surface_texture_array_->shutdown());
        surface_texture_array_.reset();
    }
    if (surface_data_texture_array_ != nullptr) {
        remember_failure(surface_data_texture_array_->shutdown());
        surface_data_texture_array_.reset();
    }
    if (texture_manager_ != nullptr) {
        remember_failure(texture_manager_->shutdown());
        texture_manager_.reset();
    }
    if (sampler_cache_ != nullptr) {
        remember_failure(sampler_cache_->shutdown());
        sampler_cache_.reset();
    }
    if (shader_manager_ != nullptr) {
        remember_failure(shader_manager_->shutdown());
        shader_manager_.reset();
    }
    terrain_pipelines_ = {};
    sky_pipeline_ = {};
    sky_pipeline_key_ = {};
    terrain_pipeline_keys_ = {};
    scene_pipelines_ = {};
    scene_pipeline_keys_ = {};
    shadow_pipelines_ = {};
    shadow_pipeline_keys_ = {};
    debug_pipelines_ = {};
    debug_pipeline_keys_ = {};
    ui_pipeline_ = {};
    ui_pipeline_key_ = {};
    terrain_shader_program_ = {};
    sky_shader_program_ = {};
    scene_shader_program_ = {};
    terrain_shadow_shader_program_ = {};
    static_shadow_shader_program_ = {};
    debug_shader_program_ = {};
    ui_shader_program_ = {};
    tone_map_shader_program_ = {};
    tone_map_pipeline_ = {};
    tone_map_pipeline_key_ = {};
    image_quality_shader_programs_ = {};
    image_quality_pipelines_ = {};
    image_quality_pipeline_keys_ = {};
    terrain_texture_array_ = {};
    terrain_normal_texture_array_ = {};
    terrain_surface_texture_array_ = {};
    surface_sampler_ = {};
    ui_texture_atlas_ = {};
    fallback_material_ = {};
    terrain_sampler_ = {};
    ui_sampler_ = {};
    environment_ = {};
    device_.reset();
    owner_thread_ = {};
    return first_failure;
}

core::Status Renderer::wait_idle() {
    if (device_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before waiting for idle");
    }
    return device_->wait_idle();
}

core::Status Renderer::synchronize_chunks(world::WorldState& world, const RenderCamera& camera) {
    if (chunk_system_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before chunk synchronization");
    }
    cpu_timings_.reset();
    frame_started_at_ = std::chrono::steady_clock::now();
    frame_timing_active_ = true;
    auto status = core::Status::ok();
    {
        profiling::ScopedCpuTimingZone synchronization_zone(
            cpu_timings_, profiling::CpuTimingZone::chunk_synchronization);
        status = chunk_system_->synchronize(world, camera);
    }
    update_frontend_stats(world.chunks().chunk_count());
    return status;
}

core::Status Renderer::process_chunk_loads(std::span<const world::ChunkStreamLoadReport> loads) {
    if (chunk_system_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before processing chunk loads");
    }
    return chunk_system_->process_chunk_loads(loads);
}

core::Status Renderer::process_chunk_evictions(std::span<const world::ChunkIdentity> evictions) {
    if (chunk_system_ == nullptr) {
        return core::Status::failure(
            "renderer.not_initialized",
            "renderer must be initialized before processing chunk evictions");
    }
    return chunk_system_->process_chunk_evictions(evictions);
}

core::Status Renderer::process_chunk_evictions(const world::ChunkStreamEvictionReport& eviction) {
    if (chunk_system_ == nullptr) {
        return core::Status::failure(
            "renderer.not_initialized",
            "renderer must be initialized before processing chunk evictions");
    }
    return chunk_system_->process_chunk_evictions(eviction);
}

core::Status Renderer::process_world_render_updates(std::span<const ChunkRenderUpdate> updates) {
    if (chunk_system_ == nullptr) {
        return core::Status::failure(
            "renderer.not_initialized",
            "renderer must be initialized before processing world render updates");
    }
    for (const auto& update : updates) {
        auto status = core::Status::ok();
        switch (update.kind) {
        case ChunkRenderUpdateKind::loaded:
            status =
                process_chunk_loads(std::span<const world::ChunkStreamLoadReport>{&update.load, 1});
            break;
        case ChunkRenderUpdateKind::evicted:
            status =
                process_chunk_evictions(std::span<const world::ChunkIdentity>{&update.identity, 1});
            break;
        default:
            return core::Status::failure("renderer.invalid_world_render_update",
                                         "world render update kind is invalid");
        }
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

core::Result<rhi::RenderFrameStats> Renderer::render(const RenderCamera& camera,
                                                     float simulation_alpha, float delta_seconds) {
    if (device_ == nullptr || chunk_system_ == nullptr || scene_render_system_ == nullptr ||
        clustered_lighting_ == nullptr || cascaded_shadows_ == nullptr ||
        sky_renderer_ == nullptr || debug_renderer_ == nullptr || ui_renderer_ == nullptr ||
        frame_builder_ == nullptr) {
        return core::Result<rhi::RenderFrameStats>::failure(
            "renderer.not_initialized", "renderer must be initialized before rendering");
    }
    if (!frame_timing_active_) {
        cpu_timings_.reset();
        frame_started_at_ = std::chrono::steady_clock::now();
        frame_timing_active_ = true;
    }
    if (std::isfinite(delta_seconds) && delta_seconds > 0.0F) {
        environment_.elapsed_seconds =
            std::fmod(environment_.elapsed_seconds + std::min(delta_seconds, 0.25F), 4'096.0F);
    }
    RenderCommandLists command_lists;
    auto sky_draw = sky_renderer_->build_draw();
    if (!sky_draw) {
        frame_timing_active_ = false;
        return core::Result<rhi::RenderFrameStats>::failure(sky_draw.error().code,
                                                            sky_draw.error().message);
    }
    auto sky_command = sky_draw.value();
    sky_command.camera_relative_origin = {
        environment_.cloud_coverage,
        environment_.cloud_density,
        environment_.elapsed_seconds,
    };
    sky_command.texture_variation_seed = std::bit_cast<std::uint32_t>(environment_.storm_intensity);
    command_lists.sky_draws.push_back(sky_command);
    debug_frame_scratch_.draws = std::move(draw_command_scratch_.debug_draws);
    ui_frame_scratch_.draws = std::move(draw_command_scratch_.ui_draws);
    auto scene_lights = scene_.extract_lights(camera);
    auto lighting_status = clustered_lighting_->update(scene_lights, camera);
    if (!lighting_status) {
        frame_timing_active_ = false;
        return core::Result<rhi::RenderFrameStats>::failure(lighting_status.error().code,
                                                            lighting_status.error().message);
    }
    // Estimate the luminance reaching a typical diffuse surface, rather than treating the
    // environment radiance itself as the final pixel luminance. The latter made daylight
    // adaptation underexpose ordinary albedo by more than a stop.
    float scene_luminance =
        (0.2126F * environment_.ambient_color.x + 0.7152F * environment_.ambient_color.y +
         0.0722F * environment_.ambient_color.z) *
            0.30F +
        environment_.sun_intensity * 0.05F;
    const auto camera_frustum = RenderFrustum::from_view_projection(camera.view_projection);
    for (const auto& light : scene_lights) {
        if (light.kind == RenderLightKind::directional) {
            continue;
        }
        const auto extent = math::Vec3f{light.radius, light.radius, light.radius};
        const math::Bounds3f influence_bounds{light.camera_relative_position - extent,
                                              light.camera_relative_position + extent};
        if (!camera_frustum.intersects(influence_bounds)) {
            continue;
        }
        const auto camera_delta = light.camera_relative_position - camera.local_position;
        const auto distance_squared = math::length_squared(camera_delta);
        const auto attenuation =
            std::clamp(light.radius * light.radius / std::max(distance_squared, 1.0F), 0.0F, 1.0F);
        scene_luminance +=
            (0.2126F * light.color.x + 0.7152F * light.color.y + 0.0722F * light.color.z) *
            light.intensity * attenuation * 0.002F;
    }
    frame_builder_->update_exposure_adaptation(scene_luminance, delta_seconds);
    std::array<RenderLightInstance, local_shadow_map_count> selected_local_shadows;
    std::size_t selected_local_shadow_count = 0;
    for (const auto& candidate : clustered_lighting_->selected_shadow_lights()) {
        const auto found =
            std::ranges::find_if(scene_lights, [&candidate](const RenderLightInstance& light) {
                return light.id == candidate.id;
            });
        if (found != scene_lights.end() &&
            selected_local_shadow_count < selected_local_shadows.size()) {
            selected_local_shadows[selected_local_shadow_count++] = *found;
        }
    }
    auto shadow_status = cascaded_shadows_->update(
        camera, environment_,
        std::span{selected_local_shadows.data(), selected_local_shadow_count});
    if (!shadow_status) {
        frame_timing_active_ = false;
        return core::Result<rhi::RenderFrameStats>::failure(shadow_status.error().code,
                                                            shadow_status.error().message);
    }
    const auto& shadow_data = cascaded_shadows_->gpu_data();
    std::array<math::Mat4f, directional_shadow_cascade_count + local_shadow_map_count>
        shadow_view_projections;
    for (std::size_t cascade = 0; cascade < directional_shadow_cascade_count; ++cascade) {
        shadow_view_projections[cascade] = shadow_data.light_view_projection[cascade];
    }
    for (std::size_t slot = 0; slot < selected_local_shadow_count; ++slot) {
        shadow_view_projections[directional_shadow_cascade_count + slot] =
            shadow_data.local_light_view_projection[slot];
    }
    const auto active_shadow_views =
        std::span{shadow_view_projections.data(),
                  directional_shadow_cascade_count + selected_local_shadow_count};

    ChunkDrawList draws;
    {
        profiling::ScopedCpuTimingZone extraction_zone(cpu_timings_,
                                                       profiling::CpuTimingZone::render_extraction);
        draws = chunk_system_->build_draw_list(camera, std::move(chunk_draw_scratch_),
                                               active_shadow_views);
    }
    chunk_draw_scratch_ = std::move(draws.draws);
    command_lists.opaque_terrain_draws = std::move(draw_command_scratch_.opaque_terrain_draws);
    command_lists.alpha_tested_terrain_draws =
        std::move(draw_command_scratch_.alpha_tested_terrain_draws);
    command_lists.transparent_terrain_draws =
        std::move(draw_command_scratch_.transparent_terrain_draws);
    command_lists.opaque_terrain_draws.clear();
    command_lists.alpha_tested_terrain_draws.clear();
    command_lists.transparent_terrain_draws.clear();
    for (auto& draw : chunk_draw_scratch_) {
        if (draw.pipeline == terrain_pipelines_.opaque) {
            command_lists.opaque_terrain_draws.push_back(draw);
        } else if (draw.pipeline == terrain_pipelines_.alpha_tested) {
            command_lists.alpha_tested_terrain_draws.push_back(draw);
        } else {
            command_lists.transparent_terrain_draws.push_back(draw);
        }
    }

    auto scene_draws = scene_render_system_->build_draw_commands(
        scene_, camera, simulation_alpha, std::move(scene_draw_scratch_), active_shadow_views);
    if (!scene_draws) {
        frame_timing_active_ = false;
        return core::Result<rhi::RenderFrameStats>::failure(scene_draws.error().code,
                                                            scene_draws.error().message);
    }
    command_lists.rich_instance_draws = std::move(scene_draws.value().opaque_and_cutout);
    auto& transparent_instances = scene_draws.value().transparent;
    command_lists.transparent_terrain_draws.insert(
        command_lists.transparent_terrain_draws.end(),
        std::make_move_iterator(transparent_instances.begin()),
        std::make_move_iterator(transparent_instances.end()));
    const auto gust = 1.0F + environment_.wind_gust_strength *
                                 std::sin(environment_.elapsed_seconds *
                                          environment_.wind_gust_frequency * 6.28318530718F);
    const math::Vec3f scene_effect_parameters{
        environment_.wind_velocity.x * gust,
        environment_.wind_velocity.z * gust,
        environment_.elapsed_seconds,
    };
    for (auto& draw : command_lists.rich_instance_draws) {
        draw.camera_relative_origin = scene_effect_parameters;
    }
    for (auto& draw : command_lists.transparent_terrain_draws) {
        if (draw.pipeline == scene_pipelines_.transparent ||
            draw.pipeline == scene_pipelines_.transparent_two_sided ||
            draw.pipeline == scene_pipelines_.additive ||
            draw.pipeline == scene_pipelines_.additive_two_sided ||
            draw.pipeline == scene_pipelines_.premultiplied ||
            draw.pipeline == scene_pipelines_.premultiplied_two_sided) {
            draw.camera_relative_origin = scene_effect_parameters;
        }
    }

    const auto append_shadow_draws = [&](auto& target, const auto& terrain_sources,
                                         const auto& scene_sources,
                                         const math::Mat4f& view_projection) {
        target.reserve(terrain_sources.size() + scene_sources.size());
        for (const auto& draw : terrain_sources) {
            auto shadow_draw = draw;
            shadow_draw.pipeline = draw.pipeline == terrain_pipelines_.alpha_tested
                                       ? shadow_pipelines_[1]
                                       : shadow_pipelines_[0];
            shadow_draw.view_projection_override_enabled = true;
            shadow_draw.view_projection_override = view_projection;
            target.push_back(shadow_draw);
        }
        for (const auto& draw : scene_sources) {
            const auto two_sided_or_cutout =
                draw.pipeline == scene_pipelines_.alpha_tested ||
                draw.pipeline == scene_pipelines_.opaque_two_sided ||
                draw.pipeline == scene_pipelines_.alpha_tested_two_sided;
            auto shadow_draw = draw;
            shadow_draw.pipeline = shadow_pipelines_[two_sided_or_cutout ? 3U : 2U];
            shadow_draw.camera_relative_origin = scene_effect_parameters;
            shadow_draw.view_projection_override_enabled = true;
            shadow_draw.view_projection_override = view_projection;
            target.push_back(shadow_draw);
        }
    };
    for (std::size_t cascade = 0; cascade < directional_shadow_cascade_count; ++cascade) {
        append_shadow_draws(command_lists.directional_shadow_draws[cascade],
                            draws.shadow_draws[cascade],
                            scene_draws.value().shadow_casters[cascade],
                            shadow_data.light_view_projection[cascade]);
    }
    for (std::size_t slot = 0; slot < selected_local_shadow_count; ++slot) {
        const auto shadow_view = directional_shadow_cascade_count + slot;
        append_shadow_draws(command_lists.local_shadow_draws[slot], draws.shadow_draws[shadow_view],
                            scene_draws.value().shadow_casters[shadow_view],
                            shadow_data.local_light_view_projection[slot]);
    }
    auto debug_frame =
        debug_renderer_->build_frame(camera, delta_seconds, std::move(debug_frame_scratch_));
    if (!debug_frame) {
        frame_timing_active_ = false;
        return core::Result<rhi::RenderFrameStats>::failure(debug_frame.error().code,
                                                            debug_frame.error().message);
    }
    command_lists.debug_draws = std::move(debug_frame.value().draws);
    debug_text_labels_ = std::move(debug_frame.value().text_labels);
    auto ui_frame = ui_renderer_->build_frame(std::move(ui_frame_scratch_));
    if (!ui_frame) {
        frame_timing_active_ = false;
        return core::Result<rhi::RenderFrameStats>::failure(ui_frame.error().code,
                                                            ui_frame.error().message);
    }
    command_lists.ui_draws = std::move(ui_frame.value().draws);
    auto frame = [&]() {
        profiling::ScopedCpuTimingZone command_zone(cpu_timings_,
                                                    profiling::CpuTimingZone::command_build);
        return frame_builder_->build(camera, std::move(command_lists), environment_);
    }();
    if (!frame) {
        return core::Result<rhi::RenderFrameStats>::failure(frame.error().code,
                                                            frame.error().message);
    }
    auto executed = device_->execute_frame(frame.value());
    draw_command_scratch_ = {};
    for (auto& pass : frame.value().pass_commands) {
        switch (pass.pass_index) {
        case hdr_pass_index::opaque_terrain:
            draw_command_scratch_.opaque_terrain_draws = std::move(pass.draws);
            break;
        case hdr_pass_index::alpha_tested_terrain:
            draw_command_scratch_.alpha_tested_terrain_draws = std::move(pass.draws);
            break;
        case hdr_pass_index::rich_static_instances:
            draw_command_scratch_.rich_instance_draws = std::move(pass.draws);
            break;
        case hdr_pass_index::transparent_terrain:
            draw_command_scratch_.transparent_terrain_draws = std::move(pass.draws);
            break;
        case hdr_pass_index::debug:
            draw_command_scratch_.debug_draws = std::move(pass.draws);
            break;
        case hdr_pass_index::ui:
            draw_command_scratch_.ui_draws = std::move(pass.draws);
            break;
        default:
            break;
        }
    }
    if (!executed) {
        frame_timing_active_ = false;
        return executed;
    }
    const auto complete_frame_ms = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - frame_started_at_)
                                       .count();
    cpu_timings_.add(profiling::CpuTimingZone::complete_frame, complete_frame_ms);
    stats_.cpu_frame_ms = cpu_timings_.milliseconds(profiling::CpuTimingZone::complete_frame);
    update_frontend_stats(stats_.loaded_chunks);
    update_backend_stats(executed.value());
    frame_timing_active_ = false;
    return executed;
}

core::Result<RenderFrameResult> Renderer::render_frame(const RenderFrameInput& input) {
    if (!std::isfinite(input.simulation_alpha) || input.simulation_alpha < 0.0F ||
        input.simulation_alpha > 1.0F || !std::isfinite(input.delta_seconds) ||
        input.delta_seconds < 0.0F) {
        return core::Result<RenderFrameResult>::failure(
            "renderer.invalid_frame_input",
            "frame interpolation must be in [0,1] and delta time must be finite and nonnegative");
    }
    auto rendered = render(input.camera, input.simulation_alpha, input.delta_seconds);
    if (!rendered) {
        return core::Result<RenderFrameResult>::failure(rendered.error().code,
                                                        rendered.error().message);
    }
    return core::Result<RenderFrameResult>::success({rendered.value(), stats_});
}

core::Status Renderer::resize(rhi::RenderExtent extent) {
    if (device_ == nullptr || frame_builder_ == nullptr || ui_renderer_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before resizing");
    }
    auto status = device_->resize(extent);
    if (!status) {
        return status;
    }
    status = frame_builder_->resize(extent);
    if (!status) {
        return status;
    }
    status = clustered_lighting_->resize(extent);
    if (!status) {
        return status;
    }
    const auto terrain_material = core::PrototypeId::parse("base:materials/milestone_terrain");
    const auto scene_material = core::PrototypeId::parse("base:materials/static_instances");
    const auto transparent_material =
        core::PrototypeId::parse("base:materials/transparent_instances");
    if (!terrain_material || !scene_material || !transparent_material) {
        return core::Status::failure("renderer.invalid_lighting_material",
                                     "internal lighting material ids are invalid");
    }
    status = clustered_lighting_->bind(terrain_material.value(), "local_lights", "light_grid");
    if (!status) {
        return status;
    }
    status = clustered_lighting_->bind(scene_material.value(), "local_lights", "light_grid");
    if (!status) {
        return status;
    }
    status = clustered_lighting_->bind(transparent_material.value(), "local_lights", "light_grid");
    if (!status) {
        return status;
    }
    return ui_renderer_->resize(extent);
}

core::Status Renderer::reload_terrain_shaders(std::span<const std::uint32_t> vertex_spirv,
                                              std::span<const std::uint32_t> fragment_spirv) {
    if (!is_initialized() || shader_manager_ == nullptr || pipeline_cache_ == nullptr ||
        chunk_system_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before shader reload");
    }
    auto status = shader_manager_->reload_program(
        terrain_shader_program_, make_terrain_shader_program(vertex_spirv, fragment_spirv));
    if (!status) {
        return status;
    }
    status = pipeline_cache_->rebuild_program(terrain_shader_program_);
    if (!status) {
        return status;
    }
    std::array<rhi::RenderResourceHandle, 4> rebuilt{};
    for (std::size_t index = 0; index < terrain_pipeline_keys_.size(); ++index) {
        auto pipeline = pipeline_cache_->find(terrain_pipeline_keys_[index]);
        if (!pipeline) {
            return core::Status::failure(pipeline.error().code, pipeline.error().message);
        }
        rebuilt[index] = pipeline.value();
    }
    terrain_pipelines_ = {rebuilt[0], rebuilt[1], rebuilt[2], rebuilt[3]};
    return chunk_system_->set_terrain_pipelines(terrain_pipelines_);
}

core::Status Renderer::reload_static_mesh_shaders(std::span<const std::uint32_t> vertex_spirv,
                                                  std::span<const std::uint32_t> fragment_spirv) {
    if (!is_initialized() || shader_manager_ == nullptr || pipeline_cache_ == nullptr ||
        scene_render_system_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before shader reload");
    }
    auto status = shader_manager_->reload_program(
        scene_shader_program_, make_static_mesh_shader_program(vertex_spirv, fragment_spirv));
    if (!status) {
        return status;
    }
    status = pipeline_cache_->rebuild_program(scene_shader_program_);
    if (!status) {
        return status;
    }
    std::array<rhi::RenderResourceHandle, 10> rebuilt{};
    for (std::size_t index = 0; index < scene_pipeline_keys_.size(); ++index) {
        auto pipeline = pipeline_cache_->find(scene_pipeline_keys_[index]);
        if (!pipeline) {
            return core::Status::failure(pipeline.error().code, pipeline.error().message);
        }
        rebuilt[index] = pipeline.value();
    }
    scene_pipelines_ = {rebuilt[0], rebuilt[1], rebuilt[2], rebuilt[3], rebuilt[4],
                        rebuilt[5], rebuilt[6], rebuilt[7], rebuilt[8], rebuilt[9]};
    return scene_render_system_->set_pipelines(scene_pipelines_);
}

core::Status Renderer::reload_debug_shaders(std::span<const std::uint32_t> vertex_spirv,
                                            std::span<const std::uint32_t> fragment_spirv) {
    if (!is_initialized() || shader_manager_ == nullptr || pipeline_cache_ == nullptr ||
        debug_renderer_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before shader reload");
    }
    auto status = shader_manager_->reload_program(
        debug_shader_program_, make_debug_shader_program(vertex_spirv, fragment_spirv));
    if (!status) {
        return status;
    }
    status = pipeline_cache_->rebuild_program(debug_shader_program_);
    if (!status) {
        return status;
    }
    std::array<rhi::RenderResourceHandle, 2> rebuilt{};
    for (std::size_t index = 0; index < debug_pipeline_keys_.size(); ++index) {
        auto pipeline = pipeline_cache_->find(debug_pipeline_keys_[index]);
        if (!pipeline) {
            return core::Status::failure(pipeline.error().code, pipeline.error().message);
        }
        rebuilt[index] = pipeline.value();
    }
    debug_pipelines_ = {rebuilt[0], rebuilt[1]};
    return debug_renderer_->set_pipelines(debug_pipelines_);
}

core::Status Renderer::reload_ui_shaders(std::span<const std::uint32_t> vertex_spirv,
                                         std::span<const std::uint32_t> fragment_spirv) {
    if (!is_initialized() || shader_manager_ == nullptr || pipeline_cache_ == nullptr ||
        ui_renderer_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before shader reload");
    }
    auto status = shader_manager_->reload_program(
        ui_shader_program_, make_ui_shader_program(vertex_spirv, fragment_spirv));
    if (!status) {
        return status;
    }
    status = pipeline_cache_->rebuild_program(ui_shader_program_);
    if (!status) {
        return status;
    }
    auto pipeline = pipeline_cache_->find(ui_pipeline_key_);
    if (!pipeline) {
        return core::Status::failure(pipeline.error().code, pipeline.error().message);
    }
    ui_pipeline_ = pipeline.value();
    return ui_renderer_->set_pipeline(ui_pipeline_);
}

core::Status Renderer::set_exposure(rhi::RenderExposureSettings exposure) {
    if (frame_builder_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before setting exposure");
    }
    return frame_builder_->set_exposure(exposure);
}

rhi::RenderExposureSettings Renderer::exposure() const noexcept {
    return frame_builder_ == nullptr ? rhi::RenderExposureSettings{} : frame_builder_->exposure();
}

core::Status Renderer::set_lighting_debug_view(LightingDebugView view) {
    if (cascaded_shadows_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before selecting debug views");
    }
    cascaded_shadows_->set_debug_view(view);
    return core::Status::ok();
}

core::Status Renderer::set_environment(rhi::RenderEnvironmentData environment) {
    if (!is_initialized()) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before setting environment");
    }
    auto status = rhi::validate_render_environment(environment);
    if (!status) {
        return status;
    }
    environment_ = environment;
    if (frame_builder_ != nullptr) {
        frame_builder_->set_clear_color(
            {environment.fog_color.x, environment.fog_color.y, environment.fog_color.z, 1.0F});
    }
    return core::Status::ok();
}

void Renderer::set_voxel_lighting_stats(const world::ChunkLightSystemStats& lighting) noexcept {
    stats_.voxel_relight_solve_ms = lighting.last_solve_ms;
    stats_.voxel_relight_apply_ms = lighting.last_apply_ms;
    stats_.voxel_relight_changed_chunks = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        lighting.changed_chunks_this_update,
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.voxel_relight_backlog_cells = lighting.snapshot_pending_cell_count;
    stats_.voxel_relight_visited_cells =
        lighting.last_sunlight_queue_visits + lighting.last_block_light_queue_visits;
    stats_.voxel_relight_stale_results = lighting.stale_results;
    stats_.voxel_relight_failed_results = lighting.failed_results;
    stats_.voxel_relight_apply_budget_overruns = lighting.apply_budget_overruns;
}

void Renderer::set_voxel_fluid_stats(const world::ChunkFluidSystemStats& fluids) noexcept {
    stats_.voxel_fluid_snapshot_ms = fluids.last_snapshot_ms;
    stats_.voxel_fluid_simulation_ms = fluids.last_simulation_ms;
    stats_.voxel_fluid_apply_ms = fluids.last_apply_ms;
    stats_.voxel_fluid_changed_chunks = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        fluids.changed_chunks_this_update,
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.voxel_fluid_active_cells = fluids.active_cell_count;
    stats_.voxel_fluid_processed_cells = fluids.processed_cells_this_update;
    stats_.voxel_fluid_budget_exhaustions = fluids.budget_exhaustions;
    stats_.voxel_fluid_apply_budget_overruns = fluids.apply_budget_overruns;
}

void Renderer::set_particle_stats(const ParticleSystemStats& particles, double presentation_ms,
                                  std::uint32_t material_groups,
                                  std::uint64_t presentation_dropped) noexcept {
    stats_.particle_update_ms = particles.update_ms;
    stats_.particle_presentation_ms = presentation_ms;
    stats_.particle_active = particles.active_particles;
    stats_.particle_emitters = particles.active_emitters;
    stats_.particle_spawned = particles.spawned_this_update;
    stats_.particle_material_groups = material_groups;
    stats_.particle_dropped = particles.dropped_particles + presentation_dropped;
}

void Renderer::set_ui_widget_stats(double layout_ms, double paint_ms,
                                   std::uint32_t widget_count) noexcept {
    stats_.ui_layout_ms = layout_ms;
    stats_.ui_paint_ms = paint_ms;
    stats_.ui_widgets = widget_count;
}

RenderObjectId Renderer::reserve_object_id() {
    return scene_.reserve_object_id();
}

RenderLightId Renderer::reserve_light_id() {
    return scene_.reserve_light_id();
}

RenderSkinPaletteId Renderer::reserve_skin_palette_id() {
    return scene_.reserve_skin_palette_id();
}

core::Result<RenderObjectId> Renderer::create_object(RenderObjectProxy object) {
    if (!is_initialized()) {
        return core::Result<RenderObjectId>::failure("renderer.not_initialized",
                                                     "renderer must be initialized first");
    }
    return scene_.create_object(std::move(object));
}

core::Result<RenderLightId> Renderer::create_light(RenderLightProxy light) {
    if (!is_initialized()) {
        return core::Result<RenderLightId>::failure("renderer.not_initialized",
                                                    "renderer must be initialized first");
    }
    return scene_.create_light(std::move(light));
}

core::Result<RenderSkinPaletteId> Renderer::create_skin_palette(RenderSkinPaletteProxy palette) {
    if (!is_initialized()) {
        return core::Result<RenderSkinPaletteId>::failure("renderer.not_initialized",
                                                          "renderer must be initialized first");
    }
    return scene_.create_skin_palette(std::move(palette));
}

core::Status Renderer::apply_scene_updates(std::span<const RenderSceneUpdate> updates) {
    if (!is_initialized()) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized first");
    }
    return scene_.apply(updates);
}

core::Result<RenderMeshHandle> Renderer::create_static_mesh(const StaticMeshUploadDesc& desc) {
    if (mesh_manager_ == nullptr) {
        return core::Result<RenderMeshHandle>::failure("renderer.not_initialized",
                                                       "renderer must be initialized first");
    }
    return mesh_manager_->create_mesh(desc);
}

core::Result<RenderMeshHandle> Renderer::create_model_primitive(std::string id,
                                                                const assets::ModelAsset& model,
                                                                std::uint32_t primitive_index) {
    if (mesh_manager_ == nullptr) {
        return core::Result<RenderMeshHandle>::failure("renderer.not_initialized",
                                                       "renderer must be initialized first");
    }
    return mesh_manager_->create_model_primitive(std::move(id), model, primitive_index);
}

core::Result<std::uint32_t> Renderer::create_surface_texture(std::string id, std::uint32_t width,
                                                             std::uint32_t height,
                                                             std::span<const std::uint8_t> rgba8) {
    if (!is_initialized() || surface_texture_array_ == nullptr) {
        return core::Result<std::uint32_t>::failure("renderer.not_initialized",
                                                    "renderer must be initialized first");
    }
    auto layer = surface_texture_array_->add(std::move(id), width, height, rgba8);
    if (!layer) {
        return layer;
    }
    auto status = surface_texture_array_->synchronize();
    if (!status) {
        return core::Result<std::uint32_t>::failure(status.error().code, status.error().message);
    }
    status = bind_scene_surface_resources();
    if (!status) {
        return core::Result<std::uint32_t>::failure(status.error().code, status.error().message);
    }
    status = bind_shadow_resources();
    if (!status) {
        return core::Result<std::uint32_t>::failure(status.error().code, status.error().message);
    }
    return layer;
}

core::Result<MaterialRuntimeHandle> Renderer::create_surface_material(MaterialRuntimeDesc desc) {
    if (!is_initialized() || material_cache_ == nullptr ||
        desc.domain != MaterialRuntimeDomain::surface) {
        return core::Result<MaterialRuntimeHandle>::failure(
            "renderer.invalid_surface_material",
            "initialized renderer and surface-domain material are required");
    }
    auto material = material_cache_->upsert(std::move(desc));
    if (!material) {
        return material;
    }
    auto status = material_cache_->synchronize_gpu();
    if (!status) {
        return core::Result<MaterialRuntimeHandle>::failure(status.error().code,
                                                            status.error().message);
    }
    status = bind_scene_surface_resources();
    if (!status) {
        return core::Result<MaterialRuntimeHandle>::failure(status.error().code,
                                                            status.error().message);
    }
    status = bind_shadow_resources();
    if (!status) {
        return core::Result<MaterialRuntimeHandle>::failure(status.error().code,
                                                            status.error().message);
    }
    return material;
}

core::Result<std::vector<ModelRenderMaterialBinding>>
Renderer::create_model_materials(std::string_view asset_id, const assets::ModelAsset& model) {
    if (!is_initialized() || surface_texture_array_ == nullptr ||
        surface_data_texture_array_ == nullptr || material_cache_ == nullptr) {
        return core::Result<std::vector<ModelRenderMaterialBinding>>::failure(
            "renderer.not_initialized", "renderer must be initialized first");
    }
    auto model_status = assets::validate_model_asset(model);
    if (!model_status) {
        return core::Result<std::vector<ModelRenderMaterialBinding>>::failure(
            model_status.error().code, model_status.error().message);
    }
    if (asset_id.empty()) {
        return core::Result<std::vector<ModelRenderMaterialBinding>>::failure(
            "renderer.invalid_model_asset_id", "model render materials require an asset id");
    }

    std::vector<std::uint32_t> color_image_layers(model.images.size(), assets::no_model_index);
    std::vector<std::uint32_t> data_image_layers(model.images.size(), assets::no_model_index);
    std::vector<bool> color_image_usage(model.images.size());
    std::vector<bool> data_image_usage(model.images.size());
    const auto mark_image = [](const assets::ModelTextureBinding& binding,
                               std::vector<bool>& usage) {
        if (binding.image != assets::no_model_index) {
            usage[binding.image] = true;
        }
    };
    for (const auto& material : model.materials) {
        mark_image(material.base_color_texture, color_image_usage);
        mark_image(material.emissive_texture, color_image_usage);
        mark_image(material.metallic_roughness_texture, data_image_usage);
        mark_image(material.normal_texture, data_image_usage);
        mark_image(material.occlusion_texture, data_image_usage);
    }
    for (std::size_t index = 0; index < model.images.size(); ++index) {
        const auto& image = model.images[index];
        if (color_image_usage[index]) {
            auto color_layer = surface_texture_array_->add(std::string(asset_id) + "#image/" +
                                                               std::to_string(index),
                                                           image.width, image.height, image.rgba8);
            if (!color_layer) {
                return core::Result<std::vector<ModelRenderMaterialBinding>>::failure(
                    color_layer.error().code, color_layer.error().message);
            }
            color_image_layers[index] = color_layer.value();
        }
        if (data_image_usage[index]) {
            auto data_layer = surface_data_texture_array_->add(
                std::string(asset_id) + "#data-image/" + std::to_string(index), image.width,
                image.height, image.rgba8);
            if (!data_layer) {
                return core::Result<std::vector<ModelRenderMaterialBinding>>::failure(
                    data_layer.error().code, data_layer.error().message);
            }
            data_image_layers[index] = data_layer.value();
        }
    }
    auto status = surface_texture_array_->synchronize();
    if (!status) {
        return core::Result<std::vector<ModelRenderMaterialBinding>>::failure(
            status.error().code, status.error().message);
    }
    status = surface_data_texture_array_->synchronize();
    if (!status) {
        return core::Result<std::vector<ModelRenderMaterialBinding>>::failure(
            status.error().code, status.error().message);
    }

    auto namespace_id = std::string_view{"base"};
    if (const auto colon = asset_id.find(':'); colon != std::string_view::npos) {
        const auto candidate = asset_id.substr(0, colon);
        if (core::is_valid_namespace_id(candidate)) {
            namespace_id = candidate;
        }
    }
    const auto model_hash = core::stable_hash64_hex(asset_id);
    std::vector<ModelRenderMaterialBinding> result;
    result.reserve(model.materials.size());
    for (std::size_t index = 0; index < model.materials.size(); ++index) {
        const auto& source = model.materials[index];
        const auto material_id =
            core::PrototypeId::parse(std::string(namespace_id) + ":materials/runtime_model_" +
                                     model_hash + "_" + std::to_string(index));
        if (!material_id) {
            return core::Result<std::vector<ModelRenderMaterialBinding>>::failure(
                "renderer.invalid_model_material_id",
                "generated model material prototype id is invalid");
        }
        MaterialRuntimeDesc runtime;
        runtime.id = material_id.value();
        runtime.domain = MaterialRuntimeDomain::surface;
        const auto sampler_state = [&](std::uint32_t sampler_index) {
            const auto sampler = sampler_index == assets::no_model_index
                                     ? assets::ModelSampler{}
                                     : model.samplers[sampler_index];
            return static_cast<std::uint32_t>(sampler.mag_filter) |
                   (static_cast<std::uint32_t>(sampler.min_filter) << 1U) |
                   (static_cast<std::uint32_t>(sampler.wrap_s) << 4U) |
                   (static_cast<std::uint32_t>(sampler.wrap_t) << 6U);
        };
        const auto runtime_binding = [&](const assets::ModelTextureBinding& binding, bool color,
                                         std::uint32_t fallback) {
            RuntimeSurfaceTextureBinding result_binding;
            result_binding.texture =
                binding.image == assets::no_model_index
                    ? fallback
                    : (color ? color_image_layers : data_image_layers)[binding.image];
            result_binding.sampler_state = sampler_state(binding.sampler);
            result_binding.texcoord = binding.texcoord;
            result_binding.offset = {binding.offset.x, binding.offset.y};
            result_binding.scale = {binding.scale.x, binding.scale.y};
            result_binding.rotation = binding.rotation;
            return result_binding;
        };
        runtime.base_color_texture = runtime_binding(source.base_color_texture, true,
                                                     surface_texture_array_->fallbacks().white);
        runtime.metallic_roughness_texture =
            runtime_binding(source.metallic_roughness_texture, false,
                            surface_data_texture_array_->fallbacks().white);
        runtime.normal_texture = runtime_binding(source.normal_texture, false,
                                                 surface_data_texture_array_->fallbacks().normal);
        runtime.occlusion_texture = runtime_binding(source.occlusion_texture, false,
                                                    surface_data_texture_array_->fallbacks().white);
        runtime.emissive_texture = runtime_binding(source.emissive_texture, true,
                                                   surface_texture_array_->fallbacks().white);
        runtime.surface_texture = runtime.base_color_texture.texture;
        runtime.base_color = source.base_color_factor;
        runtime.alpha_cutoff = source.alpha_cutoff;
        runtime.emissive_color = source.emissive_factor;
        runtime.metallic = source.metallic_factor;
        runtime.roughness = source.roughness_factor;
        runtime.normal_scale = source.normal_scale;
        runtime.occlusion_strength = source.occlusion_strength;
        ModelRenderMaterialBinding binding;
        binding.layer =
            source.alpha_mode == assets::ModelAlphaMode::mask    ? RenderLayer::alpha_tested
            : source.alpha_mode == assets::ModelAlphaMode::blend ? RenderLayer::transparent
                                                                 : RenderLayer::opaque;
        if (source.alpha_mode == assets::ModelAlphaMode::mask) {
            runtime.flags = runtime.flags | VoxelMaterialFlags::alpha_tested;
        }
        if (source.double_sided) {
            runtime.flags = runtime.flags | VoxelMaterialFlags::two_sided;
            binding.flags = binding.flags | RenderObjectFlags::two_sided;
        }
        if (source.alpha_mode == assets::ModelAlphaMode::blend) {
            runtime.flags = runtime.flags | VoxelMaterialFlags::translucent;
        }
        if (source.unlit) {
            runtime.flags = runtime.flags | VoxelMaterialFlags::unlit;
        }
        auto created = material_cache_->upsert(std::move(runtime));
        if (!created) {
            return core::Result<std::vector<ModelRenderMaterialBinding>>::failure(
                created.error().code, created.error().message);
        }
        binding.material = created.value();
        result.push_back(binding);
    }
    status = material_cache_->synchronize_gpu();
    if (!status) {
        return core::Result<std::vector<ModelRenderMaterialBinding>>::failure(
            status.error().code, status.error().message);
    }
    status = bind_scene_surface_resources();
    if (!status) {
        return core::Result<std::vector<ModelRenderMaterialBinding>>::failure(
            status.error().code, status.error().message);
    }
    status = bind_shadow_resources();
    if (!status) {
        return core::Result<std::vector<ModelRenderMaterialBinding>>::failure(
            status.error().code, status.error().message);
    }
    return core::Result<std::vector<ModelRenderMaterialBinding>>::success(std::move(result));
}

core::Status Renderer::release_static_mesh(RenderMeshHandle handle) {
    if (mesh_manager_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized first");
    }
    return mesh_manager_->release(handle);
}

bool Renderer::is_initialized() const noexcept {
    return device_ != nullptr && chunk_cache_ != nullptr && chunk_system_ != nullptr &&
           frame_builder_ != nullptr && shader_manager_ != nullptr && texture_manager_ != nullptr &&
           material_cache_ != nullptr && pipeline_cache_ != nullptr && mesh_manager_ != nullptr &&
           surface_texture_array_ != nullptr && surface_texture_array_->texture().is_valid() &&
           surface_data_texture_array_ != nullptr &&
           surface_data_texture_array_->texture().is_valid() && scene_render_system_ != nullptr &&
           sky_renderer_ != nullptr && sky_renderer_->is_initialized() &&
           debug_renderer_ != nullptr && ui_renderer_ != nullptr && sky_pipeline_.is_valid() &&
           terrain_pipelines_.is_valid() && scene_pipelines_.is_valid() &&
           debug_pipelines_.is_valid() && ui_pipeline_.is_valid() &&
           fallback_material_.is_valid() && surface_sampler_.is_valid();
}

bool Renderer::is_owner_thread() const noexcept {
    return owner_thread_ != std::thread::id{} && owner_thread_ == std::this_thread::get_id();
}

const ChunkRenderStats& Renderer::chunk_stats() const noexcept {
    return chunk_system_ == nullptr ? empty_chunk_stats : chunk_system_->stats();
}

const RendererStats& Renderer::stats() const noexcept {
    return stats_;
}

const SceneRenderStats& Renderer::scene_stats() const noexcept {
    static const SceneRenderStats empty;
    return scene_render_system_ == nullptr ? empty : scene_render_system_->stats();
}

const ClusteredLightingStats& Renderer::lighting_stats() const noexcept {
    static const ClusteredLightingStats empty;
    return clustered_lighting_ == nullptr ? empty : clustered_lighting_->stats();
}

RendererFallbackResources Renderer::fallback_resources() const noexcept {
    RendererFallbackResources resources;
    resources.error_mesh = fallback_mesh();
    resources.error_material = fallback_material_;
    if (texture_manager_ != nullptr) {
        resources.error_texture = texture_manager_->error_texture();
        resources.white_texture = texture_manager_->white_texture();
        resources.black_texture = texture_manager_->black_texture();
        resources.normal_texture = texture_manager_->normal_texture();
    }
    return resources;
}

RenderMeshHandle Renderer::fallback_mesh() const noexcept {
    return mesh_manager_ == nullptr ? RenderMeshHandle{} : mesh_manager_->fallback_mesh();
}

MaterialRuntimeHandle Renderer::fallback_material() const noexcept {
    return fallback_material_;
}

std::optional<MaterialRuntimeDesc>
Renderer::describe_material(MaterialRuntimeHandle handle) const noexcept {
    return material_cache_ == nullptr ? std::nullopt : material_cache_->describe(handle);
}

std::optional<MaterialRuntimeDesc>
Renderer::describe_voxel_material(std::uint16_t voxel_type) const noexcept {
    if (material_cache_ == nullptr || voxel_type == world::VoxelDefinition::air_type) {
        return std::nullopt;
    }
    const auto runtime_id =
        core::PrototypeId::parse("base:materials/runtime_voxel_" + std::to_string(voxel_type));
    if (!runtime_id) {
        return std::nullopt;
    }
    const auto* view = material_cache_->find(runtime_id.value());
    return view == nullptr ? std::nullopt : material_cache_->describe(view->handle);
}

std::optional<TextureView> Renderer::describe_terrain_texture() const noexcept {
    if (texture_manager_ == nullptr) {
        return std::nullopt;
    }
    const auto* view = texture_manager_->find(terrain_texture_array_);
    return view == nullptr ? std::nullopt : std::optional<TextureView>{*view};
}

std::optional<TextureView> Renderer::describe_surface_texture() const noexcept {
    if (surface_texture_array_ == nullptr) {
        return std::nullopt;
    }
    const auto* view = surface_texture_array_->texture_view();
    return view == nullptr ? std::nullopt : std::optional<TextureView>{*view};
}

DebugRenderer* Renderer::debug_renderer() noexcept {
    return debug_renderer_.get();
}

const DebugRenderer* Renderer::debug_renderer() const noexcept {
    return debug_renderer_.get();
}

std::span<const DebugTextLabelFrame> Renderer::debug_text_labels() const noexcept {
    return debug_text_labels_;
}

UiRenderer* Renderer::ui_renderer() noexcept {
    return ui_renderer_.get();
}

const UiRenderer* Renderer::ui_renderer() const noexcept {
    return ui_renderer_.get();
}

rhi::IRenderDevice* Renderer::device() noexcept {
    return device_.get();
}

const rhi::IRenderDevice* Renderer::device() const noexcept {
    return device_.get();
}

void Renderer::update_frontend_stats(std::size_t loaded_chunk_count) noexcept {
    const auto saturating_u32 = [](std::size_t value) noexcept {
        return static_cast<std::uint32_t>(
            std::min(value, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    };
    const auto& chunks = chunk_stats();
    stats_.render_extraction_ms =
        cpu_timings_.milliseconds(profiling::CpuTimingZone::render_extraction);
    stats_.chunk_synchronization_ms =
        cpu_timings_.milliseconds(profiling::CpuTimingZone::chunk_synchronization);
    stats_.culling_ms = chunks.culling_ms;
    stats_.draw_list_ms = chunks.draw_list_ms;
    stats_.command_build_ms = cpu_timings_.milliseconds(profiling::CpuTimingZone::command_build);
    stats_.chunk_snapshot_ms = chunks.chunk_snapshot_ms;
    stats_.meshing_ms = chunks.meshing_ms;
    stats_.upload_preparation_ms = chunks.upload_preparation_ms;
    stats_.upload_ms = chunks.upload_ms;
    stats_.gpu_wait_ms = chunks.gpu_wait_ms;
    stats_.loaded_chunks = saturating_u32(loaded_chunk_count);
    stats_.mesh_pending_chunks = saturating_u32(chunks.pending_mesh_count);
    stats_.upload_pending_chunks = saturating_u32(chunks.pending_upload_count);
    stats_.mesh_failures = chunks.total_failed_mesh_count;
    stats_.upload_failures = chunks.total_failed_upload_count;
    stats_.stale_mesh_results = chunks.total_stale_mesh_result_count;
    stats_.resident_chunks = saturating_u32(chunks.cache.resident_chunk_count);
    stats_.visible_chunks = saturating_u32(chunks.visible_chunk_count);
    stats_.culled_chunks = saturating_u32(chunks.culled_chunk_count);
    stats_.drawn_chunks = saturating_u32(chunks.drawn_chunk_count);
    stats_.residency_suppressed_chunks = saturating_u32(chunks.residency_suppressed_chunk_count);
    if (texture_manager_ != nullptr) {
        stats_.resident_textures = saturating_u32(texture_manager_->stats().resident_texture_count);
        stats_.resident_texture_bytes = texture_manager_->stats().resident_texture_bytes;
    }
    if (material_cache_ != nullptr) {
        stats_.runtime_materials = saturating_u32(material_cache_->stats().resident_material_count);
    }
    if (pipeline_cache_ != nullptr) {
        stats_.resident_pipelines =
            saturating_u32(pipeline_cache_->stats().resident_pipeline_count);
    }
    if (scene_render_system_ != nullptr) {
        const auto& scene = scene_render_system_->stats();
        stats_.retained_objects = scene.scene.retained_objects;
        stats_.retained_skin_palettes = scene.scene.retained_skin_palettes;
        stats_.visible_objects = scene.scene.visible_objects;
        stats_.culled_objects = scene.scene.culled_objects;
        stats_.instance_batches = scene.scene.instance_batches;
        stats_.submitted_instances = scene.submitted_instances;
        stats_.instance_draw_calls = scene.draw_calls;
        stats_.dropped_instances = scene.dropped_instances;
        stats_.uploaded_instance_bytes = scene.uploaded_instance_bytes;
        stats_.submitted_skin_palettes = scene.submitted_skin_palettes;
        stats_.submitted_skin_matrices = scene.submitted_skin_matrices;
        stats_.dropped_skinned_instances = scene.dropped_skinned_instances;
        stats_.uploaded_skin_matrix_bytes = scene.uploaded_skin_matrix_bytes;
    }
    if (mesh_manager_ != nullptr) {
        const auto meshes = mesh_manager_->stats();
        stats_.resident_static_meshes = saturating_u32(meshes.resident_mesh_count);
        stats_.resident_static_mesh_bytes = meshes.resident_mesh_bytes;
    }
    if (debug_renderer_ != nullptr) {
        const auto& debug = debug_renderer_->stats();
        stats_.debug_lines = debug.submitted_lines;
        stats_.debug_draw_calls = debug.draw_calls;
        stats_.debug_labels = debug.active_text_labels;
        stats_.debug_overflow = debug.overflowed_lines + debug.overflowed_text_labels;
        stats_.debug_uploaded_bytes = debug.uploaded_bytes;
    }
    if (ui_renderer_ != nullptr) {
        const auto& ui = ui_renderer_->stats();
        stats_.ui_draw_calls = ui.draw_calls;
        stats_.ui_clipped_draw_calls = ui.clipped_draw_calls;
        stats_.ui_vertices = ui.submitted_vertices;
        stats_.ui_glyphs = ui.submitted_glyphs;
        stats_.ui_uploaded_bytes = ui.uploaded_bytes;
        stats_.ui_overflow = ui.overflowed_batches;
    }
    stats_.vertices = chunks.visible_vertex_count;
    stats_.triangles = chunks.visible_index_count / 3;
    stats_.resident_mesh_bytes = chunks.cache.resident_bytes;
    stats_.gpu_terrain_budget_bytes = chunks.gpu_terrain_budget_bytes;
    stats_.distance_evicted_meshes = chunks.distance_evicted_mesh_count;
    stats_.memory_pressure_evicted_meshes = chunks.memory_pressure_evicted_mesh_count;
    stats_.gpu_arena_capacity_bytes =
        chunks.cache.vertex_arena.capacity_bytes + chunks.cache.index_arena.capacity_bytes;
    stats_.gpu_arena_used_bytes =
        chunks.cache.vertex_arena.used_bytes + chunks.cache.index_arena.used_bytes;
    stats_.gpu_arena_free_bytes =
        chunks.cache.vertex_arena.free_bytes + chunks.cache.index_arena.free_bytes;
    const auto arena_free = stats_.gpu_arena_free_bytes;
    const auto arena_largest = chunks.cache.vertex_arena.largest_free_range_bytes +
                               chunks.cache.index_arena.largest_free_range_bytes;
    stats_.gpu_arena_fragmentation = arena_free == 0 ? 0.0
                                                     : 1.0 - static_cast<double>(arena_largest) /
                                                                 static_cast<double>(arena_free);
    stats_.pending_upload_bytes = chunks.pending_upload_bytes;
    stats_.uploaded_bytes_this_frame = chunks.uploaded_bytes;
}

void Renderer::update_backend_stats(const rhi::RenderFrameStats& frame) noexcept {
    stats_.frame_index = frame.frame_index;
    stats_.submission_serial = frame.submission_serial;
    stats_.completed_submission_serial = frame.completed_submission_serial;
    stats_.draw_calls = static_cast<std::uint32_t>(std::min(
        frame.draw_count, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.opaque_terrain_draws = static_cast<std::uint32_t>(
        std::min(frame.opaque_terrain_draw_count,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.alpha_tested_terrain_draws = static_cast<std::uint32_t>(
        std::min(frame.alpha_tested_terrain_draw_count,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.transparent_terrain_draws = static_cast<std::uint32_t>(
        std::min(frame.transparent_terrain_draw_count,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.pipeline_switches = static_cast<std::uint32_t>(
        std::min(frame.pipeline_bind_count,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.triangles = frame.total_indices / 3U;
    stats_.command_recording_ms = frame.cpu_command_recording_ms;
    stats_.gpu_wait_ms += frame.cpu_gpu_wait_ms;
    stats_.gpu_timing_valid = frame.gpu_timing_valid;
    stats_.gpu_timing_frame_index = frame.gpu_timing_frame_index;
    stats_.gpu_timing_latency_frames = frame.gpu_timing_latency_frames;
    stats_.gpu_upload_timing_valid = frame.gpu_upload_timing_valid;
    stats_.gpu_upload_submission_serial = frame.gpu_upload_submission_serial;
    stats_.gpu_frame_ms = frame.gpu_frame_ms;
    stats_.gpu_opaque_terrain_ms = frame.gpu_opaque_terrain_ms;
    stats_.gpu_alpha_tested_terrain_ms = frame.gpu_alpha_tested_terrain_ms;
    stats_.gpu_transparent_terrain_ms = frame.gpu_transparent_terrain_ms;
    stats_.gpu_upload_ms = frame.gpu_upload_ms;
    stats_.gpu_transfer_ms = frame.gpu_transfer_ms;
    stats_.gpu_final_copy_ms = frame.gpu_final_copy_ms;
}

rhi::RenderImageFormat Renderer::scene_color_format() const noexcept {
    return rhi::RenderImageFormat::rgba16_sfloat;
}

core::Status Renderer::create_sky_pipeline(std::span<const std::uint32_t> vertex_spirv,
                                           std::span<const std::uint32_t> fragment_spirv) {
    if (shader_manager_ == nullptr || pipeline_cache_ == nullptr) {
        return core::Status::failure("renderer.runtime_assets_uninitialized",
                                     "sky runtime asset managers must be initialized first");
    }
    const auto material = core::PrototypeId::parse("base:materials/sky");
    if (!material) {
        return core::Status::failure("renderer.invalid_sky_material",
                                     "internal sky material id is invalid");
    }
    auto shader =
        shader_manager_->create_program(make_sky_shader_program(vertex_spirv, fragment_spirv));
    if (!shader) {
        return core::Status::failure(shader.error().code, shader.error().message);
    }
    sky_shader_program_ = shader.value();

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/sky.vert"};
    layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    layout.debug_name = "sky_gradient_layout";

    rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.material_id = material.value();
    pipeline.debug_name = "sky_gradient_pipeline";
    pipeline.vertex_stride = sizeof(GpuSkyVertex);
    pipeline.vertex_attributes.assign(gpu_sky_vertex_attributes.begin(),
                                      gpu_sky_vertex_attributes.end());
    pipeline.topology = rhi::RenderPrimitiveTopology::triangle_list;
    pipeline.polygon_mode = rhi::RenderPolygonMode::fill;
    pipeline.cull_mode = rhi::RenderCullMode::none;
    pipeline.front_face = rhi::RenderFrontFace::counter_clockwise;
    pipeline.depth_test_enable = true;
    pipeline.depth_write_enable = false;
    pipeline.depth_compare = rhi::RenderCompareOperation::always;
    pipeline.blend_mode = rhi::RenderBlendMode::disabled;
    pipeline.color_target_format = scene_color_format();
    pipeline.depth_target_format = rhi::RenderImageFormat::d32_sfloat;

    sky_pipeline_key_.shader_program = sky_shader_program_;
    sky_pipeline_key_.vertex_layout =
        hash_vertex_layout(pipeline.vertex_stride, pipeline.vertex_attributes);
    sky_pipeline_key_.render_phase = RenderPhase::sky;
    sky_pipeline_key_.color_format = pipeline.color_target_format;
    sky_pipeline_key_.depth_format = pipeline.depth_target_format;
    sky_pipeline_key_.cull_mode = pipeline.cull_mode;
    sky_pipeline_key_.front_face = pipeline.front_face;
    sky_pipeline_key_.depth_test = pipeline.depth_test_enable;
    sky_pipeline_key_.depth_write = pipeline.depth_write_enable;
    sky_pipeline_key_.depth_compare = pipeline.depth_compare;
    sky_pipeline_key_.blend_mode = pipeline.blend_mode;
    auto created = pipeline_cache_->prewarm(sky_pipeline_key_, layout, std::move(pipeline));
    if (!created) {
        return core::Status::failure(created.error().code, created.error().message);
    }
    sky_pipeline_ = created.value();
    return core::Status::ok();
}

core::Status
Renderer::create_terrain_pipeline(std::span<const std::uint32_t> vertex_spirv,
                                  std::span<const std::uint32_t> fragment_spirv,
                                  const world::VoxelPalette* voxel_palette,
                                  const materials::TerrainMaterialAssetSet& material_assets) {
    if (shader_manager_ == nullptr || sampler_cache_ == nullptr || texture_manager_ == nullptr ||
        material_cache_ == nullptr || pipeline_cache_ == nullptr) {
        return core::Status::failure("renderer.runtime_assets_uninitialized",
                                     "terrain runtime asset managers must be initialized first");
    }
    const auto material = core::PrototypeId::parse("base:materials/milestone_terrain");
    if (!material) {
        return core::Status::failure("renderer.invalid_terrain_material",
                                     "internal terrain material prototype id is invalid");
    }
    auto asset_status = material_assets.validate();
    if (!asset_status) {
        return asset_status;
    }

    TerrainTextureArrayBuilder texture_builder(terrain_material_tile_size,
                                               terrain_material_tile_size);
    TerrainTextureArrayBuilder normal_builder(terrain_material_tile_size,
                                              terrain_material_tile_size);
    TerrainTextureArrayBuilder surface_builder(terrain_material_tile_size,
                                               terrain_material_tile_size);
    const auto default_surface = make_terrain_surface_tile();
    const auto add_aligned_layer =
        [&](std::string id, std::span<const std::byte> color, std::span<const std::byte> normal,
            std::span<const std::byte> surface) -> core::Result<std::uint32_t> {
        auto color_layer = texture_builder.add_layer(id + "#color", color);
        if (!color_layer) {
            return color_layer;
        }
        auto normal_layer = normal_builder.add_layer(id + "#normal", normal);
        if (!normal_layer) {
            return normal_layer;
        }
        auto surface_layer = surface_builder.add_layer(id + "#surface", surface);
        if (!surface_layer) {
            return surface_layer;
        }
        if (normal_layer.value() != color_layer.value() ||
            surface_layer.value() != color_layer.value()) {
            return core::Result<std::uint32_t>::failure(
                "renderer.unaligned_terrain_texture_arrays",
                "terrain color, normal, and surface array layers lost index alignment");
        }
        return color_layer;
    };
    auto error_color = make_terrain_tile({255, 0, 255}, true);
    auto error_normal = make_flat_terrain_normal_tile();
    auto layer = add_aligned_layer("error", error_color, error_normal, default_surface);
    if (!layer) {
        return core::Status::failure(layer.error().code, layer.error().message);
    }
    constexpr std::array<std::array<std::uint8_t, 3>, 6> terrain_colors{
        std::array<std::uint8_t, 3>{74, 145, 57},   std::array<std::uint8_t, 3>{118, 78, 46},
        std::array<std::uint8_t, 3>{112, 116, 124}, std::array<std::uint8_t, 3>{184, 162, 98},
        std::array<std::uint8_t, 3>{54, 111, 48},   std::array<std::uint8_t, 3>{127, 91, 59},
    };
    for (std::size_t index = 0; index < terrain_colors.size(); ++index) {
        auto color = make_terrain_tile(terrain_colors[index]);
        auto normal = derive_terrain_normal_map(color);
        layer = add_aligned_layer("terrain_" + std::to_string(index + 1), color, normal,
                                  default_surface);
        if (!layer) {
            return core::Status::failure(layer.error().code, layer.error().message);
        }
    }
    std::vector<std::vector<std::byte>> resized_material_textures;
    resized_material_textures.reserve(material_assets.textures.size());
    for (const auto& texture : material_assets.textures) {
        auto resized =
            resize_surface_rgba8(texture.image.width, texture.image.height, texture.image.rgba8,
                                 terrain_material_tile_size, terrain_material_tile_size);
        resized_material_textures.push_back(std::move(resized));
    }

    std::vector<std::uint16_t> voxel_types;
    if (voxel_palette != nullptr && !voxel_palette->empty()) {
        const auto definitions = voxel_palette->definitions();
        voxel_types.reserve(definitions.size());
        for (const auto* definition : definitions) {
            voxel_types.push_back(definition->type);
        }
    } else {
        voxel_types.reserve(255);
        for (std::uint16_t type = 1; type <= 255; ++type) {
            voxel_types.push_back(type);
        }
    }

    struct TextureRange {
        std::uint32_t start = 0;
        std::uint32_t count = 1;
    };
    using TextureRangeKey = std::tuple<std::vector<std::uint32_t>, std::vector<std::uint32_t>,
                                       std::vector<std::uint32_t>>;
    std::map<TextureRangeKey, TextureRange> packed_texture_ranges;
    const auto resolve_texture_range = [&](const std::vector<std::uint32_t>& color_indices,
                                           const std::vector<std::uint32_t>& normal_indices,
                                           const std::vector<std::uint32_t>& surface_indices,
                                           std::uint32_t fallback, std::uint16_t voxel_type,
                                           VoxelMaterialFace face) -> core::Result<TextureRange> {
        if (color_indices.empty()) {
            return core::Result<TextureRange>::success({fallback, 1});
        }
        if (color_indices.size() > std::numeric_limits<std::uint32_t>::max()) {
            return core::Result<TextureRange>::failure(
                "renderer.too_many_terrain_texture_variants",
                "terrain face texture variant count exceeds uint32");
        }
        const TextureRangeKey key{color_indices, normal_indices, surface_indices};
        const auto existing = packed_texture_ranges.find(key);
        if (existing != packed_texture_ranges.end()) {
            return core::Result<TextureRange>::success(existing->second);
        }

        TextureRange range;
        range.count = static_cast<std::uint32_t>(color_indices.size());
        const auto sequence = packed_texture_ranges.size();
        for (std::size_t index = 0; index < color_indices.size(); ++index) {
            const auto color_index = color_indices[index];
            const auto& color = resized_material_textures[color_index];
            auto derived_normal = derive_terrain_normal_map(color);
            const auto& normal =
                normal_indices.empty()
                    ? derived_normal
                    : resized_material_textures
                          [normal_indices[normal_indices.size() == 1U ? 0U : index]];
            const auto& surface =
                surface_indices.empty()
                    ? default_surface
                    : resized_material_textures
                          [surface_indices[surface_indices.size() == 1U ? 0U : index]];
            const auto id = material_assets.textures[color_index].logical_id + "#voxel-" +
                            std::to_string(voxel_type) + "-" +
                            std::string(voxel_material_face_name(face)) + "-sequence-" +
                            std::to_string(sequence) + "-variant-" + std::to_string(index);
            auto packed = add_aligned_layer(id, color, normal, surface);
            if (!packed) {
                return core::Result<TextureRange>::failure(packed.error().code,
                                                           packed.error().message);
            }
            if (index == 0) {
                range.start = packed.value();
            } else if (packed.value() != range.start + index) {
                return core::Result<TextureRange>::failure(
                    "renderer.non_contiguous_terrain_texture_variants",
                    "terrain texture arrays failed to pack face variants contiguously");
            }
        }
        packed_texture_ranges.emplace(key, range);
        return core::Result<TextureRange>::success(range);
    };

    std::map<std::uint32_t, std::uint32_t> overlay_texture_layers;
    const auto resolve_overlay_layer =
        [&](std::uint32_t asset_index) -> core::Result<std::uint32_t> {
        const auto found = overlay_texture_layers.find(asset_index);
        if (found != overlay_texture_layers.end()) {
            return core::Result<std::uint32_t>::success(found->second);
        }
        const auto& color = resized_material_textures[asset_index];
        auto normal = derive_terrain_normal_map(color);
        auto added =
            add_aligned_layer(material_assets.textures[asset_index].logical_id + "#surface-overlay",
                              color, normal, default_surface);
        if (!added) {
            return added;
        }
        overlay_texture_layers.emplace(asset_index, added.value());
        return added;
    };

    std::vector<MaterialRuntimeDesc> runtime_materials;
    runtime_materials.reserve(voxel_types.size());
    for (const auto type : voxel_types) {
        auto runtime_id =
            core::PrototypeId::parse("base:materials/runtime_voxel_" + std::to_string(type));
        if (!runtime_id) {
            return core::Status::failure("renderer.invalid_runtime_material_id",
                                         "generated voxel material id is invalid");
        }
        MaterialRuntimeDesc runtime_material;
        runtime_material.id = runtime_id.value();
        runtime_material.voxel_type = type;
        apply_default_terrain_surface_layers(runtime_material);
        const auto fallback_texture = 1U + (static_cast<std::uint32_t>(type) - 1U) % 6U;
        runtime_material.face_texture_starts.fill(fallback_texture);
        if (const auto* material_asset = material_assets.find(type)) {
            constexpr std::array faces{
                VoxelMaterialFace::west, VoxelMaterialFace::east,  VoxelMaterialFace::bottom,
                VoxelMaterialFace::top,  VoxelMaterialFace::north, VoxelMaterialFace::south,
            };
            for (const auto face : faces) {
                auto range = resolve_texture_range(
                    material_asset->textures_for(face), material_asset->normal_textures_for(face),
                    material_asset->surface_textures_for(face), fallback_texture, type, face);
                if (!range) {
                    return core::Status::failure(range.error().code, range.error().message);
                }
                const auto face_index = voxel_material_face_index(face);
                runtime_material.face_texture_starts[face_index] = range.value().start;
                runtime_material.face_texture_counts[face_index] = range.value().count;
            }
            runtime_material.base_color = material_asset->base_color;
            runtime_material.roughness = material_asset->roughness;
            runtime_material.metallic = material_asset->metallic;
            runtime_material.occlusion_strength = material_asset->ambient_occlusion;
            runtime_material.emissive_strength = material_asset->emissive_strength;
            runtime_material.normal_scale = material_asset->normal_scale;
            runtime_material.texel_density = material_asset->texel_density;
            runtime_material.biome_tint = material_asset->biome_tint;
            runtime_material.biome_tint_strength = material_asset->biome_tint_strength;
            runtime_material.macro_color_strength = material_asset->macro_color_strength;
            runtime_material.macro_roughness_strength = material_asset->macro_roughness_strength;
            runtime_material.transition_width = material_asset->transition_width;
            runtime_material.transition_contrast = material_asset->transition_contrast;
            runtime_material.transition_noise_scale = material_asset->transition_noise_scale;
            for (std::size_t index = 0; index < material_asset->surface_layers.size(); ++index) {
                const auto& source = material_asset->surface_layers[index];
                auto& destination = runtime_material.terrain_surface_layers[index];
                destination.tint = source.tint;
                destination.strength = source.strength;
                destination.roughness = source.roughness;
                destination.metallic = source.metallic;
                destination.emissive_strength = source.emissive_strength;
                if (source.texture != materials::no_terrain_texture_asset) {
                    auto overlay_layer = resolve_overlay_layer(source.texture);
                    if (!overlay_layer) {
                        return core::Status::failure(overlay_layer.error().code,
                                                     overlay_layer.error().message);
                    }
                    destination.texture_layer = overlay_layer.value();
                }
            }
            if (material_asset->stable_rotations) {
                runtime_material.flags =
                    runtime_material.flags | VoxelMaterialFlags::stable_rotations;
            }
            if (material_asset->stable_mirroring) {
                runtime_material.flags =
                    runtime_material.flags | VoxelMaterialFlags::stable_mirroring;
            }
            if (material_asset->emissive_strength > 0.0F) {
                runtime_material.flags = runtime_material.flags | VoxelMaterialFlags::emissive;
            }
            if (material_asset->unlit) {
                runtime_material.flags = runtime_material.flags | VoxelMaterialFlags::unlit;
            }
            if (material_asset->blend_mode == materials::MaterialBlendMode::masked) {
                runtime_material.flags = runtime_material.flags | VoxelMaterialFlags::alpha_tested;
            } else if (material_asset->blend_mode == materials::MaterialBlendMode::translucent ||
                       material_asset->blend_mode == materials::MaterialBlendMode::additive) {
                runtime_material.flags = runtime_material.flags | VoxelMaterialFlags::translucent;
            }
            if (material_asset->double_sided) {
                runtime_material.flags = runtime_material.flags | VoxelMaterialFlags::two_sided;
            }
        }
        if (voxel_palette != nullptr) {
            const auto* definition = voxel_palette->find_by_type(type);
            if (definition != nullptr) {
                const auto& model = voxel_palette->model_for(*definition);
                if (definition->logical_occupancy == world::BlockLogicalOccupancy::fluid) {
                    runtime_material.flags = runtime_material.flags |
                                             VoxelMaterialFlags::translucent |
                                             VoxelMaterialFlags::fluid;
                    runtime_material.base_color[3] = 0.68F;
                    runtime_material.roughness = 0.2F;
                }
                if (model.kind == world::BlockModelKind::cross_plane) {
                    runtime_material.flags = runtime_material.flags |
                                             VoxelMaterialFlags::alpha_tested |
                                             VoxelMaterialFlags::two_sided;
                }
                if (definition->light_emission > 0) {
                    runtime_material.flags = runtime_material.flags | VoxelMaterialFlags::emissive;
                    runtime_material.emissive_strength =
                        static_cast<float>(definition->light_emission) / 255.0F;
                }
            }
        }
        runtime_materials.push_back(std::move(runtime_material));
    }

    auto texture_desc = texture_builder.build("terrain_texture_array");
    if (!texture_desc) {
        return core::Status::failure(texture_desc.error().code, texture_desc.error().message);
    }
    auto texture = texture_manager_->create_texture(std::move(texture_desc).value());
    if (!texture) {
        return core::Status::failure(texture.error().code, texture.error().message);
    }
    terrain_texture_array_ = texture.value();
    auto normal_desc =
        normal_builder.build("terrain_normal_texture_array", TextureColorSpace::linear);
    if (!normal_desc) {
        return core::Status::failure(normal_desc.error().code, normal_desc.error().message);
    }
    auto normal_texture = texture_manager_->create_texture(std::move(normal_desc).value());
    if (!normal_texture) {
        return core::Status::failure(normal_texture.error().code, normal_texture.error().message);
    }
    terrain_normal_texture_array_ = normal_texture.value();
    auto surface_desc =
        surface_builder.build("terrain_surface_texture_array", TextureColorSpace::linear);
    if (!surface_desc) {
        return core::Status::failure(surface_desc.error().code, surface_desc.error().message);
    }
    auto surface_texture = texture_manager_->create_texture(std::move(surface_desc).value());
    if (!surface_texture) {
        return core::Status::failure(surface_texture.error().code, surface_texture.error().message);
    }
    terrain_surface_texture_array_ = surface_texture.value();
    const auto* texture_view = texture_manager_->find(terrain_texture_array_);
    const auto* normal_view = texture_manager_->find(terrain_normal_texture_array_);
    const auto* terrain_surface_view = texture_manager_->find(terrain_surface_texture_array_);
    if (texture_view == nullptr || normal_view == nullptr || terrain_surface_view == nullptr) {
        return core::Status::failure("renderer.terrain_texture_missing",
                                     "an aligned terrain texture array disappeared after creation");
    }

    rhi::RenderSamplerDesc sampler_desc;
    sampler_desc.min_filter = rhi::RenderSamplerFilter::linear;
    sampler_desc.mag_filter = rhi::RenderSamplerFilter::linear;
    sampler_desc.mipmap_mode = rhi::RenderSamplerMipmapMode::linear;
    sampler_desc.max_anisotropy = 8.0F;
    sampler_desc.max_lod = static_cast<float>(texture_view->mip_levels - 1U);
    sampler_desc.debug_name = "terrain_sampler";
    auto sampler = sampler_cache_->get(std::move(sampler_desc));
    if (!sampler) {
        return core::Status::failure(sampler.error().code, sampler.error().message);
    }
    terrain_sampler_ = sampler.value();

    for (auto& runtime_material : runtime_materials) {
        auto inserted = material_cache_->upsert(std::move(runtime_material));
        if (!inserted) {
            return core::Status::failure(inserted.error().code, inserted.error().message);
        }
    }
    auto material_status = material_cache_->synchronize_gpu();
    if (!material_status) {
        return material_status;
    }

    auto shader =
        shader_manager_->create_program(make_terrain_shader_program(vertex_spirv, fragment_spirv));
    if (!shader) {
        return core::Status::failure(shader.error().code, shader.error().message);
    }
    terrain_shader_program_ = shader.value();

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/terrain.vert"};
    layout.descriptors = {
        {"terrain_textures", rhi::RenderDescriptorKind::sampled_texture, 0, true,
         rhi::RenderShaderStageFlags::fragment},
        {"voxel_materials", rhi::RenderDescriptorKind::storage_buffer, 1, true,
         rhi::RenderShaderStageFlags::fragment},
        {"local_lights", rhi::RenderDescriptorKind::storage_buffer, 2, true,
         rhi::RenderShaderStageFlags::fragment},
        {"light_grid", rhi::RenderDescriptorKind::storage_buffer, 3, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_data", rhi::RenderDescriptorKind::storage_buffer, 4, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_0", rhi::RenderDescriptorKind::sampled_texture, 5, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_1", rhi::RenderDescriptorKind::sampled_texture, 6, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_2", rhi::RenderDescriptorKind::sampled_texture, 7, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_3", rhi::RenderDescriptorKind::sampled_texture, 8, true,
         rhi::RenderShaderStageFlags::fragment},
        {"environment_map", rhi::RenderDescriptorKind::sampled_texture, 9, true,
         rhi::RenderShaderStageFlags::fragment},
        {"local_shadow_0", rhi::RenderDescriptorKind::sampled_texture, 10, true,
         rhi::RenderShaderStageFlags::fragment},
        {"local_shadow_1", rhi::RenderDescriptorKind::sampled_texture, 11, true,
         rhi::RenderShaderStageFlags::fragment},
        {"terrain_normal_textures", rhi::RenderDescriptorKind::sampled_texture, 12, true,
         rhi::RenderShaderStageFlags::fragment},
        {"terrain_surface_textures", rhi::RenderDescriptorKind::sampled_texture, 13, true,
         rhi::RenderShaderStageFlags::fragment},
        {"scene_depth", rhi::RenderDescriptorKind::sampled_texture, 14, false,
         rhi::RenderShaderStageFlags::fragment},
    };
    layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    layout.debug_name = "terrain_layout";
    layout.per_frame_descriptors = true;

    rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.material_id = material.value();
    pipeline.debug_name = "opaque_terrain_pipeline";
    pipeline.vertex_stride = sizeof(terrain::GpuChunkVertex);
    pipeline.vertex_attributes.assign(terrain::gpu_chunk_vertex_attributes.begin(),
                                      terrain::gpu_chunk_vertex_attributes.end());
    pipeline.topology = rhi::RenderPrimitiveTopology::triangle_list;
    pipeline.polygon_mode = rhi::RenderPolygonMode::fill;
    pipeline.cull_mode = rhi::RenderCullMode::back;
    pipeline.front_face = rhi::RenderFrontFace::counter_clockwise;
    pipeline.depth_test_enable = true;
    pipeline.depth_write_enable = true;
    pipeline.depth_compare = rhi::RenderCompareOperation::less;
    pipeline.blend_mode = rhi::RenderBlendMode::disabled;
    pipeline.color_target_format = scene_color_format();
    pipeline.depth_target_format = rhi::RenderImageFormat::d32_sfloat;
    const auto vertex_layout =
        hash_vertex_layout(pipeline.vertex_stride, pipeline.vertex_attributes);
    const auto prewarm = [&](std::size_t index, RenderPhase phase,
                             rhi::RenderGraphicsPipelineDesc phase_pipeline)
        -> core::Result<rhi::RenderResourceHandle> {
        GraphicsPipelineKey key;
        key.shader_program = terrain_shader_program_;
        key.vertex_layout = vertex_layout;
        key.render_phase = phase;
        key.color_format = phase_pipeline.color_target_format;
        key.depth_format = phase_pipeline.depth_target_format;
        key.cull_mode = phase_pipeline.cull_mode;
        key.front_face = phase_pipeline.front_face;
        key.depth_test = phase_pipeline.depth_test_enable;
        key.depth_write = phase_pipeline.depth_write_enable;
        key.depth_compare = phase_pipeline.depth_compare;
        key.blend_mode = phase_pipeline.blend_mode;
        terrain_pipeline_keys_[index] = key;
        return pipeline_cache_->prewarm(key, layout, std::move(phase_pipeline));
    };

    auto opaque = prewarm(0, RenderPhase::opaque_terrain, pipeline);
    if (!opaque) {
        return core::Status::failure(opaque.error().code, opaque.error().message);
    }
    auto alpha_pipeline = pipeline;
    alpha_pipeline.debug_name = "alpha_tested_terrain_pipeline";
    alpha_pipeline.cull_mode = rhi::RenderCullMode::none;
    auto alpha = prewarm(1, RenderPhase::alpha_tested_terrain, std::move(alpha_pipeline));
    if (!alpha) {
        return core::Status::failure(alpha.error().code, alpha.error().message);
    }
    auto transparent_pipeline = pipeline;
    transparent_pipeline.debug_name = "transparent_terrain_pipeline";
    transparent_pipeline.depth_write_enable = false;
    transparent_pipeline.blend_mode = rhi::RenderBlendMode::alpha;
    auto transparent =
        prewarm(2, RenderPhase::transparent_terrain, std::move(transparent_pipeline));
    if (!transparent) {
        return core::Status::failure(transparent.error().code, transparent.error().message);
    }
    auto fluid_pipeline = pipeline;
    fluid_pipeline.debug_name = "fluid_terrain_pipeline";
    fluid_pipeline.depth_write_enable = false;
    fluid_pipeline.blend_mode = rhi::RenderBlendMode::alpha;
    auto fluid = prewarm(3, RenderPhase::fluid_terrain, std::move(fluid_pipeline));
    if (!fluid) {
        return core::Status::failure(fluid.error().code, fluid.error().message);
    }
    terrain_pipelines_ = {opaque.value(), alpha.value(), transparent.value(), fluid.value()};

    const std::array texture_writes{
        rhi::RenderDescriptorWrite{material.value(), "terrain_textures", texture_view->image, 0, 0,
                                   terrain_sampler_},
        rhi::RenderDescriptorWrite{material.value(), "terrain_normal_textures", normal_view->image,
                                   0, 0, terrain_sampler_},
        rhi::RenderDescriptorWrite{material.value(), "terrain_surface_textures",
                                   terrain_surface_view->image, 0, 0, terrain_sampler_},
    };
    auto texture_binding = device_->write_descriptors(texture_writes);
    if (!texture_binding) {
        return core::Status::failure(texture_binding.error().code, texture_binding.error().message);
    }
    material_status =
        material_cache_->write_gpu_table_descriptor(material.value(), "voxel_materials");
    if (!material_status) {
        return material_status;
    }
    return core::Status::ok();
}

core::Status Renderer::create_scene_pipelines(std::span<const std::uint32_t> vertex_spirv,
                                              std::span<const std::uint32_t> fragment_spirv) {
    if (shader_manager_ == nullptr || sampler_cache_ == nullptr || pipeline_cache_ == nullptr ||
        surface_texture_array_ == nullptr || material_cache_ == nullptr) {
        return core::Status::failure("renderer.runtime_assets_uninitialized",
                                     "scene runtime asset managers must be initialized first");
    }
    const auto material = core::PrototypeId::parse("base:materials/static_instances");
    const auto transparent_material =
        core::PrototypeId::parse("base:materials/transparent_instances");
    if (!material || !transparent_material) {
        return core::Status::failure("renderer.invalid_scene_material",
                                     "internal static-instance material id is invalid");
    }
    auto shader = shader_manager_->create_program(
        make_static_mesh_shader_program(vertex_spirv, fragment_spirv));
    if (!shader) {
        return core::Status::failure(shader.error().code, shader.error().message);
    }
    scene_shader_program_ = shader.value();

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/static_mesh.vert"};
    layout.descriptors = {
        {"object_instances", rhi::RenderDescriptorKind::storage_buffer, 0, true,
         rhi::RenderShaderStageFlags::vertex},
        {"skin_matrices", rhi::RenderDescriptorKind::storage_buffer, 1, true,
         rhi::RenderShaderStageFlags::vertex},
        {"surface_textures", rhi::RenderDescriptorKind::sampled_texture, 2, true,
         rhi::RenderShaderStageFlags::fragment},
        {"surface_materials", rhi::RenderDescriptorKind::storage_buffer, 3, true,
         rhi::RenderShaderStageFlags::fragment},
        {"surface_data_textures", rhi::RenderDescriptorKind::sampled_texture, 4, true,
         rhi::RenderShaderStageFlags::fragment},
        {"morph_deltas", rhi::RenderDescriptorKind::storage_buffer, 5, true,
         rhi::RenderShaderStageFlags::vertex},
        {"morph_weights", rhi::RenderDescriptorKind::storage_buffer, 6, true,
         rhi::RenderShaderStageFlags::vertex},
        {"local_lights", rhi::RenderDescriptorKind::storage_buffer, 7, true,
         rhi::RenderShaderStageFlags::fragment},
        {"light_grid", rhi::RenderDescriptorKind::storage_buffer, 8, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_data", rhi::RenderDescriptorKind::storage_buffer, 9, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_0", rhi::RenderDescriptorKind::sampled_texture, 10, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_1", rhi::RenderDescriptorKind::sampled_texture, 11, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_2", rhi::RenderDescriptorKind::sampled_texture, 12, true,
         rhi::RenderShaderStageFlags::fragment},
        {"shadow_cascade_3", rhi::RenderDescriptorKind::sampled_texture, 13, true,
         rhi::RenderShaderStageFlags::fragment},
        {"environment_map", rhi::RenderDescriptorKind::sampled_texture, 14, true,
         rhi::RenderShaderStageFlags::fragment},
        {"local_shadow_0", rhi::RenderDescriptorKind::sampled_texture, 15, true,
         rhi::RenderShaderStageFlags::fragment},
        {"local_shadow_1", rhi::RenderDescriptorKind::sampled_texture, 16, true,
         rhi::RenderShaderStageFlags::fragment},
        {"scene_depth", rhi::RenderDescriptorKind::sampled_texture, 17, true,
         rhi::RenderShaderStageFlags::fragment},
    };
    layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    layout.debug_name = "static_instances_layout";
    layout.per_frame_descriptors = true;
    auto transparent_layout = layout;
    transparent_layout.material_id = transparent_material.value();
    transparent_layout.debug_name = "transparent_instances_layout";

    rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.material_id = material.value();
    pipeline.vertex_stride = sizeof(GpuStaticMeshVertex);
    pipeline.vertex_attributes.assign(std::begin(gpu_static_mesh_vertex_attributes),
                                      std::end(gpu_static_mesh_vertex_attributes));
    pipeline.topology = rhi::RenderPrimitiveTopology::triangle_list;
    pipeline.polygon_mode = rhi::RenderPolygonMode::fill;
    pipeline.cull_mode = rhi::RenderCullMode::back;
    pipeline.front_face = rhi::RenderFrontFace::counter_clockwise;
    pipeline.depth_test_enable = true;
    pipeline.depth_write_enable = true;
    pipeline.depth_compare = rhi::RenderCompareOperation::less;
    pipeline.blend_mode = rhi::RenderBlendMode::disabled;
    pipeline.color_target_format = scene_color_format();
    pipeline.depth_target_format = rhi::RenderImageFormat::d32_sfloat;
    const auto* surface_texture = surface_texture_array_->texture_view();
    if (surface_texture == nullptr) {
        return core::Status::failure("renderer.surface_texture_missing",
                                     "surface texture array disappeared after creation");
    }
    rhi::RenderSamplerDesc sampler_desc;
    sampler_desc.min_filter = rhi::RenderSamplerFilter::linear;
    sampler_desc.mag_filter = rhi::RenderSamplerFilter::linear;
    sampler_desc.mipmap_mode = rhi::RenderSamplerMipmapMode::linear;
    sampler_desc.address_u = rhi::RenderSamplerAddressMode::repeat;
    sampler_desc.address_v = rhi::RenderSamplerAddressMode::repeat;
    sampler_desc.max_lod = static_cast<float>(surface_texture->mip_levels - 1U);
    sampler_desc.debug_name = "surface_sampler";
    auto sampler = sampler_cache_->get(std::move(sampler_desc));
    if (!sampler) {
        return core::Status::failure(sampler.error().code, sampler.error().message);
    }
    surface_sampler_ = sampler.value();
    const auto vertex_layout =
        hash_vertex_layout(pipeline.vertex_stride, pipeline.vertex_attributes);
    const auto prewarm =
        [&](std::size_t index, RenderPhase phase, const rhi::RenderPipelineLayoutDesc& phase_layout,
            rhi::RenderGraphicsPipelineDesc desc) -> core::Result<rhi::RenderResourceHandle> {
        GraphicsPipelineKey key;
        key.shader_program = scene_shader_program_;
        key.vertex_layout = vertex_layout;
        key.render_phase = phase;
        key.color_format = desc.color_target_format;
        key.depth_format = desc.depth_target_format;
        key.cull_mode = desc.cull_mode;
        key.front_face = desc.front_face;
        key.depth_test = desc.depth_test_enable;
        key.depth_write = desc.depth_write_enable;
        key.depth_compare = desc.depth_compare;
        key.blend_mode = desc.blend_mode;
        scene_pipeline_keys_[index] = key;
        return pipeline_cache_->prewarm(key, phase_layout, std::move(desc));
    };
    pipeline.debug_name = "opaque_static_instances_pipeline";
    auto opaque = prewarm(0, RenderPhase::static_instances, layout, pipeline);
    if (!opaque) {
        return core::Status::failure(opaque.error().code, opaque.error().message);
    }
    auto alpha_desc = pipeline;
    alpha_desc.debug_name = "alpha_tested_static_instances_pipeline";
    alpha_desc.cull_mode = rhi::RenderCullMode::none;
    auto alpha = prewarm(1, RenderPhase::static_instances, layout, std::move(alpha_desc));
    if (!alpha) {
        return core::Status::failure(alpha.error().code, alpha.error().message);
    }
    auto transparent_desc = pipeline;
    transparent_desc.material_id = transparent_material.value();
    transparent_desc.debug_name = "transparent_static_instances_pipeline";
    transparent_desc.depth_write_enable = false;
    transparent_desc.blend_mode = rhi::RenderBlendMode::alpha;
    auto transparent = prewarm(2, RenderPhase::transparent_terrain, transparent_layout,
                               std::move(transparent_desc));
    if (!transparent) {
        return core::Status::failure(transparent.error().code, transparent.error().message);
    }
    auto additive_desc = pipeline;
    additive_desc.material_id = transparent_material.value();
    additive_desc.debug_name = "additive_static_instances_pipeline";
    additive_desc.depth_write_enable = false;
    additive_desc.blend_mode = rhi::RenderBlendMode::additive;
    auto additive =
        prewarm(3, RenderPhase::transparent_terrain, transparent_layout, std::move(additive_desc));
    if (!additive) {
        return core::Status::failure(additive.error().code, additive.error().message);
    }
    auto premultiplied_desc = pipeline;
    premultiplied_desc.material_id = transparent_material.value();
    premultiplied_desc.debug_name = "premultiplied_static_instances_pipeline";
    premultiplied_desc.depth_write_enable = false;
    premultiplied_desc.blend_mode = rhi::RenderBlendMode::premultiplied_alpha;
    auto premultiplied = prewarm(4, RenderPhase::transparent_terrain, transparent_layout,
                                 std::move(premultiplied_desc));
    if (!premultiplied) {
        return core::Status::failure(premultiplied.error().code, premultiplied.error().message);
    }
    auto two_sided_desc = pipeline;
    two_sided_desc.debug_name = "opaque_two_sided_static_instances_pipeline";
    two_sided_desc.cull_mode = rhi::RenderCullMode::none;
    auto opaque_two_sided =
        prewarm(5, RenderPhase::static_instances, layout, std::move(two_sided_desc));
    if (!opaque_two_sided) {
        return core::Status::failure(opaque_two_sided.error().code,
                                     opaque_two_sided.error().message);
    }
    auto alpha_two_sided_desc = pipeline;
    alpha_two_sided_desc.debug_name = "alpha_tested_two_sided_static_instances_pipeline";
    alpha_two_sided_desc.cull_mode = rhi::RenderCullMode::none;
    auto alpha_two_sided =
        prewarm(6, RenderPhase::static_instances, layout, std::move(alpha_two_sided_desc));
    if (!alpha_two_sided) {
        return core::Status::failure(alpha_two_sided.error().code, alpha_two_sided.error().message);
    }
    auto transparent_two_sided_desc = pipeline;
    transparent_two_sided_desc.material_id = transparent_material.value();
    transparent_two_sided_desc.debug_name = "transparent_two_sided_static_instances_pipeline";
    transparent_two_sided_desc.cull_mode = rhi::RenderCullMode::none;
    transparent_two_sided_desc.depth_write_enable = false;
    transparent_two_sided_desc.blend_mode = rhi::RenderBlendMode::alpha;
    auto transparent_two_sided = prewarm(7, RenderPhase::transparent_terrain, transparent_layout,
                                         std::move(transparent_two_sided_desc));
    if (!transparent_two_sided) {
        return core::Status::failure(transparent_two_sided.error().code,
                                     transparent_two_sided.error().message);
    }
    auto additive_two_sided_desc = pipeline;
    additive_two_sided_desc.material_id = transparent_material.value();
    additive_two_sided_desc.debug_name = "additive_two_sided_static_instances_pipeline";
    additive_two_sided_desc.cull_mode = rhi::RenderCullMode::none;
    additive_two_sided_desc.depth_write_enable = false;
    additive_two_sided_desc.blend_mode = rhi::RenderBlendMode::additive;
    auto additive_two_sided = prewarm(8, RenderPhase::transparent_terrain, transparent_layout,
                                      std::move(additive_two_sided_desc));
    if (!additive_two_sided) {
        return core::Status::failure(additive_two_sided.error().code,
                                     additive_two_sided.error().message);
    }
    auto premultiplied_two_sided_desc = pipeline;
    premultiplied_two_sided_desc.material_id = transparent_material.value();
    premultiplied_two_sided_desc.debug_name = "premultiplied_two_sided_static_instances_pipeline";
    premultiplied_two_sided_desc.cull_mode = rhi::RenderCullMode::none;
    premultiplied_two_sided_desc.depth_write_enable = false;
    premultiplied_two_sided_desc.blend_mode = rhi::RenderBlendMode::premultiplied_alpha;
    auto premultiplied_two_sided = prewarm(9, RenderPhase::transparent_terrain, transparent_layout,
                                           std::move(premultiplied_two_sided_desc));
    if (!premultiplied_two_sided) {
        return core::Status::failure(premultiplied_two_sided.error().code,
                                     premultiplied_two_sided.error().message);
    }
    scene_pipelines_ = {opaque.value(),
                        alpha.value(),
                        transparent.value(),
                        additive.value(),
                        premultiplied.value(),
                        opaque_two_sided.value(),
                        alpha_two_sided.value(),
                        transparent_two_sided.value(),
                        additive_two_sided.value(),
                        premultiplied_two_sided.value()};
    return bind_scene_surface_resources();
}

core::Status
Renderer::create_shadow_pipelines(std::span<const std::uint32_t> terrain_vertex_spirv,
                                  std::span<const std::uint32_t> terrain_fragment_spirv,
                                  std::span<const std::uint32_t> static_vertex_spirv,
                                  std::span<const std::uint32_t> static_fragment_spirv) {
    const auto terrain_material = core::PrototypeId::parse("base:materials/terrain_shadow");
    const auto static_material = core::PrototypeId::parse("base:materials/static_shadow");
    if (!terrain_material || !static_material) {
        return core::Status::failure("renderer.invalid_shadow_material",
                                     "internal shadow material ids are invalid");
    }
    auto terrain_shader = shader_manager_->create_program(
        make_terrain_shadow_shader_program(terrain_vertex_spirv, terrain_fragment_spirv));
    if (!terrain_shader) {
        return core::Status::failure(terrain_shader.error().code, terrain_shader.error().message);
    }
    terrain_shadow_shader_program_ = terrain_shader.value();
    auto static_shader = shader_manager_->create_program(
        make_static_shadow_shader_program(static_vertex_spirv, static_fragment_spirv));
    if (!static_shader) {
        return core::Status::failure(static_shader.error().code, static_shader.error().message);
    }
    static_shadow_shader_program_ = static_shader.value();

    rhi::RenderPipelineLayoutDesc terrain_layout;
    terrain_layout.material_id = terrain_material.value();
    terrain_layout.shader_template = {"base", "shaders/shadow_terrain.frag"};
    terrain_layout.descriptors = {
        {"terrain_textures", rhi::RenderDescriptorKind::sampled_texture, 0, true,
         rhi::RenderShaderStageFlags::fragment},
        {"voxel_materials", rhi::RenderDescriptorKind::storage_buffer, 1, true,
         rhi::RenderShaderStageFlags::fragment},
    };
    terrain_layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    terrain_layout.debug_name = "terrain_shadow_layout";

    rhi::RenderGraphicsPipelineDesc terrain_pipeline;
    terrain_pipeline.material_id = terrain_material.value();
    terrain_pipeline.debug_name = "terrain_shadow_pipeline";
    terrain_pipeline.vertex_stride = sizeof(terrain::GpuChunkVertex);
    terrain_pipeline.vertex_attributes.assign(terrain::gpu_chunk_vertex_attributes.begin(),
                                              terrain::gpu_chunk_vertex_attributes.end());
    terrain_pipeline.cull_mode = rhi::RenderCullMode::back;
    terrain_pipeline.depth_test_enable = true;
    terrain_pipeline.depth_write_enable = true;
    terrain_pipeline.depth_compare = rhi::RenderCompareOperation::less_or_equal;
    terrain_pipeline.color_target_format = scene_color_format();
    terrain_pipeline.depth_target_format = rhi::RenderImageFormat::d32_sfloat;
    terrain_pipeline.color_write_enable = false;
    const auto terrain_vertex_layout =
        hash_vertex_layout(terrain_pipeline.vertex_stride, terrain_pipeline.vertex_attributes);
    const auto prewarm =
        [&](std::size_t index, ShaderProgramHandle shader_program,
            const rhi::RenderPipelineLayoutDesc& layout,
            rhi::RenderGraphicsPipelineDesc pipeline) -> core::Result<rhi::RenderResourceHandle> {
        GraphicsPipelineKey key;
        key.shader_program = shader_program;
        key.vertex_layout = hash_vertex_layout(pipeline.vertex_stride, pipeline.vertex_attributes);
        key.render_phase = RenderPhase::shadow;
        key.color_format = pipeline.color_target_format;
        key.depth_format = pipeline.depth_target_format;
        key.cull_mode = pipeline.cull_mode;
        key.front_face = pipeline.front_face;
        key.depth_test = pipeline.depth_test_enable;
        key.depth_write = pipeline.depth_write_enable;
        key.depth_compare = pipeline.depth_compare;
        key.blend_mode = pipeline.blend_mode;
        key.color_write = false;
        shadow_pipeline_keys_[index] = key;
        return pipeline_cache_->prewarm(key, layout, std::move(pipeline));
    };
    (void)terrain_vertex_layout;
    auto terrain_opaque =
        prewarm(0, terrain_shadow_shader_program_, terrain_layout, terrain_pipeline);
    if (!terrain_opaque) {
        return core::Status::failure(terrain_opaque.error().code, terrain_opaque.error().message);
    }
    auto terrain_alpha_desc = terrain_pipeline;
    terrain_alpha_desc.debug_name = "terrain_alpha_shadow_pipeline";
    terrain_alpha_desc.cull_mode = rhi::RenderCullMode::none;
    auto terrain_alpha =
        prewarm(1, terrain_shadow_shader_program_, terrain_layout, terrain_alpha_desc);
    if (!terrain_alpha) {
        return core::Status::failure(terrain_alpha.error().code, terrain_alpha.error().message);
    }

    rhi::RenderPipelineLayoutDesc static_layout;
    static_layout.material_id = static_material.value();
    static_layout.shader_template = {"base", "shaders/shadow_static.frag"};
    static_layout.descriptors = {
        {"object_instances", rhi::RenderDescriptorKind::storage_buffer, 0, true,
         rhi::RenderShaderStageFlags::vertex},
        {"skin_matrices", rhi::RenderDescriptorKind::storage_buffer, 1, true,
         rhi::RenderShaderStageFlags::vertex},
        {"surface_textures", rhi::RenderDescriptorKind::sampled_texture, 2, true,
         rhi::RenderShaderStageFlags::fragment},
        {"surface_materials", rhi::RenderDescriptorKind::storage_buffer, 3, true,
         rhi::RenderShaderStageFlags::fragment},
        {"surface_data_textures", rhi::RenderDescriptorKind::sampled_texture, 4, true,
         rhi::RenderShaderStageFlags::fragment},
        {"morph_deltas", rhi::RenderDescriptorKind::storage_buffer, 5, true,
         rhi::RenderShaderStageFlags::vertex},
        {"morph_weights", rhi::RenderDescriptorKind::storage_buffer, 6, true,
         rhi::RenderShaderStageFlags::vertex},
    };
    static_layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    static_layout.debug_name = "static_shadow_layout";

    rhi::RenderGraphicsPipelineDesc static_pipeline = terrain_pipeline;
    static_pipeline.material_id = static_material.value();
    static_pipeline.debug_name = "static_shadow_pipeline";
    static_pipeline.vertex_stride = sizeof(GpuStaticMeshVertex);
    static_pipeline.vertex_attributes.assign(std::begin(gpu_static_mesh_vertex_attributes),
                                             std::end(gpu_static_mesh_vertex_attributes));
    auto static_opaque = prewarm(2, static_shadow_shader_program_, static_layout, static_pipeline);
    if (!static_opaque) {
        return core::Status::failure(static_opaque.error().code, static_opaque.error().message);
    }
    auto static_two_sided_desc = static_pipeline;
    static_two_sided_desc.debug_name = "static_two_sided_shadow_pipeline";
    static_two_sided_desc.cull_mode = rhi::RenderCullMode::none;
    auto static_two_sided =
        prewarm(3, static_shadow_shader_program_, static_layout, static_two_sided_desc);
    if (!static_two_sided) {
        return core::Status::failure(static_two_sided.error().code,
                                     static_two_sided.error().message);
    }
    shadow_pipelines_ = {terrain_opaque.value(), terrain_alpha.value(), static_opaque.value(),
                         static_two_sided.value()};

    const auto* terrain_texture = texture_manager_->find(terrain_texture_array_);
    if (terrain_texture == nullptr) {
        return core::Status::failure("renderer.terrain_texture_missing",
                                     "terrain shadow texture array is unavailable");
    }
    const rhi::RenderDescriptorWrite texture_write{
        terrain_material.value(), "terrain_textures", terrain_texture->image, 0, 0,
        terrain_sampler_};
    auto written = device_->write_descriptors(std::span{&texture_write, 1});
    if (!written) {
        return core::Status::failure(written.error().code, written.error().message);
    }
    return material_cache_->write_gpu_table_descriptor(terrain_material.value(), "voxel_materials");
}

core::Status Renderer::bind_shadow_resources() {
    const auto material = core::PrototypeId::parse("base:materials/static_shadow");
    if (!material || scene_render_system_ == nullptr || mesh_manager_ == nullptr ||
        surface_texture_array_ == nullptr || surface_data_texture_array_ == nullptr) {
        return core::Status::failure("renderer.shadow_resources_uninitialized",
                                     "static shadow resources are unavailable");
    }
    const auto* color = surface_texture_array_->texture_view();
    const auto* data = surface_data_texture_array_->texture_view();
    if (color == nullptr || data == nullptr) {
        return core::Status::failure("renderer.shadow_texture_missing",
                                     "static shadow texture arrays are unavailable");
    }
    const std::array writes{
        rhi::RenderDescriptorWrite{
            material.value(), "object_instances", scene_render_system_->instance_buffer(), 0,
            static_cast<std::size_t>(scene_render_system_->stats().instance_buffer_bytes)},
        rhi::RenderDescriptorWrite{
            material.value(), "skin_matrices", scene_render_system_->skin_matrix_buffer(), 0,
            static_cast<std::size_t>(scene_render_system_->stats().skin_matrix_buffer_bytes)},
        rhi::RenderDescriptorWrite{material.value(), "surface_textures", color->image, 0, 0,
                                   surface_sampler_},
        rhi::RenderDescriptorWrite{material.value(), "surface_data_textures", data->image, 0, 0,
                                   surface_sampler_},
        rhi::RenderDescriptorWrite{
            material.value(), "morph_deltas", mesh_manager_->morph_delta_buffer(), 0,
            static_cast<std::size_t>(mesh_manager_->stats().morph_arena.capacity_bytes)},
        rhi::RenderDescriptorWrite{
            material.value(), "morph_weights", scene_render_system_->morph_weight_buffer(), 0,
            static_cast<std::size_t>(scene_render_system_->stats().morph_weight_buffer_bytes)},
    };
    auto written = device_->write_descriptors(writes);
    if (!written) {
        return core::Status::failure(written.error().code, written.error().message);
    }
    return material_cache_->write_gpu_surface_table_descriptor(material.value(),
                                                               "surface_materials");
}

core::Status Renderer::bind_scene_surface_resources() {
    if (device_ == nullptr || surface_texture_array_ == nullptr ||
        surface_data_texture_array_ == nullptr || material_cache_ == nullptr ||
        !surface_sampler_.is_valid()) {
        return core::Status::failure("renderer.scene_surface_resources_uninitialized",
                                     "scene surface resources must be initialized first");
    }
    const auto scene_material = core::PrototypeId::parse("base:materials/static_instances");
    const auto transparent_material =
        core::PrototypeId::parse("base:materials/transparent_instances");
    const auto* texture = surface_texture_array_->texture_view();
    const auto* data_texture = surface_data_texture_array_->texture_view();
    const auto* fallback_depth = texture_manager_->find(texture_manager_->white_texture());
    if (!scene_material || !transparent_material || texture == nullptr ||
        !texture->image.is_valid() || data_texture == nullptr || !data_texture->image.is_valid() ||
        fallback_depth == nullptr || !fallback_depth->image.is_valid()) {
        return core::Status::failure("renderer.scene_surface_resources_missing",
                                     "scene surface texture or material identity is missing");
    }
    for (const auto& material : {scene_material.value(), transparent_material.value()}) {
        const std::array texture_writes{
            rhi::RenderDescriptorWrite{material, "surface_textures", texture->image, 0, 0,
                                       surface_sampler_},
            rhi::RenderDescriptorWrite{material, "surface_data_textures", data_texture->image, 0, 0,
                                       surface_sampler_},
            rhi::RenderDescriptorWrite{material, "scene_depth", fallback_depth->image, 0, 0,
                                       surface_sampler_},
        };
        auto written = device_->write_descriptors(texture_writes);
        if (!written) {
            return core::Status::failure(written.error().code, written.error().message);
        }
        auto material_status =
            material_cache_->write_gpu_surface_table_descriptor(material, "surface_materials");
        if (!material_status) {
            return material_status;
        }
    }
    return core::Status::ok();
}

core::Status Renderer::create_debug_pipelines(std::span<const std::uint32_t> vertex_spirv,
                                              std::span<const std::uint32_t> fragment_spirv) {
    if (shader_manager_ == nullptr || pipeline_cache_ == nullptr) {
        return core::Status::failure("renderer.runtime_assets_uninitialized",
                                     "debug runtime asset managers must be initialized first");
    }
    const auto material = core::PrototypeId::parse("base:materials/debug_lines");
    if (!material) {
        return core::Status::failure("renderer.invalid_debug_material",
                                     "internal debug-line material id is invalid");
    }
    auto shader =
        shader_manager_->create_program(make_debug_shader_program(vertex_spirv, fragment_spirv));
    if (!shader) {
        return core::Status::failure(shader.error().code, shader.error().message);
    }
    debug_shader_program_ = shader.value();

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/debug_line.vert"};
    layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    layout.debug_name = "debug_lines_layout";

    rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.material_id = material.value();
    pipeline.vertex_stride = sizeof(GpuDebugVertex);
    pipeline.vertex_attributes.assign(std::begin(gpu_debug_vertex_attributes),
                                      std::end(gpu_debug_vertex_attributes));
    pipeline.topology = rhi::RenderPrimitiveTopology::line_list;
    pipeline.polygon_mode = rhi::RenderPolygonMode::fill;
    pipeline.cull_mode = rhi::RenderCullMode::none;
    pipeline.front_face = rhi::RenderFrontFace::counter_clockwise;
    pipeline.depth_test_enable = true;
    pipeline.depth_write_enable = false;
    pipeline.depth_compare = rhi::RenderCompareOperation::less_or_equal;
    pipeline.blend_mode = rhi::RenderBlendMode::alpha;
    pipeline.color_target_format = scene_color_format();
    pipeline.depth_target_format = rhi::RenderImageFormat::d32_sfloat;
    const auto vertex_layout =
        hash_vertex_layout(pipeline.vertex_stride, pipeline.vertex_attributes);
    const auto prewarm =
        [&](std::size_t index,
            rhi::RenderGraphicsPipelineDesc desc) -> core::Result<rhi::RenderResourceHandle> {
        GraphicsPipelineKey key;
        key.shader_program = debug_shader_program_;
        key.vertex_layout = vertex_layout;
        key.render_phase = RenderPhase::debug;
        key.color_format = desc.color_target_format;
        key.depth_format = desc.depth_target_format;
        key.cull_mode = desc.cull_mode;
        key.front_face = desc.front_face;
        key.depth_test = desc.depth_test_enable;
        key.depth_write = desc.depth_write_enable;
        key.depth_compare = desc.depth_compare;
        key.blend_mode = desc.blend_mode;
        debug_pipeline_keys_[index] = key;
        return pipeline_cache_->prewarm(key, layout, std::move(desc));
    };
    pipeline.debug_name = "depth_tested_debug_lines_pipeline";
    auto depth = prewarm(0, pipeline);
    if (!depth) {
        return core::Status::failure(depth.error().code, depth.error().message);
    }
    pipeline.debug_name = "overlay_debug_lines_pipeline";
    pipeline.depth_compare = rhi::RenderCompareOperation::always;
    auto overlay = prewarm(1, std::move(pipeline));
    if (!overlay) {
        return core::Status::failure(overlay.error().code, overlay.error().message);
    }
    debug_pipelines_ = {depth.value(), overlay.value()};
    return core::Status::ok();
}

core::Status Renderer::create_ui_pipeline(std::span<const std::uint32_t> vertex_spirv,
                                          std::span<const std::uint32_t> fragment_spirv) {
    if (shader_manager_ == nullptr || pipeline_cache_ == nullptr || texture_manager_ == nullptr ||
        sampler_cache_ == nullptr) {
        return core::Status::failure("renderer.runtime_assets_uninitialized",
                                     "UI runtime asset managers must be initialized first");
    }
    const auto material = core::PrototypeId::parse("base:materials/ui");
    if (!material) {
        return core::Status::failure("renderer.invalid_ui_material",
                                     "internal UI material id is invalid");
    }
    TextureUploadDesc atlas_desc;
    atlas_desc.id = "builtin:ui_atlas";
    atlas_desc.width = 128;
    atlas_desc.height = 64;
    atlas_desc.array_layers = 2;
    atlas_desc.color_space = TextureColorSpace::srgb;
    atlas_desc.generate_mipmaps = true;
    atlas_desc.rgba8 = make_ui_atlas();
    auto atlas = texture_manager_->create_texture(std::move(atlas_desc));
    if (!atlas) {
        return core::Status::failure(atlas.error().code, atlas.error().message);
    }
    ui_texture_atlas_ = atlas.value();
    const auto* atlas_view = texture_manager_->find(ui_texture_atlas_);
    if (atlas_view == nullptr) {
        return core::Status::failure("renderer.ui_atlas_missing",
                                     "UI atlas disappeared after creation");
    }
    rhi::RenderSamplerDesc ui_sampler_desc;
    ui_sampler_desc.min_filter = rhi::RenderSamplerFilter::nearest;
    ui_sampler_desc.mag_filter = rhi::RenderSamplerFilter::nearest;
    ui_sampler_desc.mipmap_mode = rhi::RenderSamplerMipmapMode::nearest;
    ui_sampler_desc.max_lod = static_cast<float>(atlas_view->mip_levels - 1U);
    ui_sampler_desc.debug_name = "ui_atlas_sampler";
    auto ui_sampler = sampler_cache_->get(std::move(ui_sampler_desc));
    if (!ui_sampler) {
        return core::Status::failure(ui_sampler.error().code, ui_sampler.error().message);
    }
    ui_sampler_ = ui_sampler.value();
    auto shader =
        shader_manager_->create_program(make_ui_shader_program(vertex_spirv, fragment_spirv));
    if (!shader) {
        return core::Status::failure(shader.error().code, shader.error().message);
    }
    ui_shader_program_ = shader.value();

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/ui.vert"};
    layout.descriptors = {
        {"ui_atlas", rhi::RenderDescriptorKind::sampled_texture, 0, true,
         rhi::RenderShaderStageFlags::fragment},
    };
    layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    layout.debug_name = "ui_layout";

    rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.material_id = material.value();
    pipeline.debug_name = "ui_pipeline";
    pipeline.vertex_stride = sizeof(GpuUiVertex);
    pipeline.vertex_attributes.assign(gpu_ui_vertex_attributes.begin(),
                                      gpu_ui_vertex_attributes.end());
    pipeline.topology = rhi::RenderPrimitiveTopology::triangle_list;
    pipeline.polygon_mode = rhi::RenderPolygonMode::fill;
    pipeline.cull_mode = rhi::RenderCullMode::none;
    pipeline.front_face = rhi::RenderFrontFace::counter_clockwise;
    // UI composites onto the tone mapped image, which carries no depth attachment. Depth testing
    // was only ever enabled here to stay compatible with the old shared render pass.
    pipeline.depth_test_enable = false;
    pipeline.depth_write_enable = false;
    pipeline.blend_mode = rhi::RenderBlendMode::alpha;
    pipeline.color_target_format = rhi::RenderImageFormat::rgba8_unorm;
    pipeline.depth_target_format = rhi::RenderImageFormat::d32_sfloat;
    ui_pipeline_key_.shader_program = ui_shader_program_;
    ui_pipeline_key_.vertex_layout =
        hash_vertex_layout(pipeline.vertex_stride, pipeline.vertex_attributes);
    ui_pipeline_key_.render_phase = RenderPhase::ui;
    ui_pipeline_key_.color_format = pipeline.color_target_format;
    ui_pipeline_key_.depth_format = pipeline.depth_target_format;
    ui_pipeline_key_.cull_mode = pipeline.cull_mode;
    ui_pipeline_key_.front_face = pipeline.front_face;
    ui_pipeline_key_.depth_test = pipeline.depth_test_enable;
    ui_pipeline_key_.depth_write = pipeline.depth_write_enable;
    ui_pipeline_key_.depth_compare = pipeline.depth_compare;
    ui_pipeline_key_.blend_mode = pipeline.blend_mode;
    auto created = pipeline_cache_->prewarm(ui_pipeline_key_, layout, std::move(pipeline));
    if (!created) {
        return core::Status::failure(created.error().code, created.error().message);
    }
    ui_pipeline_ = created.value();
    const rhi::RenderDescriptorWrite atlas_write{
        material.value(), "ui_atlas", atlas_view->image, 0, 0, ui_sampler_};
    auto binding =
        device_->write_descriptors(std::span<const rhi::RenderDescriptorWrite>{&atlas_write, 1});
    if (!binding) {
        return core::Status::failure(binding.error().code, binding.error().message);
    }
    return core::Status::ok();
}

core::Status
Renderer::create_image_quality_pipelines(std::span<const std::uint32_t> vertex_spirv,
                                         std::span<const std::uint32_t> ssao_fragment_spirv,
                                         std::span<const std::uint32_t> ao_fragment_spirv,
                                         std::span<const std::uint32_t> fxaa_fragment_spirv,
                                         std::span<const std::uint32_t> bloom_fragment_spirv) {
    struct PostDesc {
        const char* name;
        std::span<const std::uint32_t> fragment;
        std::vector<rhi::RenderDescriptorBinding> descriptors;
        rhi::RenderImageFormat format;
    };
    std::array<PostDesc, 4> posts{{
        {"ssao",
         ssao_fragment_spirv,
         {{"scene_depth", rhi::RenderDescriptorKind::sampled_texture, 0, true,
           rhi::RenderShaderStageFlags::fragment}},
         rhi::RenderImageFormat::r8_unorm},
        {"ao_composite",
         ao_fragment_spirv,
         {{"scene_hdr", rhi::RenderDescriptorKind::sampled_texture, 0, true,
           rhi::RenderShaderStageFlags::fragment},
          {"scene_ao", rhi::RenderDescriptorKind::sampled_texture, 1, true,
           rhi::RenderShaderStageFlags::fragment}},
         rhi::RenderImageFormat::rgba16_sfloat},
        {"fxaa",
         fxaa_fragment_spirv,
         {{"input_hdr", rhi::RenderDescriptorKind::sampled_texture, 0, true,
           rhi::RenderShaderStageFlags::fragment}},
         rhi::RenderImageFormat::rgba16_sfloat},
        {"bloom",
         bloom_fragment_spirv,
         {{"input_hdr", rhi::RenderDescriptorKind::sampled_texture, 0, true,
           rhi::RenderShaderStageFlags::fragment}},
         rhi::RenderImageFormat::rgba16_sfloat},
    }};
    for (std::size_t index = 0; index < posts.size(); ++index) {
        auto& post = posts[index];
        const auto material = core::PrototypeId::parse("base:materials/" + std::string(post.name));
        if (!material) {
            return core::Status::failure("renderer.invalid_post_material",
                                         "internal post-process material id is invalid");
        }
        auto shader = shader_manager_->create_program(
            make_post_shader_program(post.name, vertex_spirv, post.fragment, post.descriptors));
        if (!shader) {
            return core::Status::failure(shader.error().code, shader.error().message);
        }
        image_quality_shader_programs_[index] = shader.value();
        rhi::RenderPipelineLayoutDesc layout;
        layout.material_id = material.value();
        layout.shader_template = {"base", "shaders/" + std::string(post.name) + ".frag"};
        layout.descriptors = post.descriptors;
        layout.debug_name = std::string(post.name) + "_layout";
        layout.per_frame_descriptors = true;

        rhi::RenderGraphicsPipelineDesc pipeline;
        pipeline.material_id = material.value();
        pipeline.debug_name = std::string(post.name) + "_pipeline";
        pipeline.vertex_stride = 0;
        pipeline.cull_mode = rhi::RenderCullMode::none;
        pipeline.depth_test_enable = false;
        pipeline.depth_write_enable = false;
        pipeline.color_target_format = post.format;
        if (index == 0U) {
            pipeline.additional_color_target_formats = {rhi::RenderImageFormat::rg16_sfloat};
        }

        GraphicsPipelineKey key;
        key.shader_program = shader.value();
        key.vertex_layout =
            hash_vertex_layout(0, std::span<const rhi::RenderVertexAttributeDesc>{});
        key.render_phase = RenderPhase::post_process;
        key.color_format = post.format;
        key.additional_color_formats = pipeline.additional_color_target_formats;
        key.depth_format = pipeline.depth_target_format;
        key.cull_mode = pipeline.cull_mode;
        key.front_face = pipeline.front_face;
        key.depth_test = false;
        key.depth_write = false;
        key.depth_compare = pipeline.depth_compare;
        key.blend_mode = pipeline.blend_mode;
        image_quality_pipeline_keys_[index] = key;
        auto created = pipeline_cache_->prewarm(key, layout, std::move(pipeline));
        if (!created) {
            return core::Status::failure(created.error().code, created.error().message);
        }
        image_quality_pipelines_[index] = created.value();
    }
    return core::Status::ok();
}

core::Status Renderer::create_tone_map_pipeline(std::span<const std::uint32_t> vertex_spirv,
                                                std::span<const std::uint32_t> fragment_spirv) {
    if (shader_manager_ == nullptr || pipeline_cache_ == nullptr) {
        return core::Status::failure("renderer.runtime_assets_uninitialized",
                                     "tone map runtime asset managers must be initialized first");
    }
    const auto material = core::PrototypeId::parse("base:materials/tone_map");
    if (!material) {
        return core::Status::failure("renderer.invalid_tone_map_material",
                                     "internal tone map material id is invalid");
    }
    auto shader =
        shader_manager_->create_program(make_tone_map_shader_program(vertex_spirv, fragment_spirv));
    if (!shader) {
        return core::Status::failure(shader.error().code, shader.error().message);
    }
    tone_map_shader_program_ = shader.value();

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/tone_map.vert"};
    layout.descriptors = {
        {"scene_hdr", rhi::RenderDescriptorKind::sampled_texture, 0, true,
         rhi::RenderShaderStageFlags::fragment},
        {"bloom_hdr", rhi::RenderDescriptorKind::sampled_texture, 1, true,
         rhi::RenderShaderStageFlags::fragment},
    };
    layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::fragment, 0, sizeof(rhi::ToneMapPushConstants)});
    layout.debug_name = "tone_map_layout";
    // The scene target this samples is a frame graph resource, so the bound image changes every
    // frame and the descriptor set cannot be shared across frames in flight.
    layout.per_frame_descriptors = true;

    rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.material_id = material.value();
    pipeline.debug_name = "tone_map_pipeline";
    // No vertex buffer is bound; the vertex shader builds the triangle from gl_VertexIndex.
    pipeline.vertex_stride = 0;
    pipeline.topology = rhi::RenderPrimitiveTopology::triangle_list;
    pipeline.polygon_mode = rhi::RenderPolygonMode::fill;
    pipeline.cull_mode = rhi::RenderCullMode::none;
    pipeline.front_face = rhi::RenderFrontFace::counter_clockwise;
    // Depth is deliberately off. The pass writes the display image and must not bind the scene
    // depth target, which is what makes per-pass depth selection in the backend necessary.
    pipeline.depth_test_enable = false;
    pipeline.depth_write_enable = false;
    pipeline.blend_mode = rhi::RenderBlendMode::disabled;
    pipeline.color_target_format = rhi::RenderImageFormat::rgba8_unorm;
    tone_map_pipeline_key_.shader_program = tone_map_shader_program_;
    tone_map_pipeline_key_.vertex_layout =
        hash_vertex_layout(pipeline.vertex_stride, pipeline.vertex_attributes);
    tone_map_pipeline_key_.render_phase = RenderPhase::post_process;
    tone_map_pipeline_key_.color_format = pipeline.color_target_format;
    tone_map_pipeline_key_.depth_format = pipeline.depth_target_format;
    tone_map_pipeline_key_.cull_mode = pipeline.cull_mode;
    tone_map_pipeline_key_.front_face = pipeline.front_face;
    tone_map_pipeline_key_.depth_test = pipeline.depth_test_enable;
    tone_map_pipeline_key_.depth_write = pipeline.depth_write_enable;
    tone_map_pipeline_key_.depth_compare = pipeline.depth_compare;
    tone_map_pipeline_key_.blend_mode = pipeline.blend_mode;
    auto created = pipeline_cache_->prewarm(tone_map_pipeline_key_, layout, std::move(pipeline));
    if (!created) {
        return core::Status::failure(created.error().code, created.error().message);
    }
    tone_map_pipeline_ = created.value();
    if (frame_builder_ != nullptr) {
        frame_builder_->set_tone_map_pipeline(tone_map_pipeline_);
    }
    // No descriptor write here on purpose: the scene_hdr binding is resolved by the backend from
    // the frame graph, every frame.
    return core::Status::ok();
}

} // namespace heartstead::renderer
