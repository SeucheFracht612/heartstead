#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/terrain/far_terrain_renderer.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <utility>

int main() {
    using namespace heartstead;

    renderer::rhi::RenderDeviceDesc device_desc;
    device_desc.backend = renderer::rhi::RenderBackend::headless;
    auto created_device = renderer::rhi::create_render_device(device_desc);
    assert(created_device);
    auto device = std::move(created_device).value();
    const auto baseline_resources = device->live_resource_count();
    renderer::rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = *core::PrototypeId::parse("base:materials/far_terrain");
    layout.shader_template = {"base", "shaders/far_terrain.vert"};
    layout.descriptors = {
        {"far_patch_draws", renderer::rhi::RenderDescriptorKind::storage_buffer, 15, true,
         renderer::rhi::RenderShaderStageFlags::vertex},
    };
    assert(device->bind_pipeline_layout(std::move(layout)));

    renderer::FarTerrainRendererConfig config;
    config.clipmap.level_count = 3;
    config.clipmap.patches_per_axis = 5;
    config.clipmap.patch_resolution = 4;
    config.clipmap.base_cell_size = 2.0;
    config.clipmap.maximum_distance = 2'048.0;
    config.clipmap.inner_exclusion_radius = 16.0;
    config.maximum_patch_builds_per_frame = 3;
    config.maximum_upload_bytes_per_frame = 1U * 1024U * 1024U;
    config.maximum_resident_bytes = 16U * 1024U * 1024U;

    renderer::FarTerrainRenderer far_terrain(*device);
    assert(far_terrain.initialize(config, renderer::rhi::RenderResourceHandle{77}));
    const renderer::FarTerrainSurfaceSampler sampler =
        [](double x, double z, renderer::FarTerrainDomain) {
            return renderer::FarTerrainSurfaceSample{
                24.0 + std::sin(x * 0.025) * 3.0 + std::cos(z * 0.025) * 2.0,
                1U,
            };
        };

    assert(far_terrain.update({0.0, 40.0, 0.0}, sampler));
    assert(far_terrain.stats().planned_patches > 0);
    assert(far_terrain.stats().built_patches == 3);
    assert(far_terrain.stats().resident_patches == 3);
    assert(far_terrain.stats().pending_patches > 0);
    // Four per-frame indirect buffers, four per-frame draw-data buffers, and one shared block in
    // each vertex/index arena stay constant as patch residency grows.
    assert(device->live_resource_count() == baseline_resources + 10U);

    for (std::size_t frame = 0; frame < 128U && far_terrain.stats().pending_patches > 0U;
         ++frame) {
        assert(far_terrain.update({0.0, 40.0, 0.0}, sampler));
    }
    assert(far_terrain.stats().pending_patches == 0);
    const auto settled_resident = far_terrain.stats().resident_patches;
    assert(settled_resident > 3U);
    renderer::RenderCamera camera;
    camera.local_position = {0.0F, 40.0F, 0.0F};
    camera.pitch_radians = -0.35F;
    camera.far_plane = 2'048.0F;
    assert(camera.update_matrices());
    const auto draws = far_terrain.build_draws(camera);
    assert(far_terrain.stats().visible_patches > 0U);
    assert(!draws.empty());
    assert(draws.size() < far_terrain.stats().visible_patches);
    assert(draws.front().indirect.is_valid());

    assert(far_terrain.update({0.0, 40.0, 0.0}, sampler, 1));
    assert(far_terrain.stats().built_patches == 3);
    assert(far_terrain.stats().resident_patches == 3);
    assert(far_terrain.update({512.0, 40.0, 512.0}, sampler, 1));
    assert(far_terrain.stats().evicted_patches > 0);
    assert(far_terrain.stats().resident_patches <= settled_resident);

    const renderer::FarTerrainSurfaceSampler missing_sampler =
        [](double, double, renderer::FarTerrainDomain) {
            return renderer::FarTerrainSurfaceSample{0.0, 0, false};
        };
    for (std::size_t frame = 0; frame < 128U; ++frame) {
        assert(far_terrain.update({0.0, 40.0, 0.0}, missing_sampler, 2));
        if (far_terrain.stats().pending_patches == 0) {
            break;
        }
    }
    assert(far_terrain.stats().pending_patches == 0);
    assert(far_terrain.build_draws(camera).empty());

    assert(far_terrain.shutdown());
    assert(device->live_resource_count() == baseline_resources);
    return 0;
}
