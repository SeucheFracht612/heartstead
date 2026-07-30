#include "engine/renderer/debug/debug_renderer.hpp"
#include "engine/renderer/frame/frame_builder.hpp"

#include <array>
#include <cassert>
#include <thread>
#include <vector>

namespace {

using namespace heartstead;

[[nodiscard]] world::WorldPosition position(math::Vec3d local) {
    auto value = world::WorldPosition::from_anchor({9'000'000'000'000'000LL, 0, -7}, local);
    assert(value);
    return value.value();
}

struct DebugFixture {
    std::unique_ptr<renderer::rhi::IRenderDevice> device;
    renderer::DebugPipelineSet pipelines;
};

[[nodiscard]] DebugFixture make_fixture() {
    using namespace renderer::rhi;
    auto device = create_render_device({});
    assert(device);
    const auto material = core::PrototypeId::parse("base:materials/debug_lines_test");
    assert(material);
    RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/debug_line.vert"};
    layout.push_constant_ranges.push_back(
        {RenderShaderStageFlags::vertex | RenderShaderStageFlags::fragment, 0,
         sizeof(ChunkPushConstants)});
    assert(device.value()->bind_pipeline_layout(layout));
    constexpr std::array<std::uint32_t, 5> spirv{0x07230203, 0x00010000, 0, 1, 0};
    auto vertex = device.value()->create_shader_module(
        {RenderShaderStage::vertex, "debug_test_vertex"}, spirv);
    auto fragment = device.value()->create_shader_module(
        {RenderShaderStage::fragment, "debug_test_fragment"}, spirv);
    assert(vertex && fragment);

    RenderGraphicsPipelineDesc pipeline;
    pipeline.vertex_shader = vertex.value().handle;
    pipeline.fragment_shader = fragment.value().handle;
    pipeline.material_id = material.value();
    pipeline.vertex_stride = sizeof(renderer::GpuDebugVertex);
    pipeline.vertex_attributes.assign(std::begin(renderer::gpu_debug_vertex_attributes),
                                      std::end(renderer::gpu_debug_vertex_attributes));
    pipeline.topology = RenderPrimitiveTopology::line_list;
    // The debug pass draws into the linear scene target, not the display image.
    pipeline.color_target_format = RenderImageFormat::rgba16_sfloat;
    pipeline.cull_mode = RenderCullMode::none;
    pipeline.depth_test_enable = true;
    pipeline.depth_write_enable = false;
    pipeline.depth_compare = RenderCompareOperation::less_or_equal;
    pipeline.debug_name = "debug_test_depth";
    auto depth = device.value()->create_graphics_pipeline(pipeline);
    assert(depth);
    pipeline.depth_compare = RenderCompareOperation::always;
    pipeline.debug_name = "debug_test_overlay";
    auto overlay = device.value()->create_graphics_pipeline(pipeline);
    assert(overlay);
    return {std::move(device).value(), {depth.value().handle, overlay.value().handle}};
}

void test_debug_primitives_lifetimes_and_unified_draws() {
    auto fixture = make_fixture();
    renderer::DebugRenderer debug(*fixture.device, fixture.pipelines);
    renderer::DebugRendererConfig config;
    config.maximum_lines = 64;
    config.maximum_text_labels = 8;
    assert(debug.initialize(config));
    const auto origin = position({0.0, 0.0, 0.0});
    const auto end = position({1.0, 0.0, -5.0});
    assert(debug.submit_line({origin, end, {1.0F, 1.0F, 0.0F, 1.0F}, 0.0F,
                              renderer::DebugDepthMode::depth_tested}));
    assert(debug.submit_line({origin, end, {1.0F, 0.0F, 1.0F, 1.0F}, 0.5F,
                              renderer::DebugDepthMode::overlay}));
    assert(debug.submit_aabb(origin, {{-1.0F, -1.0F, -6.0F}, {1.0F, 1.0F, -4.0F}},
                             {0.2F, 0.8F, 1.0F, 1.0F}));
    assert(debug.submit_axes(origin, 2.0F));
    assert(debug.submit_text({end, "debug label", {1.0F, 1.0F, 1.0F, 1.0F}, 0.5F}));

    renderer::RenderCamera camera;
    camera.floating_origin.block = origin.anchor;
    assert(camera.update_matrices());
    auto commands = debug.build_frame(camera, 0.1F);
    assert(commands);
    assert(commands.value().draws.size() == 2);
    assert(commands.value().stats.submitted_lines == 17);
    assert(commands.value().stats.draw_calls == 2);
    assert(commands.value().text_labels.size() == 1);
    assert(commands.value().stats.uploaded_bytes ==
           34U * (sizeof(renderer::GpuDebugVertex) + sizeof(std::uint32_t)));

    renderer::FrameBuilder frame_builder({640, 360});
    renderer::RenderCommandLists lists;
    lists.debug_draws = commands.value().draws;
    auto frame = frame_builder.build(camera, std::move(lists));
    assert(frame);
    assert(frame.value().pass_commands.size() == 1);
    assert(frame.value().pass_commands.front().pass_index == 5);
    auto executed = fixture.device->execute_frame(frame.value());
    assert(executed);
    assert(executed.value().draw_count == 2);
    assert(executed.value().total_indices == 34);

    auto timed_only = debug.build_frame(camera, 0.2F);
    assert(timed_only);
    assert(timed_only.value().stats.submitted_lines == 1);
    assert(timed_only.value().draws.size() == 1);
    assert(timed_only.value().text_labels.size() == 1);
    auto expires = debug.build_frame(camera, 0.2F);
    assert(expires);
    assert(expires.value().stats.submitted_lines == 1);
    auto empty = debug.build_frame(camera, 0.1F);
    assert(empty);
    assert(empty.value().draws.empty());
    assert(empty.value().text_labels.empty());
    assert(debug.shutdown());
}

void test_debug_capacity_and_thread_safe_submission() {
    auto fixture = make_fixture();
    renderer::DebugRenderer debug(*fixture.device, fixture.pipelines);
    renderer::DebugRendererConfig config;
    config.maximum_lines = 8;
    config.maximum_text_labels = 2;
    assert(debug.initialize(config));
    const auto origin = position({0.0, 0.0, 0.0});
    const auto end = position({0.0, 1.0, -2.0});
    std::vector<std::thread> workers;
    for (std::size_t worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&debug, origin, end] {
            for (std::size_t line = 0; line < 2; ++line) {
                assert(debug.submit_line({origin, end}));
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    assert(!debug.submit_line({origin, end}));
    renderer::RenderCamera camera;
    camera.floating_origin.block = origin.anchor;
    assert(camera.update_matrices());
    auto frame = debug.build_frame(camera, 0.0F);
    assert(frame);
    assert(frame.value().stats.submitted_lines == 8);
    assert(frame.value().stats.overflowed_lines == 1);
    assert(debug.shutdown());
}

} // namespace

int main() {
    test_debug_primitives_lifetimes_and_unified_draws();
    test_debug_capacity_and_thread_safe_submission();
    return 0;
}
