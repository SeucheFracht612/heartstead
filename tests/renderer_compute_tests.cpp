// Proves the compute pass path executes rather than merely records.
//
// create_compute_pipeline has existed since the renderer was written, but nothing ever dispatched:
// there was no vkCmdDispatch anywhere in the backend, so a compute pipeline could be created and
// then never run. This test dispatches a real compute shader through a real device and reads the
// buffer it wrote, so "compute works" is a claim backed by the values it produced.
//
// The shader writes each invocation's global index into a storage buffer, which distinguishes a
// dispatch that ran, one that ran with the wrong group count, and one that never ran at all.

#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"
#include "engine/renderer/shaders/spirv_loader.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {

using namespace heartstead;
using namespace heartstead::renderer;

constexpr std::uint32_t element_count = 256;
constexpr std::uint32_t local_size = 64;

struct DispatchProbeConstants {
    std::uint32_t element_count = 0;
    std::uint32_t bias = 0;
};

[[nodiscard]] std::vector<std::uint32_t> decode_u32(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint32_t> values(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(values.data(), bytes.data(), values.size() * sizeof(std::uint32_t));
    return values;
}

} // namespace

int main() {
    rhi::RenderDeviceDesc desc;
    desc.backend = rhi::RenderBackend::vulkan;
    desc.application_name = "heartstead_renderer_compute_tests";
    desc.initial_extent = {256, 144};
    desc.enable_validation = true;

    auto device_result = rhi::create_render_device(desc);
    if (!device_result) {
        std::printf("no Vulkan device available (%s); skipping compute tests\n",
                    device_result.error().code.c_str());
        return 0;
    }
    auto& device = *device_result.value();
    if (!device.capabilities().supports_compute_pipelines) {
        std::printf("device reports no compute support; skipping compute tests\n");
        return 0;
    }

    const std::filesystem::path root{HEARTSTEAD_BUILTIN_SHADER_DIR};
    auto spirv = shaders::load_spirv_file(root / "dispatch_probe.comp.spv");
    assert(spirv);

    auto module = device.create_shader_module({rhi::RenderShaderStage::compute, "dispatch_probe"},
                                              spirv.value());
    assert(module);

    const auto material = core::PrototypeId::parse("base:materials/dispatch_probe");
    assert(material);

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = *material;
    layout.shader_template = {"base", "shaders/dispatch_probe.comp"};
    layout.descriptors = {{"probe", rhi::RenderDescriptorKind::storage_buffer, 0, true,
                           rhi::RenderShaderStageFlags::compute}};
    layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::compute, 0, sizeof(DispatchProbeConstants)});
    layout.debug_name = "dispatch_probe_layout";
    assert(device.bind_pipeline_layout(layout));

    // Zero filled, so any non-zero value read back had to be written by the dispatch.
    const std::vector<std::uint32_t> zeros(element_count, 0);
    const auto zero_bytes = std::as_bytes(std::span<const std::uint32_t>(zeros));
    auto storage = device.upload_buffer({rhi::RenderBufferUsage::storage, zero_bytes.size(),
                                         "dispatch_probe_storage",
                                         rhi::RenderBufferMemory::device_local},
                                        zero_bytes);
    assert(storage);

    const rhi::RenderDescriptorWrite probe_write{*material, "probe", storage.value().handle, 0,
                                                 zero_bytes.size()};
    assert(device.write_descriptors(std::span<const rhi::RenderDescriptorWrite>{&probe_write, 1}));

    rhi::RenderComputePipelineDesc pipeline;
    pipeline.compute_shader = module.value().handle;
    pipeline.material_id = *material;
    pipeline.debug_name = "dispatch_probe_pipeline";
    auto compute_pipeline = device.create_compute_pipeline(pipeline);
    assert(compute_pipeline);

    // The buffer starts zeroed, so confirm that before dispatching. Otherwise a readback that
    // returns the expected pattern for some unrelated reason would look like success.
    const auto before = device.read_back_buffer(storage.value().handle);
    if (!before && before.error().code == "renderer.readback_unsupported") {
        std::printf("device does not support buffer readback; skipping compute tests\n");
        return 0;
    }
    assert(before);
    const auto initial = decode_u32(before.value());
    assert(initial.size() == element_count);
    assert(initial.front() == 0 && initial.back() == 0);

    rhi::RenderFramePlanBuilder builder({256, 144});
    assert(builder.add_resource({std::string(rhi::output_resource_name),
                                 {256, 144},
                                 rhi::RenderResourceLifetime::external,
                                 rhi::RenderImageFormat::rgba8_unorm}));
    assert(builder.add_resource({std::string(rhi::depth_resource_name),
                                 {256, 144},
                                 rhi::RenderResourceLifetime::transient,
                                 rhi::RenderImageFormat::d32_sfloat}));
    assert(builder.add_pass({.name = "light_cull",
                             .kind = rhi::RenderPassKind::compute,
                             .writes = {std::string(rhi::output_resource_name)}}));
    auto plan = builder.build();
    assert(plan);

    rhi::RenderFrameSubmission frame;
    frame.plan = std::move(plan).value();
    const DispatchProbeConstants constants{element_count, 0};
    std::vector<std::byte> constant_bytes(sizeof(constants));
    std::memcpy(constant_bytes.data(), &constants, sizeof(constants));

    rhi::RenderPassCommands compute_commands;
    compute_commands.pass_index = 0;
    compute_commands.dispatches.push_back({compute_pipeline.value().handle,
                                           element_count / local_size, 1, 1,
                                           std::move(constant_bytes)});
    frame.pass_commands.push_back(std::move(compute_commands));

    const auto executed = device.execute_frame(frame);
    if (!executed) {
        std::fprintf(stderr, "execute_frame failed: %s (%s)\n", executed.error().message.c_str(),
                    executed.error().code.c_str());
    }
    assert(executed);

    const auto after = device.read_back_buffer(storage.value().handle);
    assert(after);
    const auto written = decode_u32(after.value());
    assert(written.size() == element_count);

    // Every invocation writes its own index, so the whole range must be present and ordered. A
    // dispatch with too few groups would leave the tail zeroed.
    for (std::uint32_t index = 0; index < element_count; ++index) {
        assert(written[index] == index);
    }
    std::printf("compute dispatch wrote %u elements, first=%u last=%u\n", element_count,
                written.front(), written.back());
    std::printf("compute tests passed\n");
    return 0;
}
