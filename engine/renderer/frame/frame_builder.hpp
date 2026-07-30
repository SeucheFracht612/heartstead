#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/frame/render_commands.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"

#include <cstddef>
#include <cstdint>

namespace heartstead::renderer {

// Selects how scene colour reaches the display.
//
// legacy_ldr is the original prototype path: world shaders encode sRGB themselves and write
// straight into the swapchain image. It is retained only until the Vulkan backend executes the
// linear_hdr graph, and is scheduled for removal in the same change that flips the default.
//
// linear_hdr is the production path: world shading writes linear radiance into an rgba16_sfloat
// scene target, and a single tone mapping pass applies exposure, the tone curve, and the display
// transfer function before UI is composited on top.
enum class FrameColorPipeline : std::uint8_t {
    legacy_ldr,
    linear_hdr,
};

// Pass indices the frame builder assigns for FrameColorPipeline::linear_hdr. Draw command lists
// are appended against these, and the backend keys pass execution off them.
namespace hdr_pass_index {
inline constexpr std::size_t sky = 0;
inline constexpr std::size_t opaque_terrain = 1;
inline constexpr std::size_t alpha_tested_terrain = 2;
inline constexpr std::size_t rich_static_instances = 3;
inline constexpr std::size_t transparent_terrain = 4;
inline constexpr std::size_t debug = 5;
inline constexpr std::size_t tone_map = 6;
inline constexpr std::size_t ui = 7;
inline constexpr std::size_t present = 8;
inline constexpr std::size_t count = 9;
} // namespace hdr_pass_index

class FrameBuilder {
  public:
    explicit FrameBuilder(rhi::RenderExtent extent, rhi::ClearColor clear_color = {});

    [[nodiscard]] core::Status resize(rhi::RenderExtent extent);
    void set_clear_color(rhi::ClearColor clear_color) noexcept;
    void set_color_pipeline(FrameColorPipeline pipeline) noexcept;
    [[nodiscard]] FrameColorPipeline color_pipeline() const noexcept;
    [[nodiscard]] core::Status set_exposure(rhi::RenderExposureSettings exposure);
    [[nodiscard]] rhi::RenderExposureSettings exposure() const noexcept;

    [[nodiscard]] core::Result<rhi::RenderFramePlan> build_plan() const;
    [[nodiscard]] core::Result<rhi::RenderFrameSubmission> build(const RenderCamera& camera,
                                                                 RenderCommandLists commands,
                                                                 rhi::RenderEnvironmentData environment = {}) const;

    [[nodiscard]] rhi::RenderExtent extent() const noexcept;

  private:
    [[nodiscard]] core::Result<rhi::RenderFramePlan> build_legacy_ldr_plan() const;
    [[nodiscard]] core::Result<rhi::RenderFramePlan> build_linear_hdr_plan() const;

    rhi::RenderExtent extent_{};
    rhi::ClearColor clear_color_{};
    FrameColorPipeline color_pipeline_ = FrameColorPipeline::legacy_ldr;
    rhi::RenderExposureSettings exposure_{};
};

} // namespace heartstead::renderer
