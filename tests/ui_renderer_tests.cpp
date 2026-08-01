#include "engine/renderer/frame/frame_builder.hpp"
#include "engine/renderer/ui/ui_renderer.hpp"
#include "engine/core/file_io.hpp"

#include <array>
#include <cassert>

namespace {

using namespace heartstead;

struct UiFixture {
    std::unique_ptr<renderer::rhi::IRenderDevice> device;
    renderer::rhi::RenderResourceHandle pipeline;
    std::shared_ptr<const renderer::UiFont> font;
};

[[nodiscard]] UiFixture make_fixture() {
    using namespace renderer::rhi;
    RenderDeviceDesc device_desc;
    device_desc.initial_extent = {640, 360};
    auto device = create_render_device(device_desc);
    assert(device);
    const auto material = core::PrototypeId::parse("base:materials/ui_test");
    assert(material);
    RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/ui.vert"};
    layout.push_constant_ranges.push_back(
        {RenderShaderStageFlags::vertex | RenderShaderStageFlags::fragment, 0,
         sizeof(ChunkPushConstants)});
    assert(device.value()->bind_pipeline_layout(layout));
    constexpr std::array<std::uint32_t, 5> spirv{0x07230203, 0x00010000, 0, 1, 0};
    auto vertex = device.value()->create_shader_module({RenderShaderStage::vertex, "ui_vertex"},
                                                        spirv);
    auto fragment = device.value()->create_shader_module(
        {RenderShaderStage::fragment, "ui_fragment"}, spirv);
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
    pipeline.depth_compare = RenderCompareOperation::always;
    pipeline.blend_mode = RenderBlendMode::alpha;
    auto created = device.value()->create_graphics_pipeline(pipeline);
    assert(created);
    auto font_bytes = core::read_binary_file(
        std::filesystem::path{HEARTSTEAD_TEST_SOURCE_DIR} /
        "mods/base/assets/fonts/heartstead-ui.ttf");
    assert(font_bytes);
    auto font = renderer::UiFont::build(font_bytes.value());
    assert(font);
    return {std::move(device).value(), created.value().handle,
            std::make_shared<renderer::UiFont>(std::move(font).value())};
}

void test_ui_geometry_scissors_and_capacity() {
    auto fixture = make_fixture();
    renderer::UiRenderer ui(*fixture.device, fixture.pipeline, fixture.font);
    renderer::UiRendererConfig config;
    config.maximum_vertices = 8;
    config.maximum_indices = 12;
    assert(ui.initialize({640, 360}, config));
    renderer::UiQuadDesc first;
    first.minimum_pixels = {16.0F, 24.0F};
    first.maximum_pixels = {116.0F, 72.0F};
    first.color = {0.2F, 0.4F, 0.8F, 0.75F};
    first.scissor_enabled = true;
    first.scissor = {8, 8, 200, 100};
    assert(ui.submit_quad(first));
    auto second = first;
    second.minimum_pixels = {128.0F, 24.0F};
    second.maximum_pixels = {228.0F, 72.0F};
    second.texture_layer = 1;
    second.scissor_enabled = false;
    assert(ui.submit_quad(second));
    assert(!ui.submit_quad(second));

    auto commands = ui.build_frame();
    assert(commands);
    assert(commands.value().draws.size() == 2);
    assert(commands.value().draws.front().scissor_enabled);
    assert(commands.value().draws.front().scissor.width == 200);
    assert(commands.value().stats.submitted_vertices == 8);
    assert(commands.value().stats.submitted_indices == 12);
    assert(commands.value().stats.clipped_draw_calls == 1);
    assert(commands.value().stats.overflowed_batches == 1);

    renderer::RenderCamera camera;
    assert(camera.update_matrices());
    renderer::FrameBuilder builder({640, 360});
    renderer::RenderCommandLists lists;
    lists.ui_draws = commands.value().draws;
    auto frame = builder.build(camera, std::move(lists));
    assert(frame);
    assert(frame.value().pass_commands.front().pass_index == renderer::hdr_pass_index::ui);
    auto executed = fixture.device->execute_frame(frame.value());
    assert(executed);
    assert(executed.value().draw_count == 2);
    assert(ui.submit_text({{12.0F, 12.0F}, "A", 10.0F,
                           {1.0F, 1.0F, 1.0F, 1.0F}}));
    auto text = ui.build_frame();
    assert(text);
    assert(text.value().draws.size() == 1);
    assert(text.value().stats.submitted_glyphs == 1);
    assert(text.value().stats.submitted_vertices == 4);
    assert(text.value().stats.overflowed_batches == 0);
    auto empty = ui.build_frame();
    assert(empty && empty.value().draws.empty());
    assert(empty.value().stats.overflowed_batches == 0);
    assert(ui.resize({800, 600}));
    assert(ui.shutdown());
}

void test_ui_upload_failure_discards_pending_geometry() {
    auto fixture = make_fixture();
    renderer::UiRenderer ui(*fixture.device, fixture.pipeline, fixture.font);
    assert(ui.initialize({640, 360}));
    renderer::UiQuadDesc quad;
    quad.maximum_pixels = {32.0F, 32.0F};
    assert(ui.submit_quad(quad));
    assert(ui.pending_vertex_count() == 4);
    assert(ui.pending_index_count() == 6);
    assert(ui.pending_batch_count() == 1);

    assert(fixture.device->release_resource(ui.vertex_buffer()));
    assert(!ui.build_frame());
    assert(ui.pending_vertex_count() == 0);
    assert(ui.pending_index_count() == 0);
    assert(ui.pending_batch_count() == 0);
}

void test_ui_validation() {
    auto fixture = make_fixture();
    renderer::UiRendererConfig atlas_config;
    atlas_config.atlas_layers = 1;
    assert(!atlas_config.validate());
    atlas_config.atlas_layers = 17;
    assert(!atlas_config.validate());
    atlas_config.atlas_layers = 3;
    assert(atlas_config.validate());
    renderer::UiRenderer ui(*fixture.device, fixture.pipeline);
    assert(ui.initialize({640, 360}));
    renderer::UiQuadDesc invalid;
    invalid.maximum_pixels = {-1.0F, 0.0F};
    assert(!ui.submit_quad(invalid));
    renderer::UiQuadDesc clipped;
    clipped.maximum_pixels = {10.0F, 10.0F};
    clipped.scissor_enabled = true;
    clipped.scissor = {639, 0, 2, 4};
    assert(!ui.submit_quad(clipped));
    assert(ui.shutdown());
}

void test_ui_pixel_coordinates_use_top_left_origin() {
    constexpr renderer::rhi::RenderExtent extent{640, 360};
    const auto top_left = renderer::ui_pixel_position_to_ndc({0.0F, 0.0F}, extent);
    const auto center = renderer::ui_pixel_position_to_ndc({320.0F, 180.0F}, extent);
    const auto bottom_right = renderer::ui_pixel_position_to_ndc({640.0F, 360.0F}, extent);
    assert(top_left == math::Vec2f(-1.0F, -1.0F));
    assert(center == math::Vec2f(0.0F, 0.0F));
    assert(bottom_right == math::Vec2f(1.0F, 1.0F));
}

void test_ui_accessibility_color_transform() {
    renderer::UiAccessibilitySettings settings;
    settings.contrast = 1.25F;
    settings.saturation = 0.8F;
    settings.color_vision_mode = renderer::UiColorVisionMode::deuteranopia;
    assert(settings.validate());
    const auto transformed =
        renderer::apply_ui_accessibility_color({0.9F, 0.2F, 0.1F, 0.75F}, settings);
    assert(transformed[0] >= 0.0F && transformed[0] <= 1.0F);
    assert(transformed[1] >= 0.0F && transformed[1] <= 1.0F);
    assert(transformed[2] >= 0.0F && transformed[2] <= 1.0F);
    assert(transformed[3] == 0.75F);
    settings.contrast = 4.0F;
    assert(!settings.validate());
}

} // namespace

int main() {
    test_ui_geometry_scissors_and_capacity();
    test_ui_upload_failure_discards_pending_geometry();
    test_ui_validation();
    test_ui_pixel_coordinates_use_top_left_origin();
    test_ui_accessibility_color_transform();
    return 0;
}
