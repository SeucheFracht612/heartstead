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

void FrameBuilder::set_color_pipeline(FrameColorPipeline pipeline) noexcept {
    color_pipeline_ = pipeline;
}

FrameColorPipeline FrameBuilder::color_pipeline() const noexcept {
    return color_pipeline_;
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

core::Result<rhi::RenderFramePlan> FrameBuilder::build_plan() const {
    return color_pipeline_ == FrameColorPipeline::linear_hdr ? build_linear_hdr_plan()
                                                            : build_legacy_ldr_plan();
}

core::Result<rhi::RenderFramePlan> FrameBuilder::build_legacy_ldr_plan() const {
    using namespace rhi;
    auto extent_status = validate_render_extent(extent_);
    if (!extent_status) {
        return core::Result<RenderFramePlan>::failure(extent_status.error().code,
                                                      extent_status.error().message);
    }

    RenderFramePlanBuilder builder(extent_);
    auto status = builder.add_resource(
        {"output", extent_, RenderResourceLifetime::external, RenderImageFormat::rgba8_unorm});
    if (!status) {
        return core::Result<RenderFramePlan>::failure(status.error().code, status.error().message);
    }
    status = builder.add_resource(
        {"depth", extent_, RenderResourceLifetime::transient, RenderImageFormat::d32_sfloat});
    if (!status) {
        return core::Result<RenderFramePlan>::failure(status.error().code, status.error().message);
    }
    status = builder.add_pass(
        {"sky", RenderPassKind::clear, {}, {"output", "depth"}, clear_color_, false});
    if (!status) {
        return core::Result<RenderFramePlan>::failure(status.error().code, status.error().message);
    }
    status = builder.add_pass(
        {"opaque_terrain", RenderPassKind::world, {"output"}, {"output", "depth"}, {}, false});
    if (!status) {
        return core::Result<RenderFramePlan>::failure(status.error().code, status.error().message);
    }
    status = builder.add_pass({"alpha_tested_terrain",
                               RenderPassKind::world,
                               {"output", "depth"},
                               {"output", "depth"},
                               {},
                               false});
    if (!status) {
        return core::Result<RenderFramePlan>::failure(status.error().code, status.error().message);
    }
    status = builder.add_pass({"rich_static_instances",
                               RenderPassKind::world,
                               {"output", "depth"},
                               {"output", "depth"},
                               {},
                               false});
    if (!status) {
        return core::Result<RenderFramePlan>::failure(status.error().code, status.error().message);
    }
    status = builder.add_pass({"transparent_terrain",
                               RenderPassKind::world,
                               {"output", "depth"},
                               {"output", "depth"},
                               {},
                               false});
    if (!status) {
        return core::Result<RenderFramePlan>::failure(status.error().code, status.error().message);
    }
    status = builder.add_pass(
        {"debug", RenderPassKind::debug, {"output", "depth"}, {"output", "depth"}, {}, false});
    if (!status) {
        return core::Result<RenderFramePlan>::failure(status.error().code, status.error().message);
    }
    status = builder.add_pass(
        {"ui", RenderPassKind::ui, {"output", "depth"}, {"output", "depth"}, {}, false});
    if (!status) {
        return core::Result<RenderFramePlan>::failure(status.error().code, status.error().message);
    }
    status = builder.add_pass({"present", RenderPassKind::present, {"output"}, {}, {}, true});
    if (!status) {
        return core::Result<RenderFramePlan>::failure(status.error().code, status.error().message);
    }
    return builder.build();
}

core::Result<rhi::RenderFramePlan> FrameBuilder::build_linear_hdr_plan() const {
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
    status = builder.add_pass({"sky", RenderPassKind::clear, {}, {scene, depth}, clear_color_,
                               false});
    if (!status) {
        return fail(status);
    }
    status =
        builder.add_pass({"opaque_terrain", RenderPassKind::world, {scene}, {scene, depth}, {},
                          false});
    if (!status) {
        return fail(status);
    }
    for (const auto* name : {"alpha_tested_terrain", "rich_static_instances",
                             "transparent_terrain"}) {
        status = builder.add_pass(
            {name, RenderPassKind::world, {scene, depth}, {scene, depth}, {}, false});
        if (!status) {
            return fail(status);
        }
    }
    status = builder.add_pass(
        {"debug", RenderPassKind::debug, {scene, depth}, {scene, depth}, {}, false});
    if (!status) {
        return fail(status);
    }

    // The single point where exposure, the tone curve, and the display transfer function are
    // applied.
    status = builder.add_pass(
        {"tone_map", RenderPassKind::post_process, {scene}, {output}, {}, false});
    if (!status) {
        return fail(status);
    }

    // UI composites in display space, on top of the tone mapped image.
    status = builder.add_pass({"ui", RenderPassKind::ui, {output}, {output}, {}, false});
    if (!status) {
        return fail(status);
    }
    status = builder.add_pass({"present", RenderPassKind::present, {output}, {}, {}, true});
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
    // The tone mapping pass owns index 6 in the HDR graph and records no external draws, so UI
    // shifts one slot. The legacy path keeps UI at index 6.
    append(color_pipeline_ == FrameColorPipeline::linear_hdr ? hdr_pass_index::ui : 6,
           commands.ui_draws);
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
