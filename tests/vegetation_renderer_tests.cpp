#include "engine/content/content_validation.hpp"
#include "engine/renderer/renderer.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/vegetation/vegetation_renderer.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>

namespace {

void initialize_renderer(heartstead::renderer::Renderer& renderer) {
    using namespace heartstead;
    renderer::rhi::RenderDeviceDesc device_desc;
    device_desc.backend = renderer::rhi::RenderBackend::headless;
    device_desc.initial_extent = {640, 360};
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    const std::vector<std::uint32_t> spirv{0x07230203, 0x00010000, 0, 1, 0};
    renderer::RendererInitDesc init;
    init.device = std::move(device).value();
    init.sky_vertex_spirv = spirv;
    init.sky_fragment_spirv = spirv;
    init.terrain_vertex_spirv = spirv;
    init.terrain_fragment_spirv = spirv;
    init.static_mesh_vertex_spirv = spirv;
    init.static_mesh_fragment_spirv = spirv;
    init.debug_vertex_spirv = spirv;
    init.debug_fragment_spirv = spirv;
    init.ui_vertex_spirv = spirv;
    init.ui_fragment_spirv = spirv;
    init.tone_map_vertex_spirv = spirv;
    init.tone_map_fragment_spirv = spirv;
    assert(renderer.initialize(std::move(init)));
}

void test_generic_species_parsing() {
    using namespace heartstead;
    modding::GenericPrototype prototype;
    prototype.kind = std::string(modding::PrototypeKinds::vegetation_species);
    prototype.id = *core::PrototypeId::parse("test:vegetation/reed");
    prototype.fields = {
        {"display_name", "River Reed"},
        {"vegetation_kind", "kelp"},
        {"lod.0.model", "test:models/reed.gltf"},
        {"lod.0.maximum_distance", "42"},
        {"lod.0.transition_width", "5"},
        {"lod.1.model", "test:models/reed_billboard.gltf"},
        {"lod.1.maximum_distance", "96"},
        {"lod.1.transition_width", "12"},
        {"lod.1.density", "0.4"},
        {"lod.1.impostor", "true"},
        {"growth.states", "young,mature"},
        {"growth.young.scale", "0.45"},
        {"wind.stiffness", "0.08"},
        {"foliage.transmission", "0.7"},
        {"density.fade_start", "25"},
        {"density.fade_end", "50"},
        {"receives_weather", "false"},
    };
    const auto parsed = renderer::vegetation_species_from_generic(prototype);
    assert(parsed);
    assert(parsed.value().kind == renderer::VegetationKind::kelp);
    assert(parsed.value().lods.size() == 2);
    assert(parsed.value().lods[1].impostor);
    assert(parsed.value().growth_state("young") != nullptr);
    assert(parsed.value().growth_state("young")->scale_multiplier == 0.45F);
    assert(parsed.value().density_fade_start == 25.0F);
    assert(parsed.value().density_fade_end == 50.0F);
    assert(!parsed.value().receives_weather);
}

void test_production_registry_and_retained_instancing() {
    using namespace heartstead;
    const auto content =
        content::ContentValidation::validate(std::filesystem::path{HEARTSTEAD_TEST_SOURCE_DIR});
    assert(!content.has_errors());
    assert(content.vegetation_species.size() >= 7);

    renderer::Renderer renderer;
    initialize_renderer(renderer);
    renderer::VegetationRenderer vegetation;
    const auto initialized = vegetation.initialize(
        renderer, content.vegetation_species,
        std::filesystem::path{HEARTSTEAD_TEST_COOKED_ASSET_DIR});
    if (!initialized) {
        std::cerr << initialized.error().code << ": " << initialized.error().message << '\n';
    }
    assert(initialized);
    assert(vegetation.stats().loaded_species == content.vegetation_species.size());
    assert(vegetation.stats().loaded_models == 2);

    renderer::VegetationPatchDesc patch;
    patch.id = 77;
    patch.species = *core::PrototypeId::parse("base:vegetation/meadow_grass");
    patch.origin = world::WorldPosition{1'000'000'000.25, 8.0, -1'000'000'000.75};
    patch.extent = {12.0F, 10.0F};
    patch.instance_count = 24;
    patch.seed = 0x1234'5678ULL;
    patch.growth_state = "mature";
    const auto inserted = vegetation.upsert_patch(patch, [](float x, float z) {
        return 0.1F * std::sin(x * 0.25F) * std::cos(z * 0.25F);
    });
    if (!inserted) {
        std::cerr << inserted.error().code << ": " << inserted.error().message << '\n';
    }
    assert(inserted);
    const auto first_stats = vegetation.stats();
    assert(first_stats.retained_patches == 1);
    assert(first_stats.logical_instances == patch.instance_count);
    assert(first_stats.render_objects > patch.instance_count);
    assert(first_stats.density_rejected_lods > 0);

    // Replacing a patch with the same seed must preserve deterministic placement while retaining
    // exactly one patch worth of scene objects.
    assert(vegetation.upsert_patch(patch));
    assert(vegetation.stats().retained_patches == 1);
    assert(vegetation.stats().logical_instances == patch.instance_count);
    assert(vegetation.stats().render_objects == first_stats.render_objects);

    renderer::RenderCamera camera;
    camera.floating_origin.block = patch.origin.anchor;
    camera.local_position = {6.0F, 4.0F, 18.0F};
    camera.far_plane = 512.0F;
    assert(camera.update_matrices());
    const std::array occluders{
        math::Bounds3f{{-12.0F, -64.0F, 13.0F}, {24.0F, 64.0F, 16.0F}},
    };
    assert(vegetation.update_occlusion(camera, occluders));
    assert(vegetation.stats().occluded_patches == 0);
    assert(vegetation.update_occlusion(camera, occluders));
    assert(vegetation.stats().occluded_patches == 1);
    assert(vegetation.stats().visibility_updates == 1);
    assert(vegetation.update_occlusion(camera, {}));
    assert(vegetation.stats().occluded_patches == 0);
    assert(vegetation.stats().visibility_updates == 2);

    auto rendered = renderer.render(camera, 1.0F, 1.0F / 60.0F);
    assert(rendered);
    assert(renderer.scene_stats().submitted_instances > 0);
    assert(renderer.scene_stats().draw_calls <
           renderer.scene_stats().submitted_instances);

    assert(vegetation.remove_patch(patch.id));
    assert(vegetation.stats().retained_patches == 0);
    assert(vegetation.shutdown());
    assert(renderer.shutdown());
}

} // namespace

int main() {
    test_generic_species_parsing();
    test_production_registry_and_retained_instancing();
    return 0;
}
