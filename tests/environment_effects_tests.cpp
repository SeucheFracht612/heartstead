#include "engine/renderer/effects/surface_mark_renderer.hpp"
#include "engine/renderer/effects/trail_renderer.hpp"
#include "engine/renderer/renderer.hpp"
#include "engine/renderer/rhi/render_device.hpp"

#include <cassert>
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

heartstead::renderer::RenderCamera test_camera() {
    heartstead::renderer::RenderCamera camera;
    camera.floating_origin.block = {1'000'000'000, 72, -1'000'000'000};
    camera.local_position = {0.0F, 3.0F, 7.0F};
    camera.far_plane = 1'000.0F;
    assert(camera.update_matrices());
    return camera;
}

void test_surface_marks(heartstead::renderer::Renderer& renderer,
                        const heartstead::renderer::RenderCamera& camera) {
    using namespace heartstead;
    renderer::SurfaceMarkPrototype mark;
    mark.id = *core::PrototypeId::parse("test:decal/footprint");
    mark.display_name = "Footprint";
    mark.size_min = 0.5F;
    mark.size_max = 0.5F;
    mark.lifetime_seconds = 0.2F;
    mark.fade_seconds = 0.1F;
    mark.maximum_distance = 128.0F;
    mark.atlas_columns = 2;
    mark.atlas_frame_count = 2;

    renderer::SurfaceMarkPrototypeRegistry registry;
    assert(registry.add(mark));
    assert(registry.size() == 1);

    renderer::SurfaceMarkRenderer marks;
    renderer::SurfaceMarkRendererConfig config;
    config.maximum_marks = 8;
    config.maximum_spawns_per_update = 2;
    assert(marks.initialize(renderer, registry, config));

    renderer::SurfaceMarkSpawn spawn;
    spawn.prototype = mark.id;
    spawn.position = world::WorldPosition{1'000'000'000.25, 72.0,
                                          -999'999'999.75};
    spawn.normal = {0.0F, 1.0F, 0.0F};
    spawn.seed = 42;
    assert(marks.spawn(spawn));
    spawn.position = world::WorldPosition{1'000'000'001.25, 72.0,
                                          -999'999'999.75};
    spawn.seed = 43;
    assert(marks.spawn(spawn));
    assert(!marks.spawn(spawn));
    assert(marks.stats().retained_marks == 2);
    assert(marks.stats().dropped_marks == 1);

    auto frame = renderer.render(camera, 1.0F, 1.0F / 60.0F);
    assert(frame);
    assert(renderer.scene_stats().submitted_instances == 2);
    assert(renderer.scene_stats().draw_calls == 1);

    assert(marks.update(0.11F));
    assert(marks.stats().retained_marks == 2);
    assert(marks.update(0.1F));
    assert(marks.stats().retained_marks == 0);
    assert(marks.stats().expired_this_update == 2);
    assert(marks.shutdown());
}

void test_trails(heartstead::renderer::Renderer& renderer,
                 const heartstead::renderer::RenderCamera& camera) {
    using namespace heartstead;
    renderer::TrailRenderer trails;
    renderer::TrailRendererConfig config;
    config.maximum_trails = 4;
    config.maximum_segments = 8;
    assert(trails.initialize(renderer, config));

    renderer::TrailDesc desc;
    desc.minimum_point_distance = 0.1F;
    desc.segment_lifetime_seconds = 0.2F;
    desc.maximum_segments = 4;
    desc.emissive = true;
    desc.emissive_intensity = 3.0F;
    auto trail = trails.create_trail(desc);
    assert(trail);

    assert(trails.append_point(
        trail.value(), world::WorldPosition{1'000'000'000.0, 73.0, -1'000'000'000.0}));
    assert(trails.append_point(
        trail.value(), world::WorldPosition{1'000'000'001.0, 73.5, -1'000'000'000.0}));
    assert(trails.append_point(
        trail.value(), world::WorldPosition{1'000'000'002.0, 74.0, -999'999'999.5}));
    assert(trails.stats().retained_segments == 2);
    assert(trails.update(camera, 0.05F));

    auto frame = renderer.render(camera, 1.0F, 1.0F / 60.0F);
    assert(frame);
    assert(renderer.scene_stats().submitted_instances == 2);
    assert(renderer.scene_stats().draw_calls == 1);

    assert(trails.update(camera, 0.16F));
    assert(trails.stats().retained_segments == 0);
    const auto old_id = trail.value();
    assert(trails.destroy_trail(old_id));
    assert(!trails.append_point(
        old_id, world::WorldPosition{1'000'000'003.0, 74.0, -999'999'999.0}));
    auto replacement = trails.create_trail(desc);
    assert(replacement);
    assert(replacement.value().index == old_id.index);
    assert(replacement.value().generation != old_id.generation);
    assert(trails.destroy_trail(replacement.value()));
    assert(trails.shutdown());
}

} // namespace

int main() {
    heartstead::renderer::Renderer renderer;
    initialize_renderer(renderer);
    const auto camera = test_camera();
    test_surface_marks(renderer, camera);
    test_trails(renderer, camera);
    assert(renderer.shutdown());
    return 0;
}
