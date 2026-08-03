#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/scene/render_scene.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace heartstead::renderer {

inline constexpr std::uint32_t local_shadow_map_count = 2;

struct ClusteredLightingConfig {
    std::uint32_t tile_size = 16;
    std::uint32_t maximum_lights = 1024;
    std::uint32_t maximum_lights_per_tile = 32;
    std::uint32_t local_shadow_budget = local_shadow_map_count;

    [[nodiscard]] core::Status validate() const;
};

// std430-compatible local light record shared by terrain and imported-asset shaders.
struct alignas(16) GpuLocalLight {
    float position_radius[4]{};
    float direction_kind[4]{};
    float color_intensity[4]{};
    float spot_shadow[4]{};
};

static_assert(sizeof(GpuLocalLight) == 64);

struct LocalShadowCandidate {
    RenderLightId id;
    float score = 0.0F;
    std::uint32_t light_index = 0;
};

struct ClusteredLightingStats {
    std::uint32_t submitted_lights = 0;
    std::uint32_t dropped_lights = 0;
    std::uint32_t tile_count = 0;
    std::uint32_t populated_tiles = 0;
    std::uint32_t maximum_tile_light_count = 0;
    std::uint32_t selected_shadow_lights = 0;
    std::uint64_t uploaded_bytes = 0;
};

class ClusteredLightingSystem {
  public:
    explicit ClusteredLightingSystem(rhi::IRenderDevice& device);
    ~ClusteredLightingSystem();

    ClusteredLightingSystem(const ClusteredLightingSystem&) = delete;
    ClusteredLightingSystem& operator=(const ClusteredLightingSystem&) = delete;

    [[nodiscard]] core::Status initialize(rhi::RenderExtent extent,
                                          ClusteredLightingConfig config = {});
    [[nodiscard]] core::Status resize(rhi::RenderExtent extent);
    [[nodiscard]] core::Status update(std::span<const RenderLightInstance> lights,
                                      const RenderCamera& camera);
    [[nodiscard]] core::Status bind(core::PrototypeId material, std::string_view light_binding,
                                    std::string_view grid_binding);
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] rhi::RenderResourceHandle light_buffer() const noexcept;
    [[nodiscard]] rhi::RenderResourceHandle grid_buffer() const noexcept;
    [[nodiscard]] const ClusteredLightingStats& stats() const noexcept;
    [[nodiscard]] std::span<const LocalShadowCandidate> selected_shadow_lights() const noexcept;
    [[nodiscard]] std::span<const GpuLocalLight> gpu_lights() const noexcept;

  private:
    [[nodiscard]] core::Status create_grid_buffer(rhi::RenderExtent extent);

    rhi::IRenderDevice* device_ = nullptr;
    ClusteredLightingConfig config_{};
    rhi::RenderExtent extent_{};
    rhi::RenderResourceHandle light_buffer_{};
    rhi::RenderResourceHandle grid_buffer_{};
    std::vector<GpuLocalLight> gpu_lights_;
    std::vector<std::uint32_t> grid_;
    std::vector<LocalShadowCandidate> shadow_candidates_;
    std::map<RenderLightId, std::pair<std::uint64_t, std::uint64_t>> observed_shadow_revisions_;
    ClusteredLightingStats stats_{};
    bool empty_grid_resident_ = false;
};

} // namespace heartstead::renderer
