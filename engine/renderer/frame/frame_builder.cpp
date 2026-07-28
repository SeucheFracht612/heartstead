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

core::Result<rhi::RenderFramePlan> FrameBuilder::build_plan() const {
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
    append(6, commands.ui_draws);
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
