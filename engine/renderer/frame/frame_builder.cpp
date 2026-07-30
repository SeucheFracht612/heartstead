#include "engine/renderer/frame/frame_builder.hpp"

#include <utility>

namespace heartstead::renderer {

FrameBuilder::FrameBuilder(rhi::RenderExtent extent, rhi::ClearColor clear_color)
    : extent_(extent), clear_color_(clear_color) {}

core::Status FrameBuilder::resize(rhi::RenderExtent extent) {
    auto status = rhi::validate_render_extent(extent);
    if (!status) {
        return status;
    }
    extent_ = extent;
    return core::Status::ok();
}

void FrameBuilder::set_clear_color(rhi::ClearColor clear_color) noexcept {
    clear_color_ = clear_color;
}

core::Status FrameBuilder::set_exposure(rhi::RenderExposureSettings exposure) {
    auto status = rhi::validate_render_exposure(exposure);
    if (!status) {
        return status;
    }
    exposure_ = exposure;
    return core::Status::ok();
}

rhi::RenderExposureSettings FrameBuilder::exposure() const noexcept {
    return exposure_;
}

void FrameBuilder::set_tone_map_pipeline(rhi::RenderResourceHandle pipeline) noexcept {
    tone_map_pipeline_ = pipeline;
}

rhi::RenderResourceHandle FrameBuilder::tone_map_pipeline() const noexcept {
    return tone_map_pipeline_;
}

core::Result<rhi::RenderFramePlan> FrameBuilder::build_plan() const {
    using namespace rhi;
    auto extent_status = validate_render_extent(extent_);
    if (!extent_status) {
        return core::Result<RenderFramePlan>::failure(extent_status.error().code,
                                                      extent_status.error().message);
    }

    RenderFramePlanBuilder builder(extent_);
    core::Status status = core::Status::ok();
    const auto fail = [](const core::Status& failed) {
        return core::Result<RenderFramePlan>::failure(failed.error().code, failed.error().message);
    };

    // Linear radiance target. World shading never touches the swapchain image.
    status = builder.add_resource({std::string(scene_color_resource_name), extent_,
                                   RenderResourceLifetime::transient,
                                   RenderImageFormat::rgba16_sfloat});
    if (!status) {
        return fail(status);
    }
    status = builder.add_resource({std::string(depth_resource_name), extent_,
                                   RenderResourceLifetime::transient,
                                   RenderImageFormat::d32_sfloat});
    if (!status) {
        return fail(status);
    }
    status = builder.add_resource({std::string(output_resource_name), extent_,
                                   RenderResourceLifetime::external,
                                   RenderImageFormat::rgba8_unorm});
    if (!status) {
        return fail(status);
    }

    const std::string scene{scene_color_resource_name};
    const std::string depth{depth_resource_name};
    const std::string output{output_resource_name};

    // Scene passes, in the order the frame records them. Every one of these writes linear
    // radiance; none of them encodes a display transfer function.
    status = builder.add_pass({.name = "sky",
                               .kind = RenderPassKind::clear,
                               .writes = {scene, depth},
                               .clear_color = clear_color_});
    if (!status) {
        return fail(status);
    }
    status = builder.add_pass({.name = "opaque_terrain",
                               .kind = RenderPassKind::world,
                               .reads = {scene},
                               .writes = {scene, depth}});
    if (!status) {
        return fail(status);
    }
    for (const auto* name : {"alpha_tested_terrain", "rich_static_instances",
                             "transparent_terrain"}) {
        status = builder.add_pass({.name = name,
                                   .kind = RenderPassKind::world,
                                   .reads = {scene, depth},
                                   .writes = {scene, depth}});
        if (!status) {
            return fail(status);
        }
    }
    status = builder.add_pass({.name = "debug",
                               .kind = RenderPassKind::debug,
                               .reads = {scene, depth},
                               .writes = {scene, depth}});
    if (!status) {
        return fail(status);
    }

    // The single point where exposure, the tone curve, and the display transfer function are
    // applied.
    // Samples the linear scene target through the tone map material's scene_hdr binding. The
    // resource has no device handle, so the graph resolves it by name each frame.
    status = builder.add_pass(
        {.name = "tone_map",
         .kind = RenderPassKind::post_process,
         .reads = {scene},
         .writes = {output},
         .sampled_resources = {{.binding_name = "scene_hdr", .resource_name = scene}}});
    if (!status) {
        return fail(status);
    }

    // UI composites in display space, on top of the tone mapped image.
    status = builder.add_pass({.name = "ui",
                               .kind = RenderPassKind::ui,
                               .reads = {output},
                               .writes = {output}});
    if (!status) {
        return fail(status);
    }
    status = builder.add_pass({.name = "present",
                               .kind = RenderPassKind::present,
                               .reads = {output},
                               .presents = true});
    if (!status) {
        return fail(status);
    }
    return builder.build();
}

core::Result<rhi::RenderFrameSubmission>
FrameBuilder::build(const RenderCamera& camera, RenderCommandLists commands,
                    rhi::RenderEnvironmentData environment) const {
    auto plan = build_plan();
    if (!plan) {
        return core::Result<rhi::RenderFrameSubmission>::failure(plan.error().code,
                                                                 plan.error().message);
    }

    rhi::RenderFrameSubmission result;
    result.plan = std::move(plan).value();
    result.camera.view_projection = camera.view_projection;
    result.environment = environment;
    result.exposure = exposure_;
    const auto append = [&result](std::size_t pass_index,
                                  std::vector<rhi::RenderDrawCommand>& draws) {
        if (!draws.empty()) {
            result.pass_commands.push_back({pass_index, std::move(draws)});
        }
    };
    append(0, commands.sky_draws);
    append(1, commands.opaque_terrain_draws);
    append(2, commands.alpha_tested_terrain_draws);
    append(3, commands.rich_instance_draws);
    append(4, commands.transparent_terrain_draws);
    append(5, commands.debug_draws);
    // The tone mapping pass takes no caller-supplied geometry: it is a fullscreen triangle the
    // graph owns, generated in the vertex shader, so the draw is synthesized here rather than
    // being something every caller has to remember to submit.
    std::vector<rhi::RenderDrawCommand> tone_map_draws;
    if (tone_map_pipeline_.is_valid()) {
        rhi::RenderDrawCommand tone_map_draw;
        tone_map_draw.pipeline = tone_map_pipeline_;
        tone_map_draw.vertex_count = 3;
        tone_map_draw.instance_count = 1;
        tone_map_draws.push_back(tone_map_draw);
        append(hdr_pass_index::tone_map, tone_map_draws);
    }
    append(hdr_pass_index::ui, commands.ui_draws);
    auto status = rhi::validate_render_frame_submission_shape(result);
    if (!status) {
        return core::Result<rhi::RenderFrameSubmission>::failure(status.error().code,
                                                                 status.error().message);
    }
    return core::Result<rhi::RenderFrameSubmission>::success(std::move(result));
}

rhi::RenderExtent FrameBuilder::extent() const noexcept {
    return extent_;
}

} // namespace heartstead::renderer
