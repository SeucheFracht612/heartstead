#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/frame/render_commands.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"

#include <cstddef>
#include <cstdint>

namespace heartstead::renderer {

struct FrameImageQualitySettings {
    float render_scale = 1.0F;
    bool ambient_occlusion = true;
    bool anti_aliasing = true;
    bool bloom = true;

    [[nodiscard]] core::Status validate() const noexcept;
};

// Pass indices the frame builder assigns. Draw command lists
// are appended against these, and the backend keys pass execution off them.
namespace hdr_pass_index {
inline constexpr std::size_t shadow_0 = 0;
inline constexpr std::size_t shadow_1 = 1;
inline constexpr std::size_t shadow_2 = 2;
inline constexpr std::size_t shadow_3 = 3;
inline constexpr std::size_t local_shadow_0 = 4;
inline constexpr std::size_t local_shadow_1 = 5;
inline constexpr std::size_t sky = 6;
inline constexpr std::size_t opaque_terrain = 7;
inline constexpr std::size_t alpha_tested_terrain = 8;
inline constexpr std::size_t rich_static_instances = 9;
inline constexpr std::size_t ssao = 10;
inline constexpr std::size_t ao_composite = 11;
inline constexpr std::size_t transparent_terrain = 12;
inline constexpr std::size_t debug = 13;
inline constexpr std::size_t anti_alias = 14;
inline constexpr std::size_t bloom = 15;
inline constexpr std::size_t tone_map = 16;
inline constexpr std::size_t ui = 17;
inline constexpr std::size_t present = 18;
inline constexpr std::size_t count = 19;
} // namespace hdr_pass_index

class FrameBuilder {
  public:
    explicit FrameBuilder(rhi::RenderExtent extent, rhi::ClearColor clear_color = {});

    [[nodiscard]] core::Status resize(rhi::RenderExtent extent);
    void set_clear_color(rhi::ClearColor clear_color) noexcept;
    [[nodiscard]] core::Status set_exposure(rhi::RenderExposureSettings exposure);
    [[nodiscard]] rhi::RenderExposureSettings exposure() const noexcept;
    // Pipeline the synthesized fullscreen tone map draw uses. Without it the linear HDR graph
    // builds a tone_map pass that records nothing, so the renderer supplies this at startup.
    void set_tone_map_pipeline(rhi::RenderResourceHandle pipeline) noexcept;
    void set_image_quality_pipelines(rhi::RenderResourceHandle ssao,
                                     rhi::RenderResourceHandle ao_composite,
                                     rhi::RenderResourceHandle anti_alias,
                                     rhi::RenderResourceHandle bloom) noexcept;
    [[nodiscard]] core::Status set_image_quality_settings(FrameImageQualitySettings settings);
    [[nodiscard]] FrameImageQualitySettings image_quality_settings() const noexcept;
    void update_exposure_adaptation(float scene_luminance, float delta_seconds) noexcept;
    [[nodiscard]] core::Status set_shadow_resolution(std::uint32_t resolution);
    [[nodiscard]] rhi::RenderResourceHandle tone_map_pipeline() const noexcept;

    [[nodiscard]] core::Result<rhi::RenderFramePlan> build_plan() const;
    [[nodiscard]] core::Result<rhi::RenderFrameSubmission>
    build(const RenderCamera& camera, RenderCommandLists commands,
          rhi::RenderEnvironmentData environment = {}) const;

    [[nodiscard]] rhi::RenderExtent extent() const noexcept;

  private:
    rhi::RenderExtent extent_{};
    rhi::ClearColor clear_color_{};
    rhi::RenderExposureSettings exposure_{};
    rhi::RenderResourceHandle tone_map_pipeline_{};
    rhi::RenderResourceHandle ssao_pipeline_{};
    rhi::RenderResourceHandle ao_composite_pipeline_{};
    rhi::RenderResourceHandle anti_alias_pipeline_{};
    rhi::RenderResourceHandle bloom_pipeline_{};
    std::uint32_t shadow_resolution_ = 2048;
    std::uint32_t local_shadow_resolution_ = 1024;
    float adapted_exposure_stops_ = 0.0F;
    FrameImageQualitySettings image_quality_{};
};

} // namespace heartstead::renderer
