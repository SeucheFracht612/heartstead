#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/rhi/render_device.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer::rhi {

enum class RenderResourceLifetime {
    transient,
    external,
};

enum class RenderPassKind {
    clear,
    world,
    post_process,
    ui,
    debug,
    // Dispatches compute work instead of rasterizing. Records no rendering block and binds no
    // attachments; its resources are reached through descriptors.
    compute,
    present,
};

enum class RenderResourceAccess {
    read,
    write,
    read_write,
    present,
};

enum class RenderResourceState {
    undefined,
    external,
    shader_read,
    color_attachment_write,
    color_attachment_read_write,
    depth_attachment_write,
    storage_read,
    storage_write,
    storage_read_write,
    transfer_source,
    transfer_destination,
    present,
};

struct RenderResourceDesc {
    std::string name;
    RenderExtent extent{};
    RenderResourceLifetime lifetime = RenderResourceLifetime::transient;
    RenderImageFormat format = RenderImageFormat::rgba8_unorm;
    std::uint32_t array_layers = 1;
    std::uint32_t mip_levels = 1;
    bool cubemap = false;
    // Storage resources use GENERAL layout and descriptor access rather than being interpreted as
    // raster attachments merely because a pass writes them.
    bool storage = false;
};

// Feeds a frame graph resource into a named descriptor binding of the material a pass draws with.
// This is how a post-process pass samples what an earlier pass wrote: the resource has no device
// handle of its own, so it is addressed by graph name and resolved by the backend each frame.
struct RenderPassSampledResource {
    // Matches a RenderDescriptorBinding name on the material's pipeline layout.
    std::string binding_name{};
    // Graph resource to sample. Must also appear in the pass's reads.
    std::string resource_name{};
    // Shadow maps require a comparison-enabled sampler. This is part of the graph binding because
    // the image is resolved per frame rather than through an ordinary descriptor write.
    bool comparison = false;
};

struct RenderPassStorageResource {
    std::string binding_name{};
    std::string resource_name{};
    RenderResourceAccess access = RenderResourceAccess::read_write;
};

// Every member carries a default initializer so passes can be written with designated
// initializers naming only what they use, and so adding a field never silently repoints a value
// in an existing positional initializer.
struct RenderPassDesc {
    std::string name{};
    RenderPassKind kind = RenderPassKind::clear;
    std::vector<std::string> reads{};
    std::vector<std::string> writes{};
    ClearColor clear_color{};
    bool presents = false;
    std::vector<RenderPassSampledResource> sampled_resources{};
    std::vector<RenderPassStorageResource> storage_resources{};
};

struct RenderFrameResourceUse {
    std::size_t pass_index = 0;
    std::string pass_name;
    RenderPassKind pass_kind = RenderPassKind::clear;
    std::string resource_name;
    RenderResourceLifetime resource_lifetime = RenderResourceLifetime::transient;
    RenderResourceAccess access = RenderResourceAccess::read;
    RenderResourceState required_state = RenderResourceState::shader_read;
};

struct RenderFrameDependency {
    std::string resource_name;
    std::size_t source_use_index = 0;
    std::size_t destination_use_index = 0;
    std::size_t source_pass_index = 0;
    std::size_t destination_pass_index = 0;
    RenderResourceAccess source_access = RenderResourceAccess::read;
    RenderResourceAccess destination_access = RenderResourceAccess::read;
};

struct RenderFrameResourceTransition {
    std::string resource_name;
    bool has_source_use = false;
    std::size_t source_use_index = 0;
    std::size_t destination_use_index = 0;
    std::size_t source_pass_index = 0;
    std::size_t destination_pass_index = 0;
    RenderResourceState before_state = RenderResourceState::undefined;
    RenderResourceState after_state = RenderResourceState::undefined;
};

struct RenderFrameExecutionPlan {
    RenderExtent extent{};
    std::vector<std::string> ordered_passes;
    std::vector<RenderFrameResourceUse> resource_uses;
    std::vector<RenderFrameDependency> dependencies;
    std::vector<RenderFrameResourceTransition> transitions;
    std::size_t present_pass_count = 0;
};

