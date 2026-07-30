#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/frame/render_commands.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"

#include <cstddef>
#include <cstdint>

namespace heartstead::renderer {

// Pass indices the frame builder assigns. Draw command lists
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
    [[nodiscard]] core::Status set_exposure(rhi::RenderExposureSettings exposure);
    [[nodiscard]] rhi::RenderExposureSettings exposure() const noexcept;
    // Pipeline the synthesized fullscreen tone map draw uses. Without it the linear HDR graph
    // builds a tone_map pass that records nothing, so the renderer supplies this at startup.
    void set_tone_map_pipeline(rhi::RenderResourceHandle pipeline) noexcept;
    [[nodiscard]] rhi::RenderResourceHandle tone_map_pipeline() const noexcept;

    [[nodiscard]] core::Result<rhi::RenderFramePlan> build_plan() const;
    [[nodiscard]] core::Result<rhi::RenderFrameSubmission> build(const RenderCamera& camera,
                                                                 RenderCommandLists commands,
                                                                 rhi::RenderEnvironmentData environment = {}) const;

    [[nodiscard]] rhi::RenderExtent extent() const noexcept;

  private:
    rhi::RenderExtent extent_{};
    rhi::ClearColor clear_color_{};
    rhi::RenderExposureSettings exposure_{};
    rhi::RenderResourceHandle tone_map_pipeline_{};
};

} // namespace heartstead::renderer
