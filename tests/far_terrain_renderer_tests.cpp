#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/terrain/far_terrain_renderer.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <thread>
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
    config.lod_updates.mid_level_count = 2;
    config.lod_updates.maximum_mid_rebuilds_per_frame = 1;
    config.lod_updates.maximum_far_rebuilds_per_frame = 1;
    config.maximum_patch_builds_per_frame = 3;
    config.mesh_scheduler.worker_count = 1;
    config.mesh_scheduler.maximum_concurrent_jobs = 3;
    config.maximum_upload_bytes_per_frame = 1U * 1024U * 1024U;
    config.maximum_resident_bytes = 16U * 1024U * 1024U;

    renderer::FarTerrainRenderer far_terrain(*device);
    assert(far_terrain.initialize(config, renderer::rhi::RenderResourceHandle{77}));
    double edited_height = 0.0;
    const renderer::FarTerrainSurfaceSampler sampler =
        [&edited_height](double x, double z, renderer::FarTerrainDomain) {
            return renderer::FarTerrainSurfaceSample{
                24.0 + edited_height + std::sin(x * 0.025) * 3.0 + std::cos(z * 0.025) * 2.0,
                1U,
            };
        };

    assert(far_terrain.update({0.0, 40.0, 0.0}, sampler));
    assert(far_terrain.stats().planned_patches > 0);
    assert(far_terrain.stats().built_patches == 0);
    assert(far_terrain.stats().resident_patches == 0);
    assert(far_terrain.stats().pending_patches > 0);
    assert(far_terrain.stats().worker_in_flight_meshes == 3);
    // Worker submission allocates no GPU memory. Only the four per-frame indirect buffers and four
    // draw-data buffers exist until the first current mesh is published.
    assert(device->live_resource_count() == baseline_resources + 8U);

    for (std::size_t frame = 0; frame < 256U && far_terrain.stats().pending_patches > 0U;
         ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(far_terrain.update({0.0, 40.0, 0.0}, sampler));
    }
    assert(far_terrain.stats().pending_patches == 0);
    const auto settled_resident = far_terrain.stats().resident_patches;
    assert(settled_resident > 3U);
    // One shared vertex block and one shared index block then stay constant as residency grows.
    assert(device->live_resource_count() == baseline_resources + 10U);
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

    // A broad authoritative edit invalidates every active mid/far derivative. Exactly one patch
    // from each band is replaced this frame, while all other old patches remain resident and
    // drawable until their current ticket publishes.
    const std::array invalidated_regions{
        math::Bounds3d{{-2'048.0, 0.0, -2'048.0}, {2'048.0, 1.0, 2'048.0}},
    };
    edited_height = 12.0;
    assert(far_terrain.update({0.0, 40.0, 0.0}, sampler, 1, invalidated_regions));
    assert(far_terrain.stats().resident_patches == settled_resident);
    assert(far_terrain.stats().stale_resident_patches > 0);
    assert(far_terrain.stats().pending_mid_updates > 0);
    assert(far_terrain.stats().pending_far_updates > 0);
    assert(far_terrain.stats().rebuilt_mid_patches == 0);
    assert(far_terrain.stats().rebuilt_far_patches == 0);
    assert(far_terrain.stats().replaced_patches == 0);
    assert(far_terrain.stats().evicted_patches == 0);
    assert(!far_terrain.build_draws(camera).empty());
    std::size_t rebuilt_mid = 0;
    std::size_t rebuilt_far = 0;
    for (std::size_t frame = 0; frame < 256U && far_terrain.stats().pending_patches > 0U;
         ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(far_terrain.update({0.0, 40.0, 0.0}, sampler, 1));
        assert(far_terrain.stats().resident_patches == settled_resident);
        assert(far_terrain.stats().rebuilt_mid_patches <= 1U);
        assert(far_terrain.stats().rebuilt_far_patches <= 1U);
        rebuilt_mid += far_terrain.stats().rebuilt_mid_patches;
        rebuilt_far += far_terrain.stats().rebuilt_far_patches;
        assert(!far_terrain.build_draws(camera).empty());
    }
    assert(far_terrain.stats().pending_patches == 0);
    assert(far_terrain.stats().stale_resident_patches == 0);
    assert(far_terrain.stats().total_invalidated_patches == settled_resident);
    assert(rebuilt_mid > 0U);
    assert(rebuilt_far > 0U);
    assert(rebuilt_mid + rebuilt_far == settled_resident);
    assert(device->live_resource_count() == baseline_resources + 10U);

    // Superseding an active edit never publishes its old immutable snapshot. The old resident
    // geometry stays drawable until the newest tickets converge.
    const auto stale_before = far_terrain.stats().total_stale_results;
    edited_height = 18.0;
    assert(far_terrain.update({0.0, 40.0, 0.0}, sampler, 2, invalidated_regions));
    edited_height = 24.0;
    assert(far_terrain.update({0.0, 40.0, 0.0}, sampler, 3, invalidated_regions));
    assert(far_terrain.stats().resident_patches == settled_resident);
    assert(!far_terrain.build_draws(camera).empty());
    for (std::size_t frame = 0; frame < 256U && far_terrain.stats().pending_patches > 0U;
         ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(far_terrain.update({0.0, 40.0, 0.0}, sampler, 3));
        assert(far_terrain.stats().resident_patches == settled_resident);
    }
    assert(far_terrain.stats().pending_patches == 0);
    assert(far_terrain.stats().total_stale_results > stale_before);

    assert(far_terrain.update({512.0, 40.0, 512.0}, sampler, 3));
    assert(far_terrain.stats().evicted_patches > 0);
    assert(far_terrain.stats().resident_patches <= settled_resident);

    const renderer::FarTerrainSurfaceSampler missing_sampler = [](double, double,
                                                                  renderer::FarTerrainDomain) {
        return renderer::FarTerrainSurfaceSample{0.0, 0, false};
    };
    for (std::size_t frame = 0; frame < 256U; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(far_terrain.update({0.0, 40.0, 0.0}, missing_sampler, 4));
        if (far_terrain.stats().pending_patches == 0) {
            break;
        }
    }
    assert(far_terrain.stats().pending_patches == 0);
    assert(far_terrain.build_draws(camera).empty());

    // Clear waits out and replaces the private worker pool, so a fresh graph cannot collide with
    // old request revisions for the same patch keys.
    assert(far_terrain.clear());
    assert(far_terrain.stats().resident_patches == 0U);
    assert(device->live_resource_count() == baseline_resources + 10U);
    assert(far_terrain.update({0.0, 40.0, 0.0}, missing_sampler, 4));
    assert(far_terrain.stats().pending_patches > 0U);

    assert(far_terrain.shutdown());
    assert(device->live_resource_count() == baseline_resources);
    return 0;
}
