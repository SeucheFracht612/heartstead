#pragma once

#include "engine/assets/virtual_file_system.hpp"
#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/math/matrix.hpp"
#include "engine/math/vector.hpp"
#include "engine/platform/platform.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heartstead::renderer::rhi {

struct RenderFramePlan;
struct RenderFrameSubmission;

enum class RenderBackend {
    headless,
    vulkan,
};

enum class PresentMode {
    immediate,
    fifo,
    mailbox,
};

enum class RenderBufferUsage {
    vertex,
    index,
    uniform,
    storage,
    indirect,
    storage_indirect,
};

enum class RenderBufferMemory {
    host_visible,
    device_local,
};

enum class RenderIndexType : std::uint8_t {
    uint16,
    uint32,
};

[[nodiscard]] constexpr std::size_t render_index_type_size(RenderIndexType type) noexcept {
    return type == RenderIndexType::uint16 ? sizeof(std::uint16_t) : sizeof(std::uint32_t);
}

enum class RenderImageFormat {
    r8_unorm,
    rg16_sfloat,
    rgba8_unorm,
    rgba8_srgb,
    // Linear HDR scene format. All world shading writes linear radiance into a target of this
    // format; display encoding happens once, in the tone mapping pass.
    rgba16_sfloat,
    // Block-compressed material formats. BC5 is intended for two-channel normal maps; BC7
    // preserves high-quality colour and alpha. Upload sizes are expressed in 4x4 blocks.
    bc1_rgb_unorm,
    bc1_rgb_srgb,
    bc3_rgba_unorm,
    bc3_rgba_srgb,
    bc5_rg_unorm,
    bc7_rgba_unorm,
    bc7_rgba_srgb,
    d32_sfloat,
    d32_sfloat_s8_uint,
    d24_unorm_s8_uint,
};

enum class RenderDescriptorKind {
    sampled_texture,
    storage_image,
    storage_buffer,
    uniform_scalar,
    uniform_color,
};

enum class RenderSamplerFilter : std::uint8_t {
    nearest,
    linear,
};

enum class RenderSamplerMipmapMode : std::uint8_t {
    nearest,
    linear,
};

enum class RenderSamplerAddressMode : std::uint8_t {
    repeat,
    clamp_to_edge,
};

enum class RenderCompareOperation : std::uint8_t {
    never,
    less,
    less_or_equal,
    equal,
    greater,
    always,
};

enum class RenderShaderStage {
    vertex,
    fragment,
    compute,
};

enum class RenderShaderStageFlags : std::uint32_t {
    none = 0,
    vertex = 1U << 0U,
    fragment = 1U << 1U,
    compute = 1U << 2U,
};

