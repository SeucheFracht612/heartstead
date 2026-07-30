#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/rhi/render_device.hpp"

#include <string_view>

namespace heartstead::renderer {

// Owns the default prefiltered sky cubemap used by every lit material. Authored environment maps
// can replace this resource later without changing shader or material layouts.
class EnvironmentLighting {
  public:
    explicit EnvironmentLighting(rhi::IRenderDevice& device);
    ~EnvironmentLighting();

    [[nodiscard]] core::Status initialize();
    [[nodiscard]] core::Status bind(core::PrototypeId material, std::string_view binding);
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] rhi::RenderResourceHandle image() const noexcept;

  private:
    rhi::IRenderDevice* device_ = nullptr;
    rhi::RenderResourceHandle image_{};
    rhi::RenderResourceHandle sampler_{};
};

} // namespace heartstead::renderer
