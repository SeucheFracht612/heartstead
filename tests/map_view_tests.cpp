#include "engine/ui/map_view.hpp"

#include <array>
#include <cassert>

int main() {
    using namespace heartstead;
    using namespace renderer::rhi;
    RenderDeviceDesc device_desc;
    device_desc.initial_extent = {640, 360};
    auto device = create_render_device(device_desc);
    assert(device);
    const auto material = core::PrototypeId::parse("base:materials/map_test");
    assert(material);
    RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/ui.vert"};
    layout.push_constant_ranges.push_back(
        {RenderShaderStageFlags::vertex | RenderShaderStageFlags::fragment, 0,
         sizeof(ChunkPushConstants)});
    assert(device.value()->bind_pipeline_layout(layout));
    constexpr std::array<std::uint32_t, 5> spirv{0x07230203, 0x00010000, 0, 1, 0};
    auto vertex = device.value()->create_shader_module({RenderShaderStage::vertex, "map_vertex"},
                                                        spirv);
    auto fragment = device.value()->create_shader_module(
        {RenderShaderStage::fragment, "map_fragment"}, spirv);
    assert(vertex && fragment);
    RenderGraphicsPipelineDesc pipeline;
    pipeline.vertex_shader = vertex.value().handle;
    pipeline.fragment_shader = fragment.value().handle;
    pipeline.material_id = material.value();
    pipeline.vertex_stride = sizeof(renderer::GpuUiVertex);
    pipeline.vertex_attributes.assign(renderer::gpu_ui_vertex_attributes.begin(),
                                      renderer::gpu_ui_vertex_attributes.end());
    pipeline.cull_mode = RenderCullMode::none;
    pipeline.depth_test_enable = false;
    pipeline.depth_write_enable = false;
    auto created = device.value()->create_graphics_pipeline(pipeline);
    assert(created);
    renderer::UiRenderer ui_renderer(*device.value(), created.value().handle);
    assert(ui_renderer.initialize({640, 360}));

    player_profiles::MapDiscovery discovery;
    assert(discovery.discover("surface", {1'000'000'000, -1'000'000'000}));
    assert(discovery.discover("surface", {1'000'000'001, -1'000'000'000}));
    assert(discovery.discover("underground", {1'000'000'000, -1'000'000'000}));
    ui::MapViewDesc view;
    view.minimum_pixels = {420.0F, 16.0F};
    view.maximum_pixels = {620.0F, 216.0F};
    view.center = {1'000'000'000, -1'000'000'000};
    view.cell_radius = 8;
    const std::array markers{ui::MapMarker{7, view.center, "surface",
                                           ui::MapMarkerKind::player,
                                           {1.0F, 0.8F, 0.2F, 1.0F}, "Player"}};
    ui::MapViewRenderer map;
    auto painted = map.paint(ui_renderer, {640, 360}, discovery, view, markers);
    assert(painted);
    assert(painted.value().tested_cells == 289);
    assert(painted.value().discovered_cells == 2);
    assert(painted.value().visible_markers == 1);
    auto commands = ui_renderer.build_frame();
    assert(commands && commands.value().draws.size() == 1);
    assert(commands.value().draws.front().scissor_enabled);

    view.layer_id = "underground";
    painted = map.paint(ui_renderer, {640, 360}, discovery, view);
    assert(painted && painted.value().discovered_cells == 1);
    view.cell_radius = 100;
    assert(!map.paint(ui_renderer, {640, 360}, discovery, view));
    assert(ui_renderer.shutdown());
    return 0;
}
