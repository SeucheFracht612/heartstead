#include "engine/renderer/frame/frame_builder.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace heartstead::renderer {

core::Status FrameImageQualitySettings::validate() const noexcept {
    if (!std::isfinite(render_scale) || render_scale < 0.5F || render_scale > 1.0F) {
        return core::Status::failure("frame_builder.invalid_render_scale",
                                     "internal render scale must be in the range 0.5..1.0");
    }
    return core::Status::ok();
}

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

void FrameBuilder::set_image_quality_pipelines(rhi::RenderResourceHandle ssao,
                                               rhi::RenderResourceHandle ao_composite,
                                               rhi::RenderResourceHandle anti_alias,
                                               rhi::RenderResourceHandle bloom) noexcept {
    ssao_pipeline_ = ssao;
    ao_composite_pipeline_ = ao_composite;
    anti_alias_pipeline_ = anti_alias;
    bloom_pipeline_ = bloom;
}

core::Status FrameBuilder::set_image_quality_settings(FrameImageQualitySettings settings) {
    auto status = settings.validate();
    if (!status) {
        return status;
    }
    image_quality_ = settings;
    return core::Status::ok();
}

FrameImageQualitySettings FrameBuilder::image_quality_settings() const noexcept {
    return image_quality_;
}

void FrameBuilder::update_exposure_adaptation(float scene_luminance, float delta_seconds) noexcept {
    if (!exposure_.automatic_exposure || !std::isfinite(scene_luminance) ||
        !std::isfinite(delta_seconds) || delta_seconds <= 0.0F) {
        return;
    }
    const auto desired =
        std::clamp(std::log2(exposure_.target_luminance / std::max(scene_luminance, 1.0e-4F)),
                   exposure_.minimum_auto_stops, exposure_.maximum_auto_stops);
    const auto alpha =
        1.0F - std::exp(-exposure_.adaptation_speed * std::min(delta_seconds, 0.25F));
    adapted_exposure_stops_ += (desired - adapted_exposure_stops_) * alpha;
}

rhi::RenderResourceHandle FrameBuilder::tone_map_pipeline() const noexcept {
    return tone_map_pipeline_;
}

core::Status FrameBuilder::set_shadow_resolution(std::uint32_t resolution) {
    if (resolution < 256U || resolution > 8192U || (resolution & (resolution - 1U)) != 0U) {
        return core::Status::failure("frame_builder.invalid_shadow_resolution",
                                     "shadow resolution must be a power of two in 256..8192");
    }
    shadow_resolution_ = resolution;
    return core::Status::ok();
}

core::Status FrameBuilder::set_directional_shadow_cascade_count(std::uint32_t count) {
    if (count == 0U || count > 4U) {
        return core::Status::failure("frame_builder.invalid_shadow_cascade_count",
                                     "directional shadow cascade count must be in 1..4");
    }
    directional_shadow_cascade_count_ = count;
    return core::Status::ok();
}

core::Status FrameBuilder::set_local_shadow_resolution(std::uint32_t resolution) {
    if (resolution != 1U &&
        (resolution < 256U || resolution > 4096U || (resolution & (resolution - 1U)) != 0U)) {
        return core::Status::failure(
            "frame_builder.invalid_local_shadow_resolution",
            "local shadow resolution must be one or a power of two in 256..4096");
    }
    local_shadow_resolution_ = resolution;
    return core::Status::ok();
}

