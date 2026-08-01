#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"
#include "engine/renderer/shaders/spirv_loader.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

using namespace heartstead;
using namespace heartstead::renderer;

int main() {
    rhi::RenderDeviceDesc desc;
    desc.backend = rhi::RenderBackend::vulkan;
    desc.application_name = "heartstead_renderer_indirect_gpu_tests";
    desc.initial_extent = {256, 144};
    desc.enable_validation = true;
    auto device_result = rhi::create_render_device(desc);
    if (!device_result) {
        std::printf("no Vulkan device available (%s); skipping indirect GPU test\n",
                    device_result.error().code.c_str());
        return 0;
    }
    auto& device = *device_result.value();
    if (!device.capabilities().supports_draw_indirect_count) {
        std::printf("device reports no indirect-count support; skipping counted GPU test\n");
        return 0;
    }

    const std::filesystem::path root{HEARTSTEAD_BUILTIN_SHADER_DIR};
    const auto compute_words = shaders::load_spirv_file(root / "indirect_probe.comp.spv");
    const auto vertex_words = shaders::load_spirv_file(root / "indirect_probe.vert.spv");
    const auto fragment_words = shaders::load_spirv_file(root / "indirect_probe.frag.spv");
    assert(compute_words && vertex_words && fragment_words);
    const auto compute_module = device.create_shader_module(
        {rhi::RenderShaderStage::compute, "indirect_probe_compute"}, compute_words.value());
    const auto vertex_module = device.create_shader_module(
        {rhi::RenderShaderStage::vertex, "indirect_probe_vertex"}, vertex_words.value());
    const auto fragment_module = device.create_shader_module(
        {rhi::RenderShaderStage::fragment, "indirect_probe_fragment"}, fragment_words.value());
    assert(compute_module && vertex_module && fragment_module);

    const auto compute_material = core::PrototypeId::parse("base:materials/indirect_probe_compute");
    const auto graphics_material = core::PrototypeId::parse("base:materials/indirect_probe_graphics");
    assert(compute_material && graphics_material);

    rhi::RenderPipelineLayoutDesc compute_layout;
    compute_layout.material_id = *compute_material;
    compute_layout.shader_template = {"base", "shaders/indirect_probe.comp"};
    compute_layout.descriptors = {
        {"commands", rhi::RenderDescriptorKind::storage_buffer, 0, true,
         rhi::RenderShaderStageFlags::compute},
        {"count", rhi::RenderDescriptorKind::storage_buffer, 1, true,
         rhi::RenderShaderStageFlags::compute},
    };
    compute_layout.debug_name = "indirect_probe_compute_layout";
    assert(device.bind_pipeline_layout(compute_layout));

    rhi::RenderPipelineLayoutDesc graphics_layout;
    graphics_layout.material_id = *graphics_material;
    graphics_layout.shader_template = {"base", "shaders/indirect_probe"};
    graphics_layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment,
         0, sizeof(rhi::ChunkPushConstants)});
    graphics_layout.debug_name = "indirect_probe_graphics_layout";
    assert(device.bind_pipeline_layout(graphics_layout));

    const std::array<std::byte, sizeof(rhi::RenderIndexedIndirectCommand)> zeros{};
    const std::array<std::byte, sizeof(std::uint32_t)> count_zeros{};
    auto command_buffer = device.upload_buffer(
        {rhi::RenderBufferUsage::storage_indirect, zeros.size(), "gpu_indirect_commands",
         rhi::RenderBufferMemory::device_local}, zeros);
    auto count_buffer = device.upload_buffer(
        {rhi::RenderBufferUsage::storage_indirect, count_zeros.size(), "gpu_indirect_count",
         rhi::RenderBufferMemory::device_local}, count_zeros);
    const std::array<std::uint16_t, 3> indices{0, 1, 2};
    const std::array<float, 1> dummy_vertices{0.0F};
    auto vertex_buffer = device.upload_buffer(
        {rhi::RenderBufferUsage::vertex, sizeof(dummy_vertices), "indirect_probe_vertices",
         rhi::RenderBufferMemory::device_local}, std::as_bytes(std::span{dummy_vertices}));
    auto index_buffer = device.upload_buffer(
        {rhi::RenderBufferUsage::index, sizeof(indices), "indirect_probe_indices",
         rhi::RenderBufferMemory::device_local}, std::as_bytes(std::span{indices}));
    assert(command_buffer && count_buffer && vertex_buffer && index_buffer);

    const std::array descriptor_writes{
        rhi::RenderDescriptorWrite{*compute_material, "commands", command_buffer.value().handle,
                                   0, zeros.size()},
        rhi::RenderDescriptorWrite{*compute_material, "count", count_buffer.value().handle,
                                   0, count_zeros.size()},
    };
    assert(device.write_descriptors(descriptor_writes));

    rhi::RenderComputePipelineDesc compute_pipeline_desc;
    compute_pipeline_desc.compute_shader = compute_module.value().handle;
    compute_pipeline_desc.material_id = *compute_material;
    compute_pipeline_desc.debug_name = "indirect_probe_compute_pipeline";
    const auto compute_pipeline = device.create_compute_pipeline(compute_pipeline_desc);
    assert(compute_pipeline);

    rhi::RenderGraphicsPipelineDesc graphics_pipeline_desc;
    graphics_pipeline_desc.vertex_shader = vertex_module.value().handle;
    graphics_pipeline_desc.fragment_shader = fragment_module.value().handle;
    graphics_pipeline_desc.material_id = *graphics_material;
    graphics_pipeline_desc.debug_name = "indirect_probe_graphics_pipeline";
    graphics_pipeline_desc.cull_mode = rhi::RenderCullMode::none;
    graphics_pipeline_desc.color_target_format = rhi::RenderImageFormat::rgba16_sfloat;
    graphics_pipeline_desc.additional_color_target_formats = {
        rhi::RenderImageFormat::rg16_sfloat};
    const auto graphics_pipeline = device.create_graphics_pipeline(graphics_pipeline_desc);
    assert(graphics_pipeline);

    rhi::RenderFramePlanBuilder builder({256, 144});
    assert(builder.add_resource({"visibility_sync", {256, 144},
                                 rhi::RenderResourceLifetime::transient,
                                 rhi::RenderImageFormat::rgba8_unorm, true}));
    assert(builder.add_resource({"scene_hdr", {256, 144},
                                 rhi::RenderResourceLifetime::transient,
                                 rhi::RenderImageFormat::rgba16_sfloat}));
    assert(builder.add_resource({"motion", {256, 144},
                                 rhi::RenderResourceLifetime::transient,
                                 rhi::RenderImageFormat::rg16_sfloat}));
    assert(builder.add_resource({std::string(rhi::depth_resource_name), {256, 144},
                                 rhi::RenderResourceLifetime::transient,
                                 rhi::RenderImageFormat::d32_sfloat}));
    assert(builder.add_pass({.name = "visibility_cull",
                             .kind = rhi::RenderPassKind::compute,
                             .writes = {"visibility_sync"}}));
    assert(builder.add_pass({.name = "opaque_terrain",
                             .kind = rhi::RenderPassKind::world,
                             .reads = {"visibility_sync"},
                             .writes = {"scene_hdr", "motion",
                                        std::string(rhi::depth_resource_name)}}));
    auto plan = builder.build();
    assert(plan);

    rhi::RenderFrameSubmission frame;
    frame.plan = std::move(plan.value());
    rhi::RenderPassCommands compute_commands;
    compute_commands.pass_index = 0;
    compute_commands.dispatches.push_back({compute_pipeline.value().handle, 1, 1, 1, {}});
    frame.pass_commands.push_back(std::move(compute_commands));

    rhi::RenderDrawCommand draw;
    draw.pipeline = graphics_pipeline.value().handle;
    draw.vertex_buffer = vertex_buffer.value().handle;
    draw.index_buffer = index_buffer.value().handle;
    draw.index_type = rhi::RenderIndexType::uint16;
    draw.index_count = 3;
    draw.indirect.command_buffer = command_buffer.value().handle;
    draw.indirect.count_buffer = count_buffer.value().handle;
    draw.indirect.maximum_draw_count = 1;
    rhi::RenderPassCommands graphics_commands;
    graphics_commands.pass_index = 1;
    graphics_commands.draws.push_back(draw);
    frame.pass_commands.push_back(std::move(graphics_commands));

    const auto executed = device.execute_frame(frame);
    if (!executed) {
        std::fprintf(stderr, "indirect frame failed: %s (%s)\n",
                     executed.error().message.c_str(), executed.error().code.c_str());
    }
    assert(executed);
    assert(executed.value().draw_count == 1U);

    const auto command_readback = device.read_back_buffer(command_buffer.value().handle);
    const auto count_readback = device.read_back_buffer(count_buffer.value().handle);
    assert(command_readback && count_readback);
    rhi::RenderIndexedIndirectCommand command{};
    std::uint32_t count = 0;
    std::memcpy(&command, command_readback.value().data(), sizeof(command));
    std::memcpy(&count, count_readback.value().data(), sizeof(count));
    assert(command.index_count == 3U && command.instance_count == 1U);
    assert(count == 1U);
    std::printf("counted indirect GPU draw executed with Vulkan validation enabled\n");
    return 0;
}
