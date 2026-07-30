// Headless Vulkan smoke check for the dynamic rendering conversion.
//
// Creates a real Vulkan device with validation enabled, builds a graphics pipeline from the
// shipped debug-line SPIR-V, and records a frame. Any validation layer message is treated as a
// failure. This runs on a software Vulkan implementation (lavapipe), so it is a correctness check
// on the recording, not a performance measurement.

#include "engine/core/logging.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace heartstead;
using namespace heartstead::renderer;

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%-58s %s\n", what, condition ? "ok" : "FAILED");
    if (!condition) {
        ++failures;
    }
}

[[nodiscard]] std::vector<std::uint32_t> load_spirv(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const auto byte_size = static_cast<std::size_t>(stream.tellg());
    stream.seekg(0);
    std::vector<std::uint32_t> words(byte_size / sizeof(std::uint32_t));
    stream.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(byte_size));
    return words;
}

} // namespace

int main(int argc, char** argv) {
    #ifdef HEARTSTEAD_SHADER_DIR
    const std::string default_shader_root = HEARTSTEAD_SHADER_DIR;
#else
    const std::string default_shader_root = "tools/dynamic_rendering_smoke";
#endif
    const std::string shader_root = argc > 1 ? argv[1] : default_shader_root;

    rhi::RenderDeviceDesc desc;
    desc.backend = rhi::RenderBackend::vulkan;
    desc.application_name = "heartstead_dynamic_rendering_smoke";
    desc.initial_extent = {256, 144};
    desc.enable_validation = true;

    auto device_result = rhi::create_render_device(desc);
    if (!device_result) {
        std::printf("device creation failed: %s (%s)\n", device_result.error().message.c_str(),
                    device_result.error().code.c_str());
        return 1;
    }
    auto& device = device_result.value();
    check(device->backend() == rhi::RenderBackend::vulkan, "vulkan device created");
    check(device->capabilities().supports_graphics_pipelines, "graphics pipelines supported");

    // render_frame presents, so it is unavailable without a surface. Headless frames go through
    // execute_frame_plan against the offscreen target.
    const auto presented = device->render_frame({});
    check(!presented && presented.error().code == "renderer.vulkan_present_unavailable",
          "headless device refuses to present");

    const auto plan = rhi::make_clear_present_frame_plan(device->current_extent(),
                                                         rhi::ClearColor{0.1F, 0.2F, 0.3F, 1.0F},
                                                         false);
    const auto cleared = device->execute_frame_plan(plan);
    check(static_cast<bool>(cleared), "execute_frame_plan recorded");

    // Build a pipeline from the shipped debug-line shaders. Under dynamic rendering this must
    // succeed without any render pass object existing.
    const auto vertex_spirv = load_spirv(shader_root + "/smoke_line.vert.spv");
    const auto fragment_spirv = load_spirv(shader_root + "/smoke_line.frag.spv");
    check(!vertex_spirv.empty() && !fragment_spirv.empty(), "smoke SPIR-V loaded");

    auto vertex_module = device->create_shader_module({rhi::RenderShaderStage::vertex, "smoke_vs"},
                                                      vertex_spirv);
    auto fragment_module =
        device->create_shader_module({rhi::RenderShaderStage::fragment, "smoke_fs"},
                                     fragment_spirv);
    check(static_cast<bool>(vertex_module) && static_cast<bool>(fragment_module),
          "shader modules created");
    if (!vertex_module || !fragment_module) {
        return 1;
    }

    const auto material = core::PrototypeId::parse("smoke:debug_line");
    check(material.has_value(), "material id parsed");
    if (!material) {
        return 1;
    }

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = *material;
    layout.shader_template.namespace_id = "smoke";
    layout.shader_template.relative_path = "debug_line";
    layout.debug_name = "smoke_layout";
    layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         static_cast<std::uint32_t>(sizeof(rhi::ChunkPushConstants))});
    check(static_cast<bool>(device->bind_pipeline_layout(layout)), "pipeline layout bound");

    rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.vertex_shader = vertex_module.value().handle;
    pipeline.fragment_shader = fragment_module.value().handle;
    pipeline.material_id = *material;
    pipeline.debug_name = "smoke_pipeline";
    pipeline.vertex_stride = sizeof(float) * 7;
    pipeline.vertex_attributes = {
        {0, 0, rhi::RenderVertexAttributeFormat::float3},
        {1, sizeof(float) * 3, rhi::RenderVertexAttributeFormat::float4},
    };
    pipeline.topology = rhi::RenderPrimitiveTopology::line_list;
    pipeline.cull_mode = rhi::RenderCullMode::none;
    pipeline.depth_test_enable = false;
    pipeline.depth_write_enable = false;
    const auto pipeline_result = device->create_graphics_pipeline(pipeline);
    check(static_cast<bool>(pipeline_result), "graphics pipeline created without a render pass");
    if (!pipeline_result) {
        std::printf("  pipeline error: %s\n", pipeline_result.error().message.c_str());
        return 1;
    }

    // Two line vertices: position plus colour.
    const std::array<float, 14> vertices{
        -0.5F, -0.5F, 0.0F, 1.0F, 0.4F, 0.2F, 1.0F,
        0.5F,  0.5F,  0.0F, 0.2F, 0.8F, 1.0F, 1.0F,
    };
    const auto bytes = std::as_bytes(std::span<const float>(vertices));
    auto vertex_buffer = device->upload_buffer(
        {rhi::RenderBufferUsage::vertex, bytes.size(), "smoke_vertices",
         rhi::RenderBufferMemory::device_local},
        bytes);
    check(static_cast<bool>(vertex_buffer), "vertex buffer uploaded");
    if (!vertex_buffer) {
        return 1;
    }

    // Records through vkCmdBeginRendering on the offscreen target.
    rhi::RenderMeshBinding binding;
    binding.vertex_buffer = vertex_buffer.value().handle;
    binding.material_id = *material;
    binding.vertex_count = 2;
    binding.instance_count = 1;
    binding.debug_name = "smoke_line";
    const std::array<rhi::RenderMeshBinding, 1> draws{binding};
    const auto draw_result = device->bind_mesh_draws(draws);
    check(static_cast<bool>(draw_result), "dynamic rendering draw recorded");
    if (!draw_result) {
        std::printf("  draw error: %s\n", draw_result.error().message.c_str());
    }

    check(static_cast<bool>(device->resize({320, 180})), "resize accepted");
    const auto resized_plan =
        rhi::make_clear_present_frame_plan(device->current_extent(), {}, false);
    const auto resized_frame = device->execute_frame_plan(resized_plan);
    check(static_cast<bool>(resized_frame), "frame after resize recorded");
    if (!resized_frame) {
        std::printf("  resized frame error: %s (%s)\n", resized_frame.error().message.c_str(),
                    resized_frame.error().code.c_str());
    }
    check(static_cast<bool>(device->wait_idle()), "device idle");

    std::printf("%s\n", failures == 0 ? "SMOKE OK" : "SMOKE FAILED");
    return failures == 0 ? 0 : 1;
}
