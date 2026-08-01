// Coverage for the frame graph the renderer submits every frame: resource lifetimes, pass
// ordering, and the barriers the graph derives from declared reads and writes.
//
// These tests are backend independent on purpose. They pin the contract the Vulkan backend has to
// execute, so a graph regression fails here instead of turning into a validation layer error or a
// black frame.

#include "engine/renderer/frame/frame_builder.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace heartstead;
using namespace heartstead::renderer;

[[nodiscard]] std::size_t pass_index_of(const rhi::RenderFramePlan& plan, std::string_view name) {
    const auto found = std::ranges::find_if(
        plan.passes, [name](const rhi::RenderPassDesc& pass) { return pass.name == name; });
    assert(found != plan.passes.end());
    return static_cast<std::size_t>(std::distance(plan.passes.begin(), found));
}

[[nodiscard]] std::optional<rhi::RenderFrameResourceUse>
find_use(const rhi::RenderFrameExecutionPlan& execution_plan, std::string_view resource,
         std::size_t pass_index) {
    for (const auto& use : execution_plan.resource_uses) {
        if (use.resource_name == resource && use.pass_index == pass_index) {
            return use;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool has_transition(const rhi::RenderFrameExecutionPlan& execution_plan,
                                  std::string_view resource, rhi::RenderResourceState before,
                                  rhi::RenderResourceState after) {
    return std::ranges::any_of(
        execution_plan.transitions,
        [resource, before, after](const rhi::RenderFrameResourceTransition& transition) {
            return transition.resource_name == resource && transition.before_state == before &&
                   transition.after_state == after;
        });
}

[[nodiscard]] rhi::RenderFramePlan build_hdr_plan(rhi::RenderExtent extent = {1280, 720}) {
    FrameBuilder builder(extent, rhi::ClearColor{0.1F, 0.2F, 0.3F, 1.0F});
    auto plan = builder.build_plan();
    assert(plan);
    return std::move(plan).value();
}

// The scene target must be a transient linear HDR image, and the swapchain image must stay
// external. Getting this wrong is what silently reintroduces per-shader gamma encoding.
void test_hdr_plan_declares_a_linear_scene_target() {
    const auto plan = build_hdr_plan();
    assert(plan.validate());

    const auto* scene = plan.find_resource(rhi::scene_color_resource_name);
    assert(scene != nullptr);
    assert(scene->format == rhi::RenderImageFormat::rgba16_sfloat);
    assert(rhi::is_hdr_format(scene->format));
    assert(!rhi::is_depth_format(scene->format));
    assert(scene->lifetime == rhi::RenderResourceLifetime::transient);

    const auto* depth = plan.find_resource(rhi::depth_resource_name);
    assert(depth != nullptr);
    assert(rhi::is_depth_format(depth->format));
    assert(depth->lifetime == rhi::RenderResourceLifetime::transient);

    const auto* output = plan.find_resource(rhi::output_resource_name);
    assert(output != nullptr);
    assert(output->lifetime == rhi::RenderResourceLifetime::external);
    assert(!rhi::is_hdr_format(output->format));

    for (const auto& resource : plan.resources) {
        if (resource.name.starts_with("shadow_cascade_")) {
            assert(resource.extent.width == 2048);
            assert(resource.extent.height == 2048);
            assert(rhi::is_depth_format(resource.format));
        } else if (resource.name.starts_with("local_shadow_")) {
            assert(resource.extent.width == 1024);
            assert(resource.extent.height == 1024);
            assert(rhi::is_depth_format(resource.format));
        } else {
            assert(resource.extent.width == plan.extent.width);
            assert(resource.extent.height == plan.extent.height);
        }
    }
}

// The core invariant of the HDR pipeline: nothing that shades the world is allowed to write to
// the presentable image. Only the tone mapping pass and the UI pass may.
void test_only_tone_map_and_ui_write_the_presentable_image() {
    const auto plan = build_hdr_plan();
    const std::string output{rhi::output_resource_name};

    for (const auto& pass : plan.passes) {
        const auto writes_output = std::ranges::find(pass.writes, output) != pass.writes.end();
        switch (pass.kind) {
        case rhi::RenderPassKind::clear:
        case rhi::RenderPassKind::world:
        case rhi::RenderPassKind::debug:
        case rhi::RenderPassKind::compute:
            assert(!writes_output);
            break;
        case rhi::RenderPassKind::post_process:
            assert(writes_output == (pass.name == "tone_map"));
            break;
        case rhi::RenderPassKind::ui:
            assert(writes_output);
            break;
        case rhi::RenderPassKind::present:
            assert(!writes_output);
            break;
        }
    }
}

void test_hdr_pass_order_matches_the_published_indices() {
    const auto plan = build_hdr_plan();
    assert(plan.passes.size() == hdr_pass_index::count);
    const auto* motion = plan.find_resource(rhi::scene_motion_resource_name);
    assert(motion != nullptr);
    assert(motion->format == rhi::RenderImageFormat::rg16_sfloat);
    assert(motion->lifetime == rhi::RenderResourceLifetime::transient);

    assert(pass_index_of(plan, "shadow_cascade_0") == hdr_pass_index::shadow_0);
    assert(pass_index_of(plan, "shadow_cascade_1") == hdr_pass_index::shadow_1);
    assert(pass_index_of(plan, "shadow_cascade_2") == hdr_pass_index::shadow_2);
    assert(pass_index_of(plan, "shadow_cascade_3") == hdr_pass_index::shadow_3);
    assert(pass_index_of(plan, "local_shadow_0") == hdr_pass_index::local_shadow_0);
    assert(pass_index_of(plan, "local_shadow_1") == hdr_pass_index::local_shadow_1);
    assert(pass_index_of(plan, "sky") == hdr_pass_index::sky);
    assert(pass_index_of(plan, "opaque_terrain") == hdr_pass_index::opaque_terrain);
    assert(pass_index_of(plan, "alpha_tested_terrain") == hdr_pass_index::alpha_tested_terrain);
    assert(pass_index_of(plan, "rich_static_instances") == hdr_pass_index::rich_static_instances);
    assert(pass_index_of(plan, "transparent_terrain") == hdr_pass_index::transparent_terrain);
    assert(pass_index_of(plan, "debug") == hdr_pass_index::debug);
    assert(pass_index_of(plan, "tone_map") == hdr_pass_index::tone_map);
    assert(pass_index_of(plan, "ui") == hdr_pass_index::ui);
    assert(pass_index_of(plan, "present") == hdr_pass_index::present);

    // Tone mapping has to run after every scene pass and before UI composition, otherwise the UI
    // is tone mapped along with the world.
    assert(hdr_pass_index::debug < hdr_pass_index::tone_map);
    assert(hdr_pass_index::tone_map < hdr_pass_index::ui);
    assert(plan.passes[hdr_pass_index::tone_map].kind == rhi::RenderPassKind::post_process);
    assert(plan.passes.back().kind == rhi::RenderPassKind::present);
    assert(plan.has_present_pass());
    assert(plan.pass_count(rhi::RenderPassKind::post_process) == 5);
    assert(plan.pass_count(rhi::RenderPassKind::present) == 1);
    for (std::size_t pass = hdr_pass_index::sky; pass <= hdr_pass_index::debug; ++pass) {
        if (pass == hdr_pass_index::ssao || pass == hdr_pass_index::ao_composite) {
            continue;
        }
        assert(std::ranges::find(plan.passes[pass].writes,
                                 rhi::scene_motion_resource_name) !=
               plan.passes[pass].writes.end());
    }

    for (std::size_t shadow_pass = 0; shadow_pass < 6; ++shadow_pass) {
        const auto& pass = plan.passes[shadow_pass];
        assert(pass.writes.size() == 1);
        assert(pass.writes.front().starts_with("shadow_cascade_") ||
               pass.writes.front().starts_with("local_shadow_"));
        assert(std::ranges::none_of(pass.writes, [&](const std::string& name) {
            const auto* resource = plan.find_resource(name);
            return resource != nullptr && !rhi::is_depth_format(resource->format);
        }));
    }
}

// Lifetime tracking: the graph must move the scene target out of colour attachment state before
// the tone mapping pass samples it, and must transition the swapchain image to present at the end.
void test_graph_derives_scene_target_barriers() {
    const auto plan = build_hdr_plan();
    auto execution_plan = plan.build_execution_plan();
    assert(execution_plan);
    const auto& execution = execution_plan.value();

    assert(execution.ordered_passes.size() == hdr_pass_index::count);
    assert(execution.present_pass_count == 1);

    const auto scene_write =
        find_use(execution, rhi::scene_color_resource_name, hdr_pass_index::sky);
    assert(scene_write.has_value());
    assert(scene_write->required_state == rhi::RenderResourceState::color_attachment_write);
    assert(scene_write->resource_lifetime == rhi::RenderResourceLifetime::transient);

    const auto scene_read =
        find_use(execution, rhi::scene_color_resource_name, hdr_pass_index::ao_composite);
    assert(scene_read.has_value());
    assert(scene_read->access == rhi::RenderResourceAccess::read);
    assert(scene_read->required_state == rhi::RenderResourceState::shader_read);

    // Written as a colour attachment, then sampled: that edge must produce a barrier.
    assert(has_transition(execution, rhi::scene_color_resource_name,
                          rhi::RenderResourceState::color_attachment_read_write,
                          rhi::RenderResourceState::shader_read));
    const auto antialiased_read = find_use(execution, "scene_aa", hdr_pass_index::tone_map);
    assert(antialiased_read.has_value());
    assert(antialiased_read->required_state == rhi::RenderResourceState::shader_read);

    const auto output_write =
        find_use(execution, rhi::output_resource_name, hdr_pass_index::tone_map);
    assert(output_write.has_value());
    assert(output_write->required_state == rhi::RenderResourceState::color_attachment_write);

    const auto present_use =
        find_use(execution, rhi::output_resource_name, hdr_pass_index::present);
    assert(present_use.has_value());
    assert(present_use->access == rhi::RenderResourceAccess::present);
    assert(present_use->required_state == rhi::RenderResourceState::present);
    assert(has_transition(execution, rhi::output_resource_name, rhi::RenderResourceState::external,
                          rhi::RenderResourceState::color_attachment_write));

    // Every dependency has to point forwards. A backwards edge means the ordering is wrong.
    for (const auto& dependency : execution.dependencies) {
        assert(dependency.source_pass_index <= dependency.destination_pass_index);
    }
    assert(!execution.dependencies.empty());

    // The tone mapping pass reads scene colour only. It must not pull the depth target in.
    assert(!find_use(execution, rhi::depth_resource_name, hdr_pass_index::tone_map).has_value());
}

void test_hdr_submission_carries_validated_exposure() {
    FrameBuilder builder({1280, 720});

    assert(builder.set_exposure({0.75F, rhi::RenderToneMapping::khronos_pbr_neutral, 1.0F}));
    assert(builder.exposure().tone_mapping == rhi::RenderToneMapping::khronos_pbr_neutral);

    // Out of range or non-finite exposure must be rejected at the boundary, not passed to a
    // shader where it becomes a NaN frame.
    assert(!builder.set_exposure({64.0F, rhi::RenderToneMapping::aces_approx, 1.0F}));
    assert(!builder.set_exposure(
        {std::numeric_limits<float>::quiet_NaN(), rhi::RenderToneMapping::aces_approx, 1.0F}));
    assert(!builder.set_exposure({0.0F, rhi::RenderToneMapping::aces_approx, 0.0F}));
    // A rejected value must not have been applied.
    assert(builder.exposure().tone_mapping == rhi::RenderToneMapping::khronos_pbr_neutral);
    assert(std::abs(builder.exposure().exposure_stops - 0.75F) < 1.0e-6F);

    RenderCamera camera;
    auto submission = builder.build(camera, RenderCommandLists{});
    assert(submission);
    assert(std::abs(submission.value().exposure.exposure_stops - 0.75F) < 1.0e-6F);
    assert(rhi::validate_render_frame_submission_shape(submission.value()));
}

void test_tone_map_push_constants_match_the_shader_layout() {
    const auto neutral = rhi::make_tone_map_push_constants({});
    assert(std::abs(neutral.exposure_scale - 1.0F) < 1.0e-6F);
    assert(neutral.tone_mapping == static_cast<std::uint32_t>(rhi::RenderToneMapping::aces_approx));

    // Exposure is expressed in stops, so one stop has to double the scale.
    const auto one_stop =
        rhi::make_tone_map_push_constants({1.0F, rhi::RenderToneMapping::reinhard, 2.0F});
    assert(std::abs(one_stop.exposure_scale - 2.0F) < 1.0e-6F);
    assert(std::abs(one_stop.white_point - 2.0F) < 1.0e-6F);
    assert(one_stop.tone_mapping == static_cast<std::uint32_t>(rhi::RenderToneMapping::reinhard));

    const auto minus_two =
        rhi::make_tone_map_push_constants({-2.0F, rhi::RenderToneMapping::none, 1.0F});
    assert(std::abs(minus_two.exposure_scale - 0.25F) < 1.0e-6F);

    assert(rhi::render_tone_mapping_name(rhi::RenderToneMapping::aces_approx) == "aces_approx");
    assert(rhi::render_image_format_name(rhi::RenderImageFormat::rgba16_sfloat) == "rgba16_sfloat");
    assert(rhi::render_image_format_bytes_per_pixel(rhi::RenderImageFormat::rgba16_sfloat) == 8);
}

void test_hdr_plan_resizes_every_resource() {
    const auto plan = build_hdr_plan({640, 360});
    assert(plan.extent.width == 640);
    assert(plan.extent.height == 360);
    for (const auto& resource : plan.resources) {
        if (resource.name.starts_with("shadow_cascade_")) {
            assert(resource.extent.width == 2048);
            assert(resource.extent.height == 2048);
        } else if (resource.name.starts_with("local_shadow_")) {
            assert(resource.extent.width == 1024);
            assert(resource.extent.height == 1024);
        } else {
            assert(resource.extent.width == 640);
            assert(resource.extent.height == 360);
        }
    }
    assert(plan.validate());
    assert(plan.build_execution_plan());
}

} // namespace

int main() {
    test_hdr_plan_declares_a_linear_scene_target();
    test_only_tone_map_and_ui_write_the_presentable_image();
    test_hdr_pass_order_matches_the_published_indices();
    test_graph_derives_scene_target_barriers();
    test_hdr_submission_carries_validated_exposure();
    test_tone_map_push_constants_match_the_shader_layout();
    test_hdr_plan_resizes_every_resource();
    return 0;
}
