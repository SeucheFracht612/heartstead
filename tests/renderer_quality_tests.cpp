#include "engine/renderer/quality/renderer_quality.hpp"
#include "engine/renderer/frame/frame_builder.hpp"

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
    auto invalid = renderer_quality_settings(RendererQualityPreset::high);
    invalid.render_scale = 0.1F;
    assert(!invalid.validate());

    FrameBuilder builder({1000, 500});
    const auto low = renderer_quality_settings(RendererQualityPreset::low);
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
    assert(scene != nullptr && output != nullptr);
    assert(scene->extent.width == 670 && scene->extent.height == 335);
    assert(output->extent.width == 1000 && output->extent.height == 500);
    assert(plan.value().passes[hdr_pass_index::bloom].kind ==
           rhi::RenderPassKind::clear);
    return 0;
}