[[nodiscard]] constexpr RenderShaderStageFlags operator|(RenderShaderStageFlags left,
                                                         RenderShaderStageFlags right) noexcept {
    return static_cast<RenderShaderStageFlags>(static_cast<std::uint32_t>(left) |
                                               static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr RenderShaderStageFlags operator&(RenderShaderStageFlags left,
                                                         RenderShaderStageFlags right) noexcept {
    return static_cast<RenderShaderStageFlags>(static_cast<std::uint32_t>(left) &
                                               static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr bool any(RenderShaderStageFlags flags) noexcept {
    return flags != RenderShaderStageFlags::none;
}

struct RenderResourceHandle {
    std::uint64_t value = 0;

    [[nodiscard]] bool is_valid() const noexcept {
        return value != 0;
    }

    friend auto operator<=>(const RenderResourceHandle&, const RenderResourceHandle&) = default;
};

// Binary-compatible with VkDrawIndexedIndirectCommand and the GLSL indexed indirect layout.
// Keeping this RHI-owned makes GPU-generated command buffers portable across backends.
struct RenderIndexedIndirectCommand {
    std::uint32_t index_count = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t first_index = 0;
    std::int32_t vertex_offset = 0;
    std::uint32_t first_instance = 0;
};

static_assert(sizeof(RenderIndexedIndirectCommand) == 20);

struct RenderIndirectDrawBinding {
    RenderResourceHandle command_buffer;
    RenderResourceHandle count_buffer;
    std::size_t command_offset = 0;
    std::size_t count_offset = 0;
    std::uint32_t maximum_draw_count = 0;
    std::uint32_t stride = sizeof(RenderIndexedIndirectCommand);

    [[nodiscard]] bool is_valid() const noexcept {
        return command_buffer.is_valid() && maximum_draw_count > 0;
    }

    [[nodiscard]] bool is_structurally_valid() const noexcept {
        if (!command_buffer.is_valid()) {
            return !count_buffer.is_valid() && command_offset == 0 && count_offset == 0 &&
                   maximum_draw_count == 0;
        }
        return maximum_draw_count > 0 && stride >= sizeof(RenderIndexedIndirectCommand) &&
               stride % alignof(std::uint32_t) == 0 &&
               command_offset % alignof(std::uint32_t) == 0 &&
               count_offset % alignof(std::uint32_t) == 0;
    }
};

struct RenderExtent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool is_valid() const noexcept;
};

struct ClearColor {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 1.0F;
};

struct RenderDeviceDesc {
    RenderBackend backend = RenderBackend::headless;
    std::string application_name = "Heartstead";
    RenderExtent initial_extent{1280, 720};
    PresentMode present_mode = PresentMode::fifo;
    std::uint32_t frames_in_flight = 2;
    bool enable_validation = true;
    // Diagnostic-only: serialize each present through VK_KHR_present_wait when supported. This
    // measures host-observed queue-to-presentation completion and intentionally changes pacing.
    bool measure_presentation_completion = false;
    std::optional<platform::NativeWindowHandle> native_window;
};

struct RenderFrameDesc {
    ClearColor clear_color{};
    RenderExtent output_extent{};
    bool present = true;
};

struct RenderFrameStats {
    RenderBackend backend = RenderBackend::headless;
    std::uint64_t frame_index = 0;
    std::uint64_t submission_serial = 0;
    std::uint64_t completed_submission_serial = 0;
    RenderExtent extent{};
    ClearColor clear_color{};
    bool presented = false;
    std::size_t render_pass_count = 0;
    std::size_t present_pass_count = 0;
    std::size_t resource_use_count = 0;
    std::size_t dependency_count = 0;
    std::size_t transition_count = 0;
    std::size_t synchronization_barrier_count = 0;
    std::size_t submitted_synchronization_barrier_count = 0;
    std::size_t draw_count = 0;
    std::size_t indexed_draw_count = 0;
    std::size_t indirect_draw_count = 0;
    std::size_t opaque_terrain_draw_count = 0;
    std::size_t alpha_tested_terrain_draw_count = 0;
    std::size_t transparent_terrain_draw_count = 0;
    std::size_t pipeline_bind_count = 0;
    std::size_t total_indices = 0;
    double cpu_frame_setup_ms = 0.0;
    double cpu_command_recording_ms = 0.0;
    double cpu_queue_submit_ms = 0.0;
    double cpu_gpu_wait_ms = 0.0;
    double cpu_frame_context_wait_ms = 0.0;
    double cpu_swapchain_acquire_ms = 0.0;
    double cpu_queue_present_ms = 0.0;
    bool presentation_timing_valid = false;
    std::uint64_t presentation_id = 0;
    double presentation_wait_ms = 0.0;
    bool gpu_timing_valid = false;
    std::uint64_t gpu_timing_frame_index = 0;
    std::uint32_t gpu_timing_latency_frames = 0;
    bool gpu_upload_timing_valid = false;
    std::uint64_t gpu_upload_submission_serial = 0;
    double gpu_frame_ms = 0.0;
    double gpu_shadow_ms = 0.0;
    double gpu_sky_ms = 0.0;
    double gpu_opaque_terrain_ms = 0.0;
    double gpu_alpha_tested_terrain_ms = 0.0;
    double gpu_transparent_terrain_ms = 0.0;
    double gpu_tone_map_ms = 0.0;
    double gpu_ui_ms = 0.0;
    double gpu_upload_ms = 0.0;
    double gpu_transfer_ms = 0.0;
    double gpu_final_copy_ms = 0.0;
    bool memory_budget_valid = false;
    std::uint64_t device_local_memory_budget_bytes = 0;
    std::uint64_t device_local_memory_usage_bytes = 0;
};

struct RenderBufferDesc {
    RenderBufferUsage usage = RenderBufferUsage::vertex;
    std::size_t byte_size = 0;
    std::string debug_name;
    RenderBufferMemory memory = RenderBufferMemory::host_visible;
};

struct RenderBufferWrite {
    RenderResourceHandle destination;
    std::size_t destination_offset = 0;
    std::span<const std::byte> bytes;
};

struct RenderBufferCreateStats {
    RenderBackend backend = RenderBackend::headless;
    RenderResourceHandle handle;
    RenderBufferUsage usage = RenderBufferUsage::vertex;
    RenderBufferMemory memory = RenderBufferMemory::host_visible;
    std::size_t byte_size = 0;
    std::size_t live_resource_count = 0;
    bool gpu_backed = false;
};

struct RenderBufferBatchUploadStats {
    RenderBackend backend = RenderBackend::headless;
    std::size_t write_count = 0;
    std::size_t byte_size = 0;
    std::uint64_t submission_serial = 0;
    double cpu_gpu_wait_ms = 0.0;
    bool used_fallback_staging = false;
    bool gpu_backed = false;
};

struct RenderImageDesc {
    RenderImageFormat format = RenderImageFormat::rgba8_unorm;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string debug_name;
    std::uint32_t array_layers = 1;
    std::uint32_t mip_levels = 1;
    // Cubemaps are six-layer images (or a multiple of six for cube arrays) exposed through cube
    // views. Keeping this explicit prevents a six-layer terrain array from changing dimension.
    bool cubemap = false;
    bool storage = false;
};

struct RenderSamplerDesc {
    RenderSamplerFilter min_filter = RenderSamplerFilter::nearest;
    RenderSamplerFilter mag_filter = RenderSamplerFilter::nearest;
    RenderSamplerMipmapMode mipmap_mode = RenderSamplerMipmapMode::nearest;
    RenderSamplerAddressMode address_u = RenderSamplerAddressMode::repeat;
    RenderSamplerAddressMode address_v = RenderSamplerAddressMode::repeat;
    RenderSamplerAddressMode address_w = RenderSamplerAddressMode::repeat;
    float max_anisotropy = 1.0F;
    float min_lod = 0.0F;
    float max_lod = 0.0F;
    bool comparison_enable = false;
    RenderCompareOperation comparison = RenderCompareOperation::less_or_equal;
    std::string debug_name;
};

struct RenderSamplerCreateStats {
    RenderBackend backend = RenderBackend::headless;
    RenderResourceHandle handle;
    std::size_t live_resource_count = 0;
    bool gpu_backed = false;
};

struct RenderUploadStats {
    RenderBackend backend = RenderBackend::headless;
    RenderResourceHandle handle;
    RenderBufferUsage usage = RenderBufferUsage::vertex;
    std::size_t byte_size = 0;
    std::size_t live_resource_count = 0;
    bool gpu_backed = false;
};

struct RenderImageUploadStats {
    RenderBackend backend = RenderBackend::headless;
    RenderResourceHandle handle;
    RenderImageFormat format = RenderImageFormat::rgba8_unorm;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t array_layers = 1;
    std::uint32_t mip_levels = 1;
    std::size_t byte_size = 0;
    std::size_t live_resource_count = 0;
    double cpu_gpu_wait_ms = 0.0;
    bool gpu_backed = false;
};

struct RenderDescriptorBinding {
    std::string name;
    RenderDescriptorKind kind = RenderDescriptorKind::sampled_texture;
    std::uint32_t slot = 0;
    bool required = true;
    RenderShaderStageFlags stages =
        RenderShaderStageFlags::vertex | RenderShaderStageFlags::fragment;
};

struct RenderPushConstantRange {
    RenderShaderStageFlags stages = RenderShaderStageFlags::vertex;
    std::uint32_t byte_offset = 0;
    std::uint32_t byte_size = 0;
};

struct RenderPipelineLayoutDesc {
    core::PrototypeId material_id;
    assets::VirtualPath shader_template;
    std::uint32_t pipeline_version = 1;
    std::vector<RenderDescriptorBinding> descriptors;
    std::vector<RenderPushConstantRange> push_constant_ranges;
    std::string debug_name;
    // Set when the material samples frame graph resources. The bound image then changes every
    // frame, so the backend keeps one descriptor set per frame in flight: a set bound to a command
    // buffer that is still executing must not be rewritten.
    bool per_frame_descriptors = false;
};

struct RenderPipelineLayoutStats {
    RenderBackend backend = RenderBackend::headless;
    core::PrototypeId material_id;
    std::uint32_t pipeline_version = 0;
    std::size_t descriptor_count = 0;
    std::size_t sampled_texture_count = 0;
    std::size_t storage_buffer_count = 0;
    std::size_t uniform_count = 0;
    std::size_t push_constant_range_count = 0;
    std::size_t bound_pipeline_count = 0;
    bool gpu_backed = false;
};

struct RenderShaderModuleDesc {
    RenderShaderStage stage = RenderShaderStage::vertex;
    std::string debug_name;
};

struct RenderShaderModuleStats {
    RenderBackend backend = RenderBackend::headless;
    RenderResourceHandle handle;
    RenderShaderStage stage = RenderShaderStage::vertex;
    std::size_t word_count = 0;
    std::size_t live_shader_module_count = 0;
    bool gpu_backed = false;
};

struct RenderComputePipelineDesc {
    RenderResourceHandle compute_shader;
    core::PrototypeId material_id;
    std::string entry_point = "main";
    std::string debug_name;
};

struct RenderComputePipelineStats {
    RenderBackend backend = RenderBackend::headless;
    RenderResourceHandle handle;
    RenderResourceHandle compute_shader;
    core::PrototypeId material_id;
    std::size_t live_compute_pipeline_count = 0;
    bool gpu_backed = false;
};

enum class RenderVertexAttributeFormat : std::uint8_t {
    float2,
    float3,
    float4,
    sint16x4,
    uint16x4,
    uint16x2,
    uint16,
    snorm16x4,
    unorm16x4,
    snorm8x4,
    unorm8x4,
    uint8x4,
    uint8,
};

enum class RenderPrimitiveTopology : std::uint8_t {
    triangle_list,
    line_list,
};

enum class RenderPolygonMode : std::uint8_t {
    fill,
    line,
};

enum class RenderCullMode : std::uint8_t {
    none,
    front,
    back,
};

enum class RenderFrontFace : std::uint8_t {
    clockwise,
    counter_clockwise,
};

enum class RenderBlendMode : std::uint8_t {
    disabled,
    alpha,
    additive,
    premultiplied_alpha,
};

struct RenderVertexAttributeDesc {
    std::uint32_t location = 0;
    std::uint32_t byte_offset = 0;
    RenderVertexAttributeFormat format = RenderVertexAttributeFormat::float3;
};

struct RenderGraphicsPipelineDesc {
    RenderResourceHandle vertex_shader;
    RenderResourceHandle fragment_shader;
    core::PrototypeId material_id;
    std::string vertex_entry_point = "main";
    std::string fragment_entry_point = "main";
    std::string debug_name;
    std::uint32_t vertex_stride = 0;
    std::vector<RenderVertexAttributeDesc> vertex_attributes;
    RenderPrimitiveTopology topology = RenderPrimitiveTopology::triangle_list;
    RenderPolygonMode polygon_mode = RenderPolygonMode::fill;
    RenderCullMode cull_mode = RenderCullMode::back;
    RenderFrontFace front_face = RenderFrontFace::counter_clockwise;
    bool depth_test_enable = true;
    bool depth_write_enable = true;
    RenderCompareOperation depth_compare = RenderCompareOperation::less;
    RenderBlendMode blend_mode = RenderBlendMode::disabled;
    // Depth-only pipelines set this false. Extra entries enable MRT while preserving the legacy
    // primary format field used throughout the renderer.
    bool color_write_enable = true;
    RenderImageFormat color_target_format = RenderImageFormat::rgba8_unorm;
    std::vector<RenderImageFormat> additional_color_target_formats;
    RenderImageFormat depth_target_format = RenderImageFormat::d32_sfloat;

    RenderGraphicsPipelineDesc() = default;
    RenderGraphicsPipelineDesc(RenderResourceHandle vertex, RenderResourceHandle fragment,
                               core::PrototypeId material, std::string vertex_entry,
                               std::string fragment_entry, std::string name)
        : vertex_shader(vertex), fragment_shader(fragment), material_id(std::move(material)),
          vertex_entry_point(std::move(vertex_entry)),
          fragment_entry_point(std::move(fragment_entry)), debug_name(std::move(name)),
          cull_mode(RenderCullMode::none), depth_test_enable(false), depth_write_enable(false) {}
};

struct RenderGraphicsPipelineStats {
    RenderBackend backend = RenderBackend::headless;
    RenderResourceHandle handle;
    RenderResourceHandle vertex_shader;
    RenderResourceHandle fragment_shader;
    core::PrototypeId material_id;
    std::size_t live_graphics_pipeline_count = 0;
    bool gpu_backed = false;
};

struct RenderDescriptorWrite {
    core::PrototypeId material_id;
    std::string binding_name;
    RenderResourceHandle resource;
    std::size_t byte_offset = 0;
    std::size_t byte_size = 0;
    RenderResourceHandle sampler;

    RenderDescriptorWrite() = default;
    RenderDescriptorWrite(core::PrototypeId material, std::string binding,
                          RenderResourceHandle resource_handle, std::size_t offset = 0,
                          std::size_t size = 0, RenderResourceHandle sampler_handle = {})
        : material_id(std::move(material)), binding_name(std::move(binding)),
          resource(resource_handle), byte_offset(offset), byte_size(size), sampler(sampler_handle) {
    }
};

struct RenderDescriptorWriteStats {
    RenderBackend backend = RenderBackend::headless;
    std::size_t write_count = 0;
    std::size_t uniform_write_count = 0;
    std::size_t sampled_texture_write_count = 0;
    std::size_t storage_buffer_write_count = 0;
    std::size_t material_count = 0;
    bool gpu_backed = false;
};

struct RenderMeshBinding {
    RenderResourceHandle vertex_buffer;
    RenderResourceHandle index_buffer;
    core::PrototypeId material_id;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    std::uint32_t instance_count = 1;
    std::string debug_name;
    std::int64_t world_anchor_x = 0;
    std::int64_t world_anchor_y = 0;
    std::int64_t world_anchor_z = 0;
    float camera_relative_x = 0.0F;
    float camera_relative_y = 0.0F;
    float camera_relative_z = 0.0F;

    RenderMeshBinding() = default;
    RenderMeshBinding(RenderResourceHandle vertices, RenderResourceHandle indices,
                      core::PrototypeId material, std::uint32_t vertices_count,
                      std::uint32_t indices_count, std::uint32_t instances, std::string name)
        : vertex_buffer(vertices), index_buffer(indices), material_id(std::move(material)),
          vertex_count(vertices_count), index_count(indices_count), instance_count(instances),
          debug_name(std::move(name)) {}
};

struct RenderDrawStats {
    RenderBackend backend = RenderBackend::headless;
    std::size_t draw_count = 0;
    std::size_t indexed_draw_count = 0;
    std::size_t material_count = 0;
    std::size_t total_vertices = 0;
    std::size_t total_indices = 0;
    bool gpu_backed = false;
    bool draw_commands_submitted = false;
};

struct RenderCameraData {
    math::Mat4f view_projection = math::Mat4f::identity();
};

struct RenderEnvironmentData {
    math::Vec3f sun_direction{0.45F, 0.82F, 0.35F};
    float sun_intensity = 0.68F;
    math::Vec3f sun_color{1.0F, 0.96F, 0.88F};
    float elapsed_seconds = 0.0F;
    math::Vec3f ambient_color{0.32F, 0.36F, 0.42F};
    float fog_start = 384.0F;
    math::Vec3f fog_color{0.055F, 0.09F, 0.14F};
    float fog_end = 512.0F;
    math::Vec3f sky_zenith_color{0.12F, 0.30F, 0.58F};
    float cloud_coverage = 0.0F;
    math::Vec3f sky_horizon_color{0.38F, 0.56F, 0.72F};
    float cloud_density = 0.0F;
    float sky_diffuse_intensity = 1.0F;
    float environment_specular_intensity = 1.0F;
    float environment_rotation_radians = 0.0F;
    float aerial_perspective = 0.0F;
    math::Vec3f wind_velocity{};
    float wind_gust_strength = 0.0F;
    float wind_gust_frequency = 0.2F;
    float wind_turbulence = 0.0F;
    float precipitation_intensity = 0.0F;
    float wetness = 0.0F;
    float snow = 0.0F;
    float storm_intensity = 0.0F;
    float visibility = 1.0F;
    float height_fog_density = 0.0F;
    float height_fog_falloff = 0.1F;
    float local_fog_density = 0.0F;
    math::Vec3f water_shallow_color{0.16F, 0.46F, 0.58F};
    float water_absorption_distance = 8.0F;
    math::Vec3f water_deep_color{0.015F, 0.10F, 0.18F};
    float water_scattering_strength = 0.25F;
    math::Vec3f water_scattering_color{0.08F, 0.34F, 0.42F};
    float water_refraction_strength = 0.025F;
    math::Vec3f water_foam_color{0.86F, 0.94F, 0.92F};
    float water_foam_strength = 0.5F;
    float water_normal_strength = 0.45F;
    float water_normal_speed = 0.08F;
    float water_fresnel_f0 = 0.02F;
    float water_ripple_strength = 0.2F;
    float underwater_fog_distance = 24.0F;
    bool underwater = false;
};

enum class RenderToneMapping : std::uint8_t {
    // Clamp only. Useful for debugging the linear scene target, not for shipping.
    none,
    reinhard,
    // Narkowicz ACES fit. Cheap, filmic, and the renderer default.
    aces_approx,
    // Khronos PBR neutral. Preserves saturated albedo better than the ACES fit.
    khronos_pbr_neutral,
};

// Frame-wide exposure and display-encoding controls consumed by the tone mapping pass.
struct RenderExposureSettings {
    // Exposure compensation in stops. The scene target is scaled by 2^exposure_stops before
    // tone mapping.
    float exposure_stops = 0.0F;
    RenderToneMapping tone_mapping = RenderToneMapping::aces_approx;
    // Scene luminance mapped to display white before the curve is applied.
    float white_point = 1.0F;
    float saturation = 1.0F;
    float contrast = 1.0F;
    float bloom_intensity = 0.08F;
    bool automatic_exposure = true;
    float target_luminance = 0.18F;
    float adaptation_speed = 1.5F;
    float minimum_auto_stops = -6.0F;
    float maximum_auto_stops = 6.0F;
};

// Shader-visible constants for the tone mapping pass. Mirrors ToneMapPushConstants in
// shaders/builtin/tone_map.frag.
struct ToneMapPushConstants {
    float exposure_scale = 1.0F;
    float white_point = 1.0F;
    std::uint32_t tone_mapping = 2U;
    std::uint32_t padding = 0U;
    float saturation = 1.0F;
    float contrast = 1.0F;
    float bloom_intensity = 0.08F;
    float grading_padding = 0.0F;
};

static_assert(sizeof(ToneMapPushConstants) == 32);

// Shader-visible constants. Mat4f is column-major and all vectors occupy complete 16-byte lanes.
struct ChunkPushConstants {
    math::Mat4f view_projection = math::Mat4f::identity();
    float camera_relative_origin[4]{};
    float sun_direction_intensity[4]{0.45F, 0.82F, 0.35F, 0.68F};
    float ambient_color_fog_start[4]{0.32F, 0.36F, 0.42F, 384.0F};
    float fog_color_fog_end[4]{0.055F, 0.09F, 0.14F, 512.0F};
};

static_assert(sizeof(math::Mat4f) == 64);
static_assert(sizeof(ChunkPushConstants) == 128);
static_assert(offsetof(ChunkPushConstants, view_projection) == 0);
static_assert(offsetof(ChunkPushConstants, camera_relative_origin) == 64);
static_assert(offsetof(ChunkPushConstants, sun_direction_intensity) == 80);
static_assert(offsetof(ChunkPushConstants, ambient_color_fog_start) == 96);
static_assert(offsetof(ChunkPushConstants, fog_color_fog_end) == 112);

struct RendererBackendInfo {
    RenderBackend backend = RenderBackend::headless;
    std::string_view name;
    bool available = false;
    std::string_view status;
};

struct RenderDeviceInfo {
    RenderBackend backend = RenderBackend::headless;
    std::string_view device_name;
    std::string_view driver_name;
    std::string_view driver_info;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
};

struct RenderDeviceCapabilities {
    RenderBackend backend = RenderBackend::headless;
    RenderExtent max_extent{};
    bool supports_present = false;
    bool supports_validation = false;
    bool supports_debug_markers = false;
    bool supports_shader_modules = false;
    bool supports_pipeline_layout = false;
    bool supports_compute_pipelines = false;
    bool supports_graphics_pipelines = false;
    bool supports_descriptor_writes = false;
    bool supports_buffer_upload = false;
    bool supports_image_upload = false;
    bool supports_sampler_cache = false;
    bool supports_draw_binding = false;
    bool supports_multi_draw_indirect = false;
    bool supports_draw_indirect_first_instance = false;
    bool supports_draw_indirect_count = false;
    std::uint32_t maximum_draw_indirect_count = 1;
    bool supports_presentation_completion_timing = false;
    bool supports_frame_submission = false;
    bool supports_depth = false;
    bool headless = true;
    bool supports_memory_budget = false;
    std::uint64_t device_local_memory_budget_bytes = 0;
    std::uint64_t device_local_memory_usage_bytes = 0;
};

struct RenderBackendCapabilities {
    RenderBackend backend = RenderBackend::headless;
    bool available = false;
    bool supports_present = false;
    bool supports_validation = false;
    bool supports_debug_markers = false;
    bool supports_shader_modules = false;
    bool supports_pipeline_layout = false;
    bool supports_compute_pipelines = false;
    bool supports_graphics_pipelines = false;
    bool supports_descriptor_writes = false;
    bool supports_buffer_upload = false;
    bool supports_image_upload = false;
    bool supports_sampler_cache = false;
    bool supports_draw_binding = false;
    bool supports_frame_submission = false;
    bool supports_depth = false;
    bool requires_window_surface = false;
    bool requires_gpu_device = false;
    bool supports_headless = false;
    std::uint32_t recommended_frames_in_flight = 1;
    std::string_view graphics_api;
    bool supports_multi_draw_indirect = false;
    bool supports_draw_indirect_first_instance = false;
    bool supports_draw_indirect_count = false;
};

class IRenderDevice {
  public:
    virtual ~IRenderDevice() = default;

    [[nodiscard]] virtual RenderBackend backend() const noexcept = 0;
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
    [[nodiscard]] virtual RenderDeviceInfo info() const noexcept = 0;
    [[nodiscard]] virtual RenderDeviceCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual RenderExtent current_extent() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t completed_frame_count() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t last_submission_serial() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t completed_submission_serial() const noexcept = 0;
    [[nodiscard]] virtual std::size_t live_resource_count() const noexcept = 0;

    [[nodiscard]] virtual core::Status wait_idle() = 0;
    // Copies the image the most recent frame resolved into, as tightly packed RGBA8 at the current
    // extent. For tests and tooling only: it idles the GPU, so it is not a per-frame operation.
    // Backends that cannot produce real pixels report renderer.readback_unsupported.
    [[nodiscard]] virtual core::Result<std::vector<std::uint8_t>> read_back_output_image() = 0;
    // Copies a buffer's contents back to the CPU. Same caveats as image readback: it idles the
    // device, so it is for tests and tooling rather than per-frame use.
    [[nodiscard]] virtual core::Result<std::vector<std::uint8_t>>
    read_back_buffer(RenderResourceHandle buffer) = 0;
    [[nodiscard]] virtual core::Status resize(RenderExtent extent) = 0;
    [[nodiscard]] virtual core::Result<RenderFrameStats> render_frame(RenderFrameDesc desc) = 0;
    [[nodiscard]] virtual core::Result<RenderFrameStats>
    execute_frame_plan(const RenderFramePlan& plan) = 0;
    [[nodiscard]] virtual core::Result<RenderFrameStats>
    execute_frame(const RenderFrameSubmission& frame) = 0;
    [[nodiscard]] virtual core::Result<RenderBufferCreateStats>
    create_buffer(RenderBufferDesc desc) = 0;
    [[nodiscard]] virtual core::Result<RenderBufferBatchUploadStats>
    upload_buffer_batch(std::span<const RenderBufferWrite> writes) = 0;
    [[nodiscard]] virtual core::Result<RenderUploadStats>
    upload_buffer(RenderBufferDesc desc, std::span<const std::byte> bytes) = 0;
    [[nodiscard]] virtual core::Result<RenderImageUploadStats>
    upload_image(RenderImageDesc desc, std::span<const std::byte> bytes) = 0;
    [[nodiscard]] virtual core::Result<RenderSamplerCreateStats>
    create_sampler(RenderSamplerDesc desc) = 0;
    [[nodiscard]] virtual core::Result<RenderShaderModuleStats>
    create_shader_module(RenderShaderModuleDesc desc,
                         std::span<const std::uint32_t> spirv_words) = 0;
    [[nodiscard]] virtual core::Result<RenderPipelineLayoutStats>
    bind_pipeline_layout(RenderPipelineLayoutDesc desc) = 0;
    [[nodiscard]] virtual core::Result<RenderComputePipelineStats>
    create_compute_pipeline(RenderComputePipelineDesc desc) = 0;
    [[nodiscard]] virtual core::Result<RenderGraphicsPipelineStats>
    create_graphics_pipeline(RenderGraphicsPipelineDesc desc) = 0;
    [[nodiscard]] virtual core::Result<RenderDescriptorWriteStats>
    write_descriptors(std::span<const RenderDescriptorWrite> writes) = 0;
    // Deprecated compatibility path. New rendering must submit draws through execute_frame().
    [[nodiscard]] virtual core::Result<RenderDrawStats>
    bind_mesh_draws(std::span<const RenderMeshBinding> draws) = 0;
    [[nodiscard]] virtual core::Status release_resource(RenderResourceHandle handle) = 0;
};

[[nodiscard]] core::Result<std::unique_ptr<IRenderDevice>>
create_render_device(RenderDeviceDesc desc);

[[nodiscard]] core::Status validate_render_device_desc(const RenderDeviceDesc& desc);
[[nodiscard]] core::Status validate_render_extent(RenderExtent extent);
[[nodiscard]] core::Status validate_render_buffer_desc(const RenderBufferDesc& desc);
[[nodiscard]] core::Status
validate_render_buffer_writes_shape(std::span<const RenderBufferWrite> writes);
[[nodiscard]] core::Status validate_render_buffer_upload(const RenderBufferDesc& desc,
                                                         std::span<const std::byte> bytes);
[[nodiscard]] core::Status validate_render_image_upload(const RenderImageDesc& desc,
                                                        std::span<const std::byte> bytes);
[[nodiscard]] core::Status validate_render_sampler_desc(const RenderSamplerDesc& desc);
[[nodiscard]] core::Status
validate_render_shader_module_upload(const RenderShaderModuleDesc& desc,
                                     std::span<const std::uint32_t> spirv_words);
[[nodiscard]] core::Status
validate_render_pipeline_layout_shape(const RenderPipelineLayoutDesc& desc);
[[nodiscard]] bool
equivalent_render_pipeline_layout(const RenderPipelineLayoutDesc& left,
                                  const RenderPipelineLayoutDesc& right) noexcept;
[[nodiscard]] core::Status
validate_render_compute_pipeline_shape(const RenderComputePipelineDesc& desc);
[[nodiscard]] core::Status
validate_render_graphics_pipeline_shape(const RenderGraphicsPipelineDesc& desc);
[[nodiscard]] core::Status
validate_render_descriptor_writes_shape(std::span<const RenderDescriptorWrite> writes);
[[nodiscard]] core::Status
validate_render_mesh_bindings_shape(std::span<const RenderMeshBinding> draws);

[[nodiscard]] RendererBackendInfo renderer_backend_info(RenderBackend backend) noexcept;
[[nodiscard]] RenderBackendCapabilities render_backend_capabilities(RenderBackend backend) noexcept;
[[nodiscard]] std::string_view render_backend_name(RenderBackend backend) noexcept;
[[nodiscard]] std::string_view present_mode_name(PresentMode mode) noexcept;
[[nodiscard]] std::string_view render_buffer_usage_name(RenderBufferUsage usage) noexcept;
[[nodiscard]] std::string_view render_buffer_memory_name(RenderBufferMemory memory) noexcept;
[[nodiscard]] std::string_view render_image_format_name(RenderImageFormat format) noexcept;
[[nodiscard]] std::size_t render_image_format_bytes_per_pixel(RenderImageFormat format) noexcept;
[[nodiscard]] std::string_view render_descriptor_kind_name(RenderDescriptorKind kind) noexcept;
[[nodiscard]] std::string_view render_sampler_filter_name(RenderSamplerFilter value) noexcept;
[[nodiscard]] std::string_view
render_sampler_mipmap_mode_name(RenderSamplerMipmapMode value) noexcept;
[[nodiscard]] std::string_view
render_sampler_address_mode_name(RenderSamplerAddressMode value) noexcept;
[[nodiscard]] std::string_view render_shader_stage_name(RenderShaderStage stage) noexcept;
[[nodiscard]] std::string_view
render_primitive_topology_name(RenderPrimitiveTopology value) noexcept;
[[nodiscard]] std::string_view render_polygon_mode_name(RenderPolygonMode value) noexcept;
[[nodiscard]] std::string_view render_cull_mode_name(RenderCullMode value) noexcept;
[[nodiscard]] std::string_view render_front_face_name(RenderFrontFace value) noexcept;
[[nodiscard]] std::string_view render_compare_operation_name(RenderCompareOperation value) noexcept;
[[nodiscard]] std::string_view render_blend_mode_name(RenderBlendMode value) noexcept;
[[nodiscard]] bool is_depth_format(RenderImageFormat format) noexcept;
[[nodiscard]] bool is_hdr_format(RenderImageFormat format) noexcept;
[[nodiscard]] std::string_view render_tone_mapping_name(RenderToneMapping value) noexcept;
[[nodiscard]] core::Status validate_render_exposure(const RenderExposureSettings& exposure);
[[nodiscard]] ToneMapPushConstants
make_tone_map_push_constants(const RenderExposureSettings& exposure) noexcept;

} // namespace heartstead::renderer::rhi
