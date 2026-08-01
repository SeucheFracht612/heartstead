#include "engine/renderer/quality/renderer_quality.hpp"

#include <cmath>

namespace heartstead::renderer {

core::Status RendererQualitySettings::validate() const noexcept {
    if (!std::isfinite(render_scale) || render_scale < 0.5F || render_scale > 1.0F ||
        shadow_resolution < 512U || shadow_resolution > 8192U ||
        (shadow_resolution & (shadow_resolution - 1U)) != 0U ||
        !std::isfinite(shadow_distance) || shadow_distance < 32.0F ||
        shadow_distance > 2'048.0F || local_shadow_budget > 2U ||
        ambient_occlusion_quality > 3U || indirect_lighting_quality > 3U ||
        volumetric_quality > 3U || water_quality > 3U || reflection_quality > 3U ||
        !std::isfinite(vegetation_density) || vegetation_density < 0.1F ||
        vegetation_density > 2.0F || !std::isfinite(vegetation_distance) ||
        vegetation_distance < 32.0F || vegetation_distance > 2'048.0F ||
        terrain_chunk_radius < 4U || terrain_chunk_radius > 64U ||
        !std::isfinite(asset_lod_bias) || asset_lod_bias < -2.0F || asset_lod_bias > 4.0F ||
        !std::isfinite(particle_budget_scale) || particle_budget_scale < 0.1F ||
        particle_budget_scale > 2.0F || texture_budget_bytes < 128U * 1024U * 1024U) {
        return core::Status::failure("renderer_quality.invalid_settings",
                                     "renderer quality values exceed supported production limits");
    }
    return core::Status::ok();
}

RendererQualitySettings renderer_quality_settings(RendererQualityPreset preset) noexcept {
    RendererQualitySettings settings;
    settings.preset = preset;
    switch (preset) {
    case RendererQualityPreset::low:
        settings.render_scale = 0.67F;
        settings.anti_aliasing = RendererAntiAliasing::off;
        settings.shadow_resolution = 1024;
        settings.shadow_distance = 160.0F;
        settings.local_shadow_budget = 0;
        settings.ambient_occlusion_quality = 0;
        settings.indirect_lighting_quality = 1;
        settings.volumetric_quality = 0;
        settings.water_quality = 1;
        settings.vegetation_density = 0.55F;
        settings.vegetation_distance = 160.0F;
        settings.terrain_chunk_radius = 8;
        settings.asset_lod_bias = 1.0F;
        settings.particle_budget_scale = 0.5F;
        settings.texture_budget_bytes = 256U * 1024U * 1024U;
        settings.reflection_quality = 0;
        settings.bloom = false;
        break;
    case RendererQualityPreset::medium:
        settings.render_scale = 0.85F;
        settings.shadow_resolution = 1024;
        settings.shadow_distance = 240.0F;
        settings.local_shadow_budget = 1;
        settings.ambient_occlusion_quality = 1;
        settings.indirect_lighting_quality = 1;
        settings.volumetric_quality = 1;
        settings.water_quality = 1;
        settings.vegetation_density = 0.75F;
        settings.vegetation_distance = 240.0F;
        settings.terrain_chunk_radius = 12;
        settings.asset_lod_bias = 0.5F;
        settings.particle_budget_scale = 0.75F;
        settings.texture_budget_bytes = 384U * 1024U * 1024U;
        settings.reflection_quality = 1;
        break;
    case RendererQualityPreset::high:
        break;
    case RendererQualityPreset::ultra:
        settings.shadow_resolution = 4096;
        settings.shadow_distance = 480.0F;
        settings.ambient_occlusion_quality = 3;
        settings.indirect_lighting_quality = 3;
        settings.volumetric_quality = 3;
        settings.water_quality = 3;
        settings.vegetation_density = 1.25F;
        settings.vegetation_distance = 480.0F;
        settings.terrain_chunk_radius = 24;
        settings.asset_lod_bias = -0.5F;
        settings.particle_budget_scale = 1.5F;
        settings.texture_budget_bytes = 768U * 1024U * 1024U;
        settings.reflection_quality = 3;
        break;
    }
    return settings;
}

std::string_view renderer_quality_preset_name(RendererQualityPreset preset) noexcept {
    switch (preset) {
    case RendererQualityPreset::low: return "Low";
    case RendererQualityPreset::medium: return "Medium";
    case RendererQualityPreset::high: return "High";
    case RendererQualityPreset::ultra: return "Ultra";
    }
    return "Unknown";
}

} // namespace heartstead::renderer
