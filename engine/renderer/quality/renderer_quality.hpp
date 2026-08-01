#pragma once

#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace heartstead::renderer {

enum class RendererQualityPreset : std::uint8_t { low, medium, high, ultra };
enum class RendererAntiAliasing : std::uint8_t { off, fxaa };

struct RendererQualitySettings {
    RendererQualityPreset preset = RendererQualityPreset::high;
    float render_scale = 1.0F;
    RendererAntiAliasing anti_aliasing = RendererAntiAliasing::fxaa;
    std::uint32_t shadow_resolution = 2048;
    float shadow_distance = 320.0F;
    std::uint32_t local_shadow_budget = 2;
    std::uint32_t ambient_occlusion_quality = 2;
    std::uint32_t indirect_lighting_quality = 2;
    std::uint32_t volumetric_quality = 2;
    std::uint32_t water_quality = 2;
    float vegetation_density = 1.0F;
    float vegetation_distance = 320.0F;
    std::uint16_t terrain_chunk_radius = 16;
    float asset_lod_bias = 0.0F;
    float particle_budget_scale = 1.0F;
    std::size_t texture_budget_bytes = 512U * 1024U * 1024U;
    std::uint32_t reflection_quality = 2;
    bool bloom = true;

    [[nodiscard]] core::Status validate() const noexcept;
};

[[nodiscard]] RendererQualitySettings
renderer_quality_settings(RendererQualityPreset preset) noexcept;
[[nodiscard]] std::string_view renderer_quality_preset_name(RendererQualityPreset preset) noexcept;

} // namespace heartstead::renderer