struct RenderFramePlan {
    RenderExtent extent{};
    std::vector<RenderResourceDesc> resources;
    std::vector<RenderPassDesc> passes;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] core::Result<RenderFrameExecutionPlan> build_execution_plan() const;
    [[nodiscard]] const RenderResourceDesc* find_resource(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t pass_count(RenderPassKind kind) const noexcept;
    [[nodiscard]] bool has_present_pass() const noexcept;
    [[nodiscard]] ClearColor first_clear_color() const noexcept;
};

struct RenderScissorRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct RenderDrawCommand {
    RenderResourceHandle pipeline;
    RenderResourceHandle vertex_buffer;
    RenderResourceHandle index_buffer;

    // A draw is indexed when it carries an index buffer, and non-indexed otherwise. Non-indexed
    // draws use vertex_count and need no vertex buffer at all, which is what lets a fullscreen
    // pass derive its positions from gl_VertexIndex instead of uploading geometry.
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    std::uint32_t first_index = 0;
    std::int32_t vertex_offset = 0;
    std::uint32_t instance_count = 1;
    std::uint32_t first_instance = 0;

    math::Vec3f camera_relative_origin{};
    RenderIndexType index_type = RenderIndexType::uint32;
    // Renderer-side stable ordering metadata. Backends deliberately ignore this value.
    float sort_depth = 0.0F;
    // Retained scene batches use this to exclude non-shadow-casting effects from shadow passes.
    // Backends deliberately ignore it outside renderer pass construction.
    bool casts_shadow = true;
    bool scissor_enabled = false;
    RenderScissorRect scissor{};
    std::uint32_t texture_variation_seed = 0;
    // Shadow and probe passes use a pass-specific camera while retaining the ordinary draw
    // geometry and large-coordinate origin.
    bool view_projection_override_enabled = false;
    math::Mat4f view_projection_override = math::Mat4f::identity();
    RenderIndirectDrawBinding indirect;
};

// One compute dispatch recorded into a compute pass.
struct RenderComputeDispatch {
    RenderResourceHandle pipeline;
    std::uint32_t group_count_x = 1;
    std::uint32_t group_count_y = 1;
    std::uint32_t group_count_z = 1;
    // Constants for the dispatch, pushed at offset zero to the compute stage. A compute shader
    // that statically uses push constants must be given them, so the caller supplies the bytes
    // rather than the backend guessing at a layout the way graphics draws do.
    std::vector<std::byte> push_constants{};
};

struct RenderPassCommands {
    std::size_t pass_index = 0;
    std::vector<RenderDrawCommand> draws;
    std::vector<RenderComputeDispatch> dispatches{};
};

struct RenderFrameSubmission {
    RenderFramePlan plan;
    RenderCameraData camera;
    RenderEnvironmentData environment;
    RenderExposureSettings exposure;
    std::vector<RenderPassCommands> pass_commands;
};

// Names the graph uses for the resources every frame owns. Passes and backends refer to these
// instead of repeating string literals.
inline constexpr std::string_view scene_color_resource_name = "scene_hdr";
inline constexpr std::string_view scene_motion_resource_name = "scene_motion";
inline constexpr std::string_view depth_resource_name = "depth";
inline constexpr std::string_view output_resource_name = "output";

class RenderFramePlanBuilder {
  public:
    explicit RenderFramePlanBuilder(RenderExtent extent);

    [[nodiscard]] core::Status add_resource(RenderResourceDesc resource);
    [[nodiscard]] core::Status add_pass(RenderPassDesc pass);
    [[nodiscard]] core::Result<RenderFramePlan> build() const;

  private:
    RenderFramePlan plan_;
};

[[nodiscard]] RenderFramePlan make_clear_present_frame_plan(RenderExtent extent,
                                                            ClearColor clear_color, bool present);
[[nodiscard]] core::Status
validate_render_frame_submission_shape(const RenderFrameSubmission& frame);
[[nodiscard]] core::Status validate_render_environment(const RenderEnvironmentData& environment);

[[nodiscard]] std::string_view
render_resource_lifetime_name(RenderResourceLifetime lifetime) noexcept;
[[nodiscard]] std::string_view render_pass_kind_name(RenderPassKind kind) noexcept;
[[nodiscard]] std::string_view render_resource_access_name(RenderResourceAccess access) noexcept;
[[nodiscard]] std::string_view render_resource_state_name(RenderResourceState state) noexcept;

} // namespace heartstead::renderer::rhi
