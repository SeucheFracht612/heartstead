#include "engine/renderer/renderer.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/water/large_water_renderer.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
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

} // namespace

int main() {
    using namespace heartstead;
    renderer::Renderer renderer;
    initialize_renderer(renderer);

    renderer::LargeWaterRenderer water;
    renderer::LargeWaterRendererConfig config;
    config.grid_resolution = 33;
    config.camera_snap_distance = 16.5F;
    assert(water.initialize(renderer, config));
    assert(water.stats().mesh_vertices == 33U * 33U);
    assert(water.stats().mesh_triangles == 32U * 32U * 2U);

    renderer::LargeWaterBodyDesc ocean;
    ocean.id = 1;
    ocean.center = world::WorldPosition{4'000'000'000.0, 12.25, -4'000'000'000.0};
    ocean.half_extent = 1'600.0F;
    assert(water.add_body(ocean));
    assert(water.stats().retained_bodies == 1);
    assert(water.stats().ocean_bodies == 1);

    renderer::RenderCamera camera;
    camera.floating_origin.block = ocean.center.anchor;
    camera.local_position = {47.5F, 8.0F, -35.25F};
    camera.far_plane = 2'000.0F;
    assert(camera.update_matrices());
    assert(water.synchronize(camera));
    assert(water.stats().camera_recenters == 1);
    assert(water.synchronize(camera));
    assert(water.stats().camera_recenters == 1);
    const auto first = water.inspect_body(ocean.id);
    assert(first.has_value());

    camera.floating_origin.block.x += 32;
    camera.local_position.x -= 32.0F;
    assert(camera.update_matrices());
    assert(water.synchronize(camera));
    const auto rebased = water.inspect_body(ocean.id);
    assert(rebased.has_value());
    assert(water.stats().camera_recenters == 1);
    assert(rebased->render_center == first->render_center);
    assert(rebased->world_wave_phase == first->world_wave_phase);

    auto frame = renderer.render(camera, 1.0F, 1.0F / 60.0F);
    assert(frame);
    assert(renderer.scene_stats().submitted_instances == 1);
    assert(renderer.scene_stats().draw_calls == 1);

    camera.local_position.x += 33.0F;
    assert(camera.update_matrices());
    assert(water.synchronize(camera));
    assert(water.stats().camera_recenters == 2);
    const auto moved = water.inspect_body(ocean.id);
    assert(moved.has_value());
    const auto phase_delta = std::remainder(
        static_cast<double>(moved->world_wave_phase.x - first->world_wave_phase.x), 4096.0);
    const auto center_delta =
        moved->render_center.relative_to(first->render_center.anchor).x -
        first->render_center.local_offset.x;
    assert(std::abs(phase_delta - center_delta) < 0.001);

    assert(water.remove_body(ocean.id));
    assert(water.shutdown());
    assert(renderer.shutdown());
    return 0;
}
