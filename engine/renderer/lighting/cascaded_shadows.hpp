#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/lighting/clustered_lighting.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/scene/render_scene.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace heartstead::renderer {

inline constexpr std::uint32_t directional_shadow_cascade_count = 4;

struct DirectionalShadowConfig {
    std::uint32_t resolution = 2048;
    float distance = 320.0F;
    float split_lambda = 0.72F;
    float constant_bias = 0.0007F;
    float normal_bias = 0.0025F;
    float fade_fraction = 0.12F;

    [[nodiscard]] core::Status validate() const;
};

enum class LightingDebugView : std::uint32_t {
    none,
    base_color,
    normal,
    roughness,
    metallic,
    ambient_occlusion,
    emissive,
    shadow_cascades,
    local_light_tiles,
    uv0,
    uv1,
    tangents,
    vertex_colors,
    mip_level,
    texel_density,
    texture_residency,
    lod,
    bounds,
    skeletons,
    skin_weights,
    overdraw,
};

struct alignas(16) GpuDirectionalShadowData {
    std::array<math::Mat4f, directional_shadow_cascade_count> light_view_projection{};
    float split_distances[4]{};
    float parameters[4]{};
    float environment_parameters[4]{};
    float camera_position[4]{};
    std::array<math::Mat4f, local_shadow_map_count> local_light_view_projection{};
    std::array<std::array<float, 4>, local_shadow_map_count> local_parameters{};
};

static_assert(sizeof(GpuDirectionalShadowData) == 480);

class CascadedShadowSystem {
  public:
    explicit CascadedShadowSystem(rhi::IRenderDevice& device);
    ~CascadedShadowSystem();

    [[nodiscard]] core::Status initialize(DirectionalShadowConfig config = {});
    [[nodiscard]] core::Status update(const RenderCamera& camera, math::Vec3f sun_direction,
                                      float sky_diffuse_intensity,
                                      float environment_specular_intensity,
                                      float environment_rotation_radians,
                                      std::span<const RenderLightInstance> local_shadow_lights = {});
    [[nodiscard]] core::Status bind(core::PrototypeId material, std::string_view binding);
    [[nodiscard]] core::Status shutdown();
    void set_debug_view(LightingDebugView view) noexcept;

    [[nodiscard]] const DirectionalShadowConfig& config() const noexcept;
    [[nodiscard]] const GpuDirectionalShadowData& gpu_data() const noexcept;
    [[nodiscard]] rhi::RenderResourceHandle data_buffer() const noexcept;

  private:
    rhi::IRenderDevice* device_ = nullptr;
    DirectionalShadowConfig config_{};
    LightingDebugView debug_view_ = LightingDebugView::none;
    GpuDirectionalShadowData gpu_data_{};
    rhi::RenderResourceHandle data_buffer_{};
};

} // namespace heartstead::renderer