core::Result<rhi::RenderFramePlan> FrameBuilder::build_plan() const {
    using namespace rhi;
    auto extent_status = validate_render_extent(extent_);
    if (!extent_status) {
        return core::Result<RenderFramePlan>::failure(extent_status.error().code,
                                                      extent_status.error().message);
    }

    RenderFramePlanBuilder builder(extent_);
    const rhi::RenderExtent scene_extent{
        std::max(1U, static_cast<std::uint32_t>(
                         std::lround(static_cast<double>(extent_.width) *
                                     image_quality_.render_scale))),
        std::max(1U, static_cast<std::uint32_t>(
                         std::lround(static_cast<double>(extent_.height) *
                                     image_quality_.render_scale)))};
    core::Status status = core::Status::ok();
    const auto fail = [](const core::Status& failed) {
        return core::Result<RenderFramePlan>::failure(failed.error().code, failed.error().message);
    };

    // Linear radiance target. World shading never touches the swapchain image.
    status =
        builder.add_resource({std::string(scene_color_resource_name), scene_extent,
                              RenderResourceLifetime::transient, RenderImageFormat::rgba16_sfloat});
    if (!status) {
        return fail(status);
    }
    status = builder.add_resource({std::string(scene_motion_resource_name), scene_extent,
                                   RenderResourceLifetime::transient,
                                   RenderImageFormat::rg16_sfloat});
    if (!status) {
        return fail(status);
    }
    for (std::uint32_t cascade = 0; cascade < 4U; ++cascade) {
        const auto cascade_extent = cascade < directional_shadow_cascade_count_
                                        ? RenderExtent{shadow_resolution_, shadow_resolution_}
                                        : RenderExtent{1, 1};
        status = builder.add_resource({"shadow_cascade_" + std::to_string(cascade),
                                       cascade_extent,
                                       RenderResourceLifetime::transient,
                                       RenderImageFormat::d32_sfloat});
        if (!status) {
            return fail(status);
        }
    }
    for (std::uint32_t slot = 0; slot < local_shadow_map_count; ++slot) {
        status = builder.add_resource({"local_shadow_" + std::to_string(slot),
                                       {local_shadow_resolution_, local_shadow_resolution_},
                                       RenderResourceLifetime::transient,
                                       RenderImageFormat::d32_sfloat});
        if (!status) {
            return fail(status);
        }
    }
    status =
        builder.add_resource({std::string(depth_resource_name), scene_extent,
                              RenderResourceLifetime::transient, RenderImageFormat::d32_sfloat});
    if (!status) {
        return fail(status);
    }
    status =
        builder.add_resource({std::string(output_resource_name), extent_,
                              RenderResourceLifetime::external, RenderImageFormat::rgba8_unorm});
    if (!status) {
        return fail(status);
    }
    status = builder.add_resource(
        {"scene_ao", scene_extent, RenderResourceLifetime::transient, RenderImageFormat::r8_unorm});
    if (!status) {
        return fail(status);
    }
    status = builder.add_resource(
        {"scene_depth_copy", scene_extent, RenderResourceLifetime::transient,
         RenderImageFormat::rg16_sfloat});
    if (!status) {
        return fail(status);
    }
    for (const auto* name : {"scene_grounded", "scene_aa"}) {
        status = builder.add_resource(
            {name, scene_extent, RenderResourceLifetime::transient, RenderImageFormat::rgba16_sfloat});
        if (!status) {
            return fail(status);
        }
    }
    // The tone-map layout always carries a bloom binding.  When bloom is disabled, a one-texel
    // black image preserves that descriptor contract without clearing and sampling another
    // full-resolution HDR target every frame.
    const RenderExtent bloom_extent = image_quality_.bloom ? scene_extent : RenderExtent{1, 1};
    status = builder.add_resource({"bloom_hdr", bloom_extent,
                                   RenderResourceLifetime::transient,
                                   RenderImageFormat::rgba16_sfloat});
    if (!status) {
        return fail(status);
    }

    const std::string scene{scene_color_resource_name};
    const std::string motion{scene_motion_resource_name};
    const std::string depth{depth_resource_name};
    const std::string output{output_resource_name};
    std::vector<RenderPassSampledResource> shadow_samples;
    std::vector<std::string> world_shadow_reads;
    for (std::uint32_t cascade = 0; cascade < 4U; ++cascade) {
        const auto name = "shadow_cascade_" + std::to_string(cascade);
        shadow_samples.push_back({name, name, true});
        world_shadow_reads.push_back(name);
        status = builder.add_pass({.name = name, .kind = RenderPassKind::world, .writes = {name}});
        if (!status) {
            return fail(status);
        }
    }
    for (std::uint32_t slot = 0; slot < local_shadow_map_count; ++slot) {
        const auto name = "local_shadow_" + std::to_string(slot);
        shadow_samples.push_back({name, name, true});
        world_shadow_reads.push_back(name);
        status = builder.add_pass({.name = name, .kind = RenderPassKind::world, .writes = {name}});
        if (!status) {
            return fail(status);
        }
    }

    // Scene passes, in the order the frame records them. Every one of these writes linear
    // radiance; none of them encodes a display transfer function.
    status = builder.add_pass({.name = "sky",
                               .kind = RenderPassKind::clear,
                               .writes = {scene, motion, depth},
                               .clear_color = clear_color_});
    if (!status) {
        return fail(status);
    }
    auto opaque_reads = world_shadow_reads;
    opaque_reads.push_back(scene);
    opaque_reads.push_back(motion);
    status = builder.add_pass({.name = "opaque_terrain",
                               .kind = RenderPassKind::world,
                               .reads = std::move(opaque_reads),
                               .writes = {scene, motion, depth},
                               .sampled_resources = shadow_samples});
    if (!status) {
        return fail(status);
    }
    for (const auto* name : {"alpha_tested_terrain", "rich_static_instances"}) {
        auto reads = world_shadow_reads;
        reads.push_back(scene);
        reads.push_back(motion);
        reads.push_back(depth);
        status = builder.add_pass({.name = name,
                                   .kind = RenderPassKind::world,
                                   .reads = std::move(reads),
                                   .writes = {scene, motion, depth},
                                   .sampled_resources = shadow_samples});
        if (!status) {
            return fail(status);
        }
    }
    status = image_quality_.ambient_occlusion
                 ? builder.add_pass({.name = "ssao",
                                     .kind = RenderPassKind::post_process,
                                     .reads = {depth},
                                     .writes = {"scene_ao", "scene_depth_copy"},
                                     .sampled_resources = {{"scene_depth", depth, false}}})
                 : builder.add_pass({.name = "ssao",
                                     .kind = RenderPassKind::post_process,
                                     .reads = {depth}});
    if (!status) {
        return fail(status);
    }
    status = image_quality_.ambient_occlusion
                 ? builder.add_pass(
                       {.name = "ao_composite",
                        .kind = RenderPassKind::post_process,
                        .reads = {scene, "scene_ao"},
                        .writes = {"scene_grounded"},
                        .sampled_resources = {{"scene_hdr", scene, false},
                                              {"scene_ao", "scene_ao", false}}})
                 : builder.add_pass({.name = "ao_composite",
                                     .kind = RenderPassKind::post_process,
                                     .reads = {scene},
                                     .writes = {scene}});
    if (!status) {
        return fail(status);
    }
    auto transparent_reads = world_shadow_reads;
    const std::string grounded = image_quality_.ambient_occlusion ? "scene_grounded" : scene;
    transparent_reads.push_back(grounded);
    transparent_reads.push_back(depth);
    if (image_quality_.ambient_occlusion) {
        transparent_reads.push_back("scene_depth_copy");
    }
    transparent_reads.push_back(motion);
    auto transparent_samples = shadow_samples;
    transparent_samples.push_back(
        {"scene_depth", image_quality_.ambient_occlusion ? "scene_depth_copy" : depth, false});
    status = builder.add_pass({.name = "transparent_terrain",
                               .kind = RenderPassKind::world,
                               .reads = std::move(transparent_reads),
                               .writes = {grounded, motion, depth},
                               .sampled_resources = std::move(transparent_samples)});
    if (!status) {
        return fail(status);
    }
    status = builder.add_pass({.name = "debug",
                               .kind = RenderPassKind::debug,
                               .reads = {grounded, motion, depth},
                               .writes = {grounded, motion, depth}});
    if (!status) {
        return fail(status);
    }
    status = image_quality_.anti_aliasing
                 ? builder.add_pass({.name = "anti_alias",
                                     .kind = RenderPassKind::post_process,
                                     .reads = {grounded},
                                     .writes = {"scene_aa"},
                                     .sampled_resources = {{"input_hdr", grounded, false}}})
                 : builder.add_pass({.name = "anti_alias",
                                     .kind = RenderPassKind::post_process,
                                     .reads = {grounded},
                                     .writes = {grounded}});
    if (!status) {
        return fail(status);
    }
    const std::string final_scene = image_quality_.anti_aliasing ? "scene_aa" : grounded;
    status = image_quality_.bloom
                 ? builder.add_pass({.name = "bloom",
                                     .kind = RenderPassKind::post_process,
                                     .reads = {final_scene},
                                     .writes = {"bloom_hdr"},
                                     .sampled_resources = {{"input_hdr", final_scene, false}}})
                 : builder.add_pass({.name = "bloom",
                                     .kind = RenderPassKind::clear,
                                     .writes = {"bloom_hdr"},
                                     .clear_color = {0.0F, 0.0F, 0.0F, 0.0F}});
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
         .reads = {final_scene, "bloom_hdr"},
         .writes = {output},
         .sampled_resources = {{.binding_name = "scene_hdr", .resource_name = final_scene},
                               {.binding_name = "bloom_hdr", .resource_name = "bloom_hdr"}}});
    if (!status) {
        return fail(status);
    }

    // UI composites in display space, on top of the tone mapped image.
    status = builder.add_pass(
        {.name = "ui", .kind = RenderPassKind::ui, .reads = {output}, .writes = {output}});
    if (!status) {
        return fail(status);
    }
    status = builder.add_pass(
        {.name = "present", .kind = RenderPassKind::present, .reads = {output}, .presents = true});
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
    if (result.exposure.automatic_exposure) {
        result.exposure.exposure_stops += adapted_exposure_stops_;
    }
    const auto append = [&result](std::size_t pass_index,
                                  std::vector<rhi::RenderDrawCommand>& draws) {
        if (!draws.empty()) {
            result.pass_commands.push_back({pass_index, std::move(draws)});
        }
    };
    for (std::size_t cascade = 0; cascade < commands.directional_shadow_draws.size(); ++cascade) {
        append(cascade, commands.directional_shadow_draws[cascade]);
    }
    for (std::size_t slot = 0; slot < commands.local_shadow_draws.size(); ++slot) {
        append(hdr_pass_index::local_shadow_0 + slot, commands.local_shadow_draws[slot]);
    }
    append(hdr_pass_index::sky, commands.sky_draws);
    append(hdr_pass_index::opaque_terrain, commands.opaque_terrain_draws);
    append(hdr_pass_index::alpha_tested_terrain, commands.alpha_tested_terrain_draws);
    append(hdr_pass_index::rich_static_instances, commands.rich_instance_draws);
    const auto append_fullscreen = [&append](std::size_t index,
                                             rhi::RenderResourceHandle pipeline) {
        if (!pipeline.is_valid()) {
            return;
        }
        std::vector<rhi::RenderDrawCommand> draws(1);
        draws.front().pipeline = pipeline;
        draws.front().vertex_count = 3;
        append(index, draws);
    };
    if (image_quality_.ambient_occlusion) {
        append_fullscreen(hdr_pass_index::ssao, ssao_pipeline_);
        append_fullscreen(hdr_pass_index::ao_composite, ao_composite_pipeline_);
    }
    append(hdr_pass_index::transparent_terrain, commands.transparent_terrain_draws);
    append(hdr_pass_index::debug, commands.debug_draws);
    if (image_quality_.anti_aliasing) {
        append_fullscreen(hdr_pass_index::anti_alias, anti_alias_pipeline_);
    }
    if (image_quality_.bloom) {
        append_fullscreen(hdr_pass_index::bloom, bloom_pipeline_);
    }
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
