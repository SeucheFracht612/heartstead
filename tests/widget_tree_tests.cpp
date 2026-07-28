#include "engine/renderer/frame/frame_builder.hpp"
#include "engine/ui/widget_tree.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

namespace {

using namespace heartstead;

struct UiFixture {
    std::unique_ptr<renderer::rhi::IRenderDevice> device;
    renderer::rhi::RenderResourceHandle pipeline;
};

[[nodiscard]] UiFixture make_fixture() {
    using namespace renderer::rhi;
    RenderDeviceDesc device_desc;
    device_desc.initial_extent = {1280, 720};
    auto device = create_render_device(device_desc);
    assert(device);
    const auto material = core::PrototypeId::parse("base:materials/widget_test");
    assert(material);
    RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/ui.vert"};
    layout.push_constant_ranges.push_back(
        {RenderShaderStageFlags::vertex | RenderShaderStageFlags::fragment, 0,
         sizeof(ChunkPushConstants)});
    assert(device.value()->bind_pipeline_layout(layout));
    constexpr std::array<std::uint32_t, 5> spirv{0x07230203, 0x00010000, 0, 1, 0};
    auto vertex =
        device.value()->create_shader_module({RenderShaderStage::vertex, "widget_vertex"}, spirv);
    auto fragment = device.value()->create_shader_module(
        {RenderShaderStage::fragment, "widget_fragment"}, spirv);
    assert(vertex && fragment);
    RenderGraphicsPipelineDesc pipeline;
    pipeline.vertex_shader = vertex.value().handle;
    pipeline.fragment_shader = fragment.value().handle;
    pipeline.material_id = material.value();
    pipeline.vertex_stride = sizeof(renderer::GpuUiVertex);
    pipeline.vertex_attributes.assign(renderer::gpu_ui_vertex_attributes.begin(),
                                      renderer::gpu_ui_vertex_attributes.end());
    pipeline.cull_mode = RenderCullMode::none;
    pipeline.depth_test_enable = true;
    pipeline.depth_write_enable = false;
    pipeline.depth_compare = RenderCompareOperation::always;
    pipeline.blend_mode = RenderBlendMode::alpha;
    auto created = device.value()->create_graphics_pipeline(pipeline);
    assert(created);
    return {std::move(device).value(), created.value().handle};
}

[[nodiscard]] ui::WidgetDesc root_desc(std::string_view name) {
    ui::WidgetDesc root;
    root.id = ui::widget_id(name);
    root.layout.width = ui::UiSize::fill();
    root.layout.height = ui::UiSize::fill();
    root.layout.mode = ui::UiLayoutMode::overlay;
    root.color = {0.0F, 0.0F, 0.0F, 0.0F};
    return root;
}

void test_layout_grid_clipping_and_stable_ids() {
    ui::WidgetTree tree;
    auto root = root_desc("layout.root");
    assert(tree.add(root));

    ui::WidgetDesc panel;
    panel.id = ui::widget_id("layout.panel");
    panel.parent = root.id;
    panel.layout.width = ui::UiSize::pixels(300.0F);
    panel.layout.height = ui::UiSize::pixels(220.0F);
    panel.layout.horizontal_alignment = ui::UiAlignment::center;
    panel.layout.vertical_alignment = ui::UiAlignment::center;
    panel.layout.mode = ui::UiLayoutMode::grid;
    panel.layout.grid_columns = 3;
    panel.layout.grid_cell_height = 48.0F;
    panel.layout.gap = 4.0F;
    panel.layout.padding = {8.0F, 8.0F, 8.0F, 8.0F};
    panel.layout.clip_children = true;
    assert(tree.add(panel));

    for (std::uint32_t index = 0; index < 7; ++index) {
        ui::WidgetDesc slot;
        slot.id = ui::widget_id("layout.slot." + std::to_string(index));
        slot.parent = panel.id;
        slot.kind = ui::WidgetKind::grid_slot;
        slot.layout.width = ui::UiSize::fill();
        slot.layout.height = ui::UiSize::fill();
        slot.nine_slice = "carved_slot";
        assert(tree.add(std::move(slot)));
    }
    assert(tree.layout({800.0F, 600.0F}));
    const auto panel_rect = tree.rect(panel.id);
    assert(panel_rect);
    assert(panel_rect->x == 250.0F);
    assert(panel_rect->y == 190.0F);
    const auto first = tree.rect(ui::widget_id("layout.slot.0"));
    const auto second = tree.rect(ui::widget_id("layout.slot.1"));
    const auto fourth = tree.rect(ui::widget_id("layout.slot.3"));
    assert(first && second && fourth);
    assert(first->x < second->x);
    assert(first->y < fourth->y);
    const auto first_layout = *first;
    assert(tree.layout({800.0F, 600.0F}));
    assert(tree.rect(ui::widget_id("layout.slot.0")) == first_layout);
    assert(tree.paint_order().size() == 9);
    assert(tree.layout_stats().widget_count == 9);
}

void test_capture_focus_text_and_gameplay_consumption() {
    ui::WidgetTree tree;
    auto root = root_desc("input.root");
    root.layout.mode = ui::UiLayoutMode::row;
    root.layout.padding = {10.0F, 10.0F, 10.0F, 10.0F};
    root.layout.gap = 10.0F;
    root.blocks_gameplay = true;
    assert(tree.add(root));

    ui::WidgetDesc button;
    button.id = ui::widget_id("input.button");
    button.parent = root.id;
    button.kind = ui::WidgetKind::button;
    button.layout.width = ui::UiSize::pixels(100.0F);
    button.layout.height = ui::UiSize::pixels(40.0F);
    button.focusable = true;
    button.pointer_events = true;
    assert(tree.add(button));

    ui::WidgetDesc toggle = button;
    toggle.id = ui::widget_id("input.toggle");
    toggle.kind = ui::WidgetKind::toggle;
    assert(tree.add(toggle));

    ui::WidgetDesc text = button;
    text.id = ui::widget_id("input.text");
    text.kind = ui::WidgetKind::text_input;
    text.text.clear();
    assert(tree.add(text));
    assert(tree.layout({640.0F, 100.0F}));

    ui::UiInputFrame press;
    press.pointer_inside = true;
    press.pointer = {30.0F, 30.0F};
    press.primary_pressed = true;
    press.primary_down = true;
    auto routed = tree.route_input(press);
    assert(routed.consumed.pointer);
    assert(routed.consumed.blocks_gameplay);
    assert(tree.captured_widget() == button.id);
    auto release = press;
    release.primary_pressed = false;
    release.primary_down = false;
    release.primary_released = true;
    routed = tree.route_input(release);
    assert(!tree.captured_widget().is_valid());
    assert(routed.events.back().kind == ui::UiEventKind::clicked);
    assert(routed.events.back().target == button.id);

    ui::UiInputFrame next;
    next.navigation = ui::UiNavigation::next;
    routed = tree.route_input(next);
    assert(routed.consumed.gamepad);
    assert(tree.focused_widget() == toggle.id);
    ui::UiInputFrame activate;
    activate.navigation = ui::UiNavigation::activate;
    routed = tree.route_input(activate);
    assert(routed.events.back().kind == ui::UiEventKind::toggled);
    assert(tree.find(toggle.id)->checked);

    tree.set_focus(text.id);
    ui::UiInputFrame type;
    type.text = "oak";
    routed = tree.route_input(type);
    assert(routed.consumed.text);
    assert(tree.find(text.id)->text == "oak");
    assert(routed.events.back().kind == ui::UiEventKind::text_changed);
}

void test_drag_drop_and_slider() {
    ui::WidgetTree tree;
    auto root = root_desc("drag.root");
    root.layout.mode = ui::UiLayoutMode::row;
    root.layout.padding = {10.0F, 10.0F, 10.0F, 10.0F};
    root.layout.gap = 20.0F;
    assert(tree.add(root));

    ui::WidgetDesc source;
    source.id = ui::widget_id("drag.source");
    source.parent = root.id;
    source.kind = ui::WidgetKind::grid_slot;
    source.layout.width = ui::UiSize::pixels(60.0F);
    source.layout.height = ui::UiSize::pixels(60.0F);
    source.pointer_events = true;
    source.draggable = true;
    source.drag_payload = "inventory:7:2";
    assert(tree.add(source));

    auto target = source;
    target.id = ui::widget_id("drag.target");
    target.draggable = false;
    target.drag_payload.clear();
    target.drop_target = true;
    assert(tree.add(target));

    ui::WidgetDesc slider;
    slider.id = ui::widget_id("drag.slider");
    slider.parent = root.id;
    slider.kind = ui::WidgetKind::slider;
    slider.layout.width = ui::UiSize::pixels(100.0F);
    slider.layout.height = ui::UiSize::pixels(30.0F);
    slider.pointer_events = true;
    slider.minimum_value = 0.0F;
    slider.maximum_value = 10.0F;
    assert(tree.add(slider));
    assert(tree.layout({500.0F, 100.0F}));

    ui::UiInputFrame press;
    press.pointer_inside = true;
    press.pointer = {20.0F, 20.0F};
    press.primary_pressed = true;
    press.primary_down = true;
    (void)tree.route_input(press);
    ui::UiInputFrame move = press;
    move.primary_pressed = false;
    move.pointer = {100.0F, 30.0F};
    auto routed = tree.route_input(move);
    assert(tree.dragging());
    assert(routed.events.back().kind == ui::UiEventKind::drag_started);
    ui::UiInputFrame release = move;
    release.primary_down = false;
    release.primary_released = true;
    release.pointer = {100.0F, 30.0F};
    routed = tree.route_input(release);
    assert(!tree.dragging());
    assert(routed.events.back().kind == ui::UiEventKind::dropped);
    assert(routed.events.back().target == target.id);
    assert(routed.events.back().payload == "inventory:7:2");

    const auto slider_rect = tree.rect(slider.id);
    assert(slider_rect);
    press.pointer = {slider_rect->x + 25.0F, slider_rect->y + 10.0F};
    (void)tree.route_input(press);
    move.pointer = {slider_rect->x + 75.0F, slider_rect->y + 10.0F};
    routed = tree.route_input(move);
    assert(tree.find(slider.id)->value > 7.0F);
    assert(routed.events.back().kind == ui::UiEventKind::value_changed);
}

void test_nine_slice_tooltip_paint_and_batching() {
    auto fixture = make_fixture();
    renderer::UiRenderer renderer(*fixture.device, fixture.pipeline);
    renderer::UiRendererConfig config;
    config.maximum_vertices = 32'768;
    config.maximum_indices = 49'152;
    assert(renderer.initialize({1280, 720}, config));

    ui::WidgetTree tree;
    auto root = root_desc("paint.root");
    root.layout.padding = {20.0F, 20.0F, 20.0F, 20.0F};
    assert(tree.add(root));
    ui::WidgetDesc panel;
    panel.id = ui::widget_id("paint.panel");
    panel.parent = root.id;
    panel.layout.width = ui::UiSize::pixels(320.0F);
    panel.layout.height = ui::UiSize::pixels(180.0F);
    panel.layout.horizontal_alignment = ui::UiAlignment::center;
    panel.layout.vertical_alignment = ui::UiAlignment::center;
    panel.nine_slice = "carved_panel";
    panel.color = {0.34F, 0.18F, 0.075F, 0.96F};
    panel.pointer_events = true;
    panel.tooltip = "Carved oak inventory";
    panel.text = "Inventory";
    assert(tree.add(panel));
    assert(tree.layout({1280.0F, 720.0F}, 1.25F));
    ui::UiInputFrame hover;
    hover.pointer_inside = true;
    hover.pointer = {640.0F, 360.0F};
    (void)tree.route_input(hover);
    auto painted = tree.paint(renderer);
    assert(painted);
    assert(painted.value().nine_slice_quads == 9);
    assert(painted.value().submitted_glyphs > panel.text.size());
    auto frame = renderer.build_frame();
    assert(frame);
    assert(frame.value().stats.submitted_vertices > 0);
    assert(frame.value().stats.draw_calls <= 3);
    assert(renderer.shutdown());
}

void test_two_thousand_widget_layout_budget_probe() {
    ui::WidgetTree tree;
    auto root = root_desc("budget.root");
    root.layout.mode = ui::UiLayoutMode::grid;
    root.layout.grid_columns = 50;
    root.layout.grid_cell_height = 16.0F;
    root.layout.gap = 1.0F;
    assert(tree.add(root));
    for (std::uint32_t index = 0; index < 2'000; ++index) {
        ui::WidgetDesc widget;
        widget.id = ui::widget_id("budget.widget." + std::to_string(index));
        widget.parent = root.id;
        widget.kind = ui::WidgetKind::grid_slot;
        widget.layout.width = ui::UiSize::fill();
        widget.layout.height = ui::UiSize::fill();
        assert(tree.add(std::move(widget)));
    }
    const auto start = std::chrono::steady_clock::now();
    for (std::uint32_t iteration = 0; iteration < 20; ++iteration) {
        assert(tree.layout({1280.0F, 720.0F}));
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    assert(tree.layout_stats().widget_count == 2'001);
    std::cout << "widget_layout_2000_ms=" << elapsed / 20.0 << '\n';
}

} // namespace

int main() {
    test_layout_grid_clipping_and_stable_ids();
    test_capture_focus_text_and_gameplay_consumption();
    test_drag_drop_and_slider();
    test_nine_slice_tooltip_paint_and_batching();
    test_two_thousand_widget_layout_budget_probe();
    return 0;
}
