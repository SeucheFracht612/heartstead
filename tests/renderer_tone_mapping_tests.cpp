// Reads back what the renderer actually resolved to the display image and asserts the tone mapping
// pass responded to its inputs.
//
// This exists because "does exposure work?" was previously only answerable by a human looking at a
// window. Every assertion here is over real pixels produced by a real device, so a resolve pass
// that ignored its push constants, sampled the wrong image, or inverted its exposure would fail
// here instead of surviving until somebody noticed the screen looked odd.
//
// The test needs a working Vulkan device. Where none exists it reports success without asserting,
// because a missing driver is an absent environment rather than a renderer defect.

#include "engine/renderer/frame/frame_builder.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"

#include "engine/renderer/shaders/spirv_loader.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <numeric>
#include <span>
#include <vector>

namespace {

using namespace heartstead;
using namespace heartstead::renderer;

[[nodiscard]] double mean_luminance(std::span<const std::uint8_t> rgba) {
    if (rgba.size() < 4) {
        return 0.0;
    }
    double total = 0.0;
    std::size_t samples = 0;
    for (std::size_t index = 0; index + 3 < rgba.size(); index += 4) {
        total += 0.2126 * rgba[index] + 0.7152 * rgba[index + 1] + 0.0722 * rgba[index + 2];
        ++samples;
    }
    return samples == 0 ? 0.0 : total / static_cast<double>(samples);
}

// Builds the real tone map material against the shipped SPIR-V. Without it the graph still has a
// tone_map pass, but nothing draws into it, and every exposure resolves to the same cleared image.
[[nodiscard]] rhi::RenderResourceHandle create_tone_map_pipeline(rhi::IRenderDevice& device) {
    const std::filesystem::path root{HEARTSTEAD_BUILTIN_SHADER_DIR};
    auto vertex_spirv = shaders::load_spirv_file(root / "tone_map.vert.spv");
    auto fragment_spirv = shaders::load_spirv_file(root / "tone_map.frag.spv");
    assert(vertex_spirv && fragment_spirv);

    auto vertex = device.create_shader_module({rhi::RenderShaderStage::vertex, "tone_map_vs"},
                                              vertex_spirv.value());
    auto fragment = device.create_shader_module({rhi::RenderShaderStage::fragment, "tone_map_fs"},
                                                fragment_spirv.value());
    assert(vertex && fragment);

    const auto material = core::PrototypeId::parse("base:materials/tone_map");
    assert(material);

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = *material;
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
    layout.per_frame_descriptors = true;
    const auto bound = device.bind_pipeline_layout(layout);
    assert(bound);

    rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.vertex_shader = vertex.value().handle;
    pipeline.fragment_shader = fragment.value().handle;
    pipeline.material_id = *material;
    pipeline.debug_name = "tone_map_pipeline";
    pipeline.vertex_stride = 0;
    pipeline.topology = rhi::RenderPrimitiveTopology::triangle_list;
    pipeline.cull_mode = rhi::RenderCullMode::none;
    pipeline.depth_test_enable = false;
    pipeline.depth_write_enable = false;
    pipeline.color_target_format = rhi::RenderImageFormat::rgba8_unorm;
    const auto created = device.create_graphics_pipeline(pipeline);
    assert(created);
    return created.value().handle;
}

// Renders one frame at the given exposure and returns the mean luminance of the resolved image.
[[nodiscard]] double resolve_mean_luminance(rhi::IRenderDevice& device,
                                            rhi::RenderResourceHandle tone_map_pipeline,
                                            float exposure_stops) {
    FrameBuilder builder({256, 144}, rhi::ClearColor{0.35F, 0.35F, 0.35F, 1.0F});
    builder.set_tone_map_pipeline(tone_map_pipeline);
    rhi::RenderExposureSettings exposure;
    exposure.exposure_stops = exposure_stops;
    const auto applied = builder.set_exposure(exposure);
    assert(applied);

    RenderCamera camera;
    auto submission = builder.build(camera, RenderCommandLists{});
    assert(submission);

    // There is no window here, so drop the present pass and resolve offscreen. The tone mapping
    // pass under test runs either way; only the final blit to a swapchain is skipped.
    auto frame = std::move(submission).value();
    if (!frame.plan.passes.empty() &&
        frame.plan.passes.back().kind == rhi::RenderPassKind::present) {
        frame.plan.passes.pop_back();
    }

    const auto executed = device.execute_frame(frame);
    if (!executed) {
        std::printf("  execute_frame failed: %s (%s)\n", executed.error().message.c_str(),
                    executed.error().code.c_str());
    }
    assert(executed);

    auto pixels = device.read_back_output_image();
    if (!pixels) {
        std::printf("  readback failed: %s (%s)\n", pixels.error().message.c_str(),
                    pixels.error().code.c_str());
    }
    assert(pixels);
    return mean_luminance(pixels.value());
}

// Exposure is expressed in stops, so a higher value must resolve to a brighter image. Getting the
// sign backwards is invisible to every other test in the suite.
void test_exposure_brightens_the_resolved_image(rhi::IRenderDevice& device,
                                                rhi::RenderResourceHandle tone_map_pipeline) {
    const auto dark = resolve_mean_luminance(device, tone_map_pipeline, -4.0F);
    const auto neutral = resolve_mean_luminance(device, tone_map_pipeline, 0.0F);
    const auto bright = resolve_mean_luminance(device, tone_map_pipeline, 4.0F);

    std::fprintf(stderr, "  exposure -4: %.2f, 0: %.2f, +4: %.2f\n", dark, neutral, bright);
    assert(dark < neutral);
    assert(neutral < bright);
    // Four stops either side of neutral has to be an unmistakable difference, not a rounding one.
    // A resolve pass that ignored its constants would land all three within noise of each other.
    assert(bright - dark > 20.0);
}

// The scene target is cleared to a known grey, so the resolved image must actually carry it. A
// pass sampling the wrong image, or an unbound descriptor reading black, fails here.
void test_resolved_image_carries_scene_content(rhi::IRenderDevice& device,
                                               rhi::RenderResourceHandle tone_map_pipeline) {
    const auto neutral = resolve_mean_luminance(device, tone_map_pipeline, 0.0F);
    std::fprintf(stderr, "  neutral resolve luminance: %.2f\n", neutral);
    assert(neutral > 1.0);
    assert(neutral < 254.0);
}

} // namespace

int main() {
    rhi::RenderDeviceDesc desc;
    desc.backend = rhi::RenderBackend::vulkan;
    desc.application_name = "heartstead_renderer_tone_mapping_tests";
    desc.initial_extent = {256, 144};
    desc.enable_validation = true;

    auto device = rhi::create_render_device(desc);
    if (!device) {
        std::printf("no Vulkan device available (%s); skipping tone mapping readback tests\n",
                    device.error().code.c_str());
        return 0;
    }

    // A device that cannot read pixels back cannot answer these questions either way.
    auto probe = device.value()->read_back_output_image();
    if (!probe && probe.error().code == "renderer.readback_unsupported") {
        std::printf("device does not support readback; skipping tone mapping tests\n");
        return 0;
    }

    const auto tone_map_pipeline = create_tone_map_pipeline(*device.value());
    test_resolved_image_carries_scene_content(*device.value(), tone_map_pipeline);
    test_exposure_brightens_the_resolved_image(*device.value(), tone_map_pipeline);
    std::printf("tone mapping readback tests passed\n");
    return 0;
}
