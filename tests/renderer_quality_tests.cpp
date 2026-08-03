#include "engine/renderer/frame/frame_builder.hpp"
#include "engine/renderer/quality/renderer_quality.hpp"

#include <array>
#include <cassert>
#include <cstdio>

int main() {
    using namespace heartstead::renderer;
    constexpr std::array presets{RendererQualityPreset::low, RendererQualityPreset::medium,
                                 RendererQualityPreset::high, RendererQualityPreset::ultra};
    std::size_t previous_budget = 0;
    float previous_distance = 0.0F;
    for (const auto preset : presets) {
        const auto settings = renderer_quality_settings(preset);
        assert(settings.validate());
        assert(!renderer_quality_preset_name(preset).empty());
        assert(settings.texture_budget_bytes >= previous_budget);
        assert(settings.shadow_distance >= previous_distance);
        previous_budget = settings.texture_budget_bytes;
        previous_distance = settings.shadow_distance;
    }
    assert(renderer_quality_settings(RendererQualityPreset::low).terrain_shading ==
           RendererTerrainShading::simplified);
    assert(renderer_quality_settings(RendererQualityPreset::medium).terrain_shading ==
           RendererTerrainShading::full);
    assert(renderer_quality_settings(RendererQualityPreset::high).terrain_shading ==
           RendererTerrainShading::full);
    assert(renderer_quality_settings(RendererQualityPreset::ultra).terrain_shading ==
           RendererTerrainShading::full);
    const auto low_settings = renderer_quality_settings(RendererQualityPreset::low);
    assert(low_settings.render_scale == 0.5F);
    assert(low_settings.shadow_resolution == 256U);
    assert(low_settings.directional_shadow_cascades == 1U);
    assert(low_settings.local_shadow_resolution == 1U);
    assert(low_settings.shadow_distance == 48.0F);
    assert(low_settings.local_shadow_budget == 0U);
    assert(renderer_quality_settings(RendererQualityPreset::medium).local_shadow_resolution ==
           1024U);
    assert(renderer_quality_settings(RendererQualityPreset::high).local_shadow_resolution == 1024U);
    assert(renderer_quality_settings(RendererQualityPreset::ultra).local_shadow_resolution ==
           1024U);
    auto invalid = renderer_quality_settings(RendererQualityPreset::high);
    invalid.render_scale = 0.1F;
    assert(!invalid.validate());
    invalid = renderer_quality_settings(RendererQualityPreset::high);
    invalid.directional_shadow_cascades = 0U;
    assert(!invalid.validate());
    invalid = renderer_quality_settings(RendererQualityPreset::high);
    invalid.local_shadow_resolution = 1U;
    assert(!invalid.validate());

    FrameBuilder builder({1000, 500});
    const auto low = low_settings;
    assert(low.directional_shadow_cascades == 1U);
    assert(!builder.set_directional_shadow_cascade_count(0U));
    assert(!builder.set_local_shadow_resolution(128U));
    assert(builder.set_shadow_resolution(low.shadow_resolution));
    assert(builder.set_directional_shadow_cascade_count(low.directional_shadow_cascades));
    assert(builder.set_local_shadow_resolution(low.local_shadow_resolution));
    assert(builder.set_image_quality_settings(
        {low.render_scale, low.ambient_occlusion_quality != 0U,
         low.anti_aliasing != RendererAntiAliasing::off, low.bloom}));
    auto plan = builder.build_plan();
    if (!plan) {
        std::fprintf(stderr, "%s: %s\n", plan.error().code.c_str(), plan.error().message.c_str());
    }
    assert(plan);
    const auto* scene = plan.value().find_resource("scene_hdr");
    const auto* output = plan.value().find_resource("output");
    const auto* bloom = plan.value().find_resource("bloom_hdr");
    const auto* first_shadow = plan.value().find_resource("shadow_cascade_0");
    const auto* second_shadow = plan.value().find_resource("shadow_cascade_1");
    assert(scene != nullptr && output != nullptr && bloom != nullptr && first_shadow != nullptr &&
           second_shadow != nullptr);
    assert(scene->extent.width == 500 && scene->extent.height == 250);
    assert(output->extent.width == 1000 && output->extent.height == 500);
    assert(bloom->extent.width == 1 && bloom->extent.height == 1);
    assert(first_shadow->extent.width == low.shadow_resolution);
    assert(second_shadow->extent.width == 1U && second_shadow->extent.height == 1U);
    assert(plan.value().passes[hdr_pass_index::bloom].kind == rhi::RenderPassKind::clear);

    assert(builder.set_image_quality_settings({1.0F, true, true, true}));
    auto full_plan = builder.build_plan();
    assert(full_plan);
    const auto* full_bloom = full_plan.value().find_resource("bloom_hdr");
    assert(full_bloom != nullptr);
    assert(full_bloom->extent.width == 1000U && full_bloom->extent.height == 500U);
    assert(full_plan.value().passes[hdr_pass_index::bloom].kind ==
           rhi::RenderPassKind::post_process);
    return 0;
}
