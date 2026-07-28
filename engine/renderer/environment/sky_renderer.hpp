#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"

#include <array>
#include <cstdint>

namespace heartstead::renderer {

struct GpuSkyVertex {
    math::Vec2f position;
};

inline constexpr std::array<rhi::RenderVertexAttributeDesc, 1> gpu_sky_vertex_attributes{{
    {0, 0, rhi::RenderVertexAttributeFormat::float2},
}};

class SkyRenderer {
  public:
    SkyRenderer(rhi::IRenderDevice& device, rhi::RenderResourceHandle pipeline);
    ~SkyRenderer();

    SkyRenderer(const SkyRenderer&) = delete;
    SkyRenderer& operator=(const SkyRenderer&) = delete;

    [[nodiscard]] core::Status initialize();
    [[nodiscard]] core::Status shutdown();
    [[nodiscard]] core::Result<rhi::RenderDrawCommand> build_draw() const;
    [[nodiscard]] bool is_initialized() const noexcept;

  private:
    rhi::IRenderDevice* device_ = nullptr;
    rhi::RenderResourceHandle pipeline_;
    rhi::RenderResourceHandle vertex_buffer_;
    rhi::RenderResourceHandle index_buffer_;
};

} // namespace heartstead::renderer
