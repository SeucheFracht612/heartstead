#include "engine/renderer/rhi/render_device.hpp"
#include "game/presentation/particle_presentation.hpp"

#include <cassert>
#include <cstdint>
#include <utility>
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
    assert(renderer.initialize(std::move(init)));
}

heartstead::renderer::ParticlePrototype prototype(std::string_view id, std::uint8_t group) {
    using namespace heartstead;
    renderer::ParticlePrototype result;
    result.id = *core::PrototypeId::parse(id);
    result.material_group = group;
    result.lifetime_min_seconds = 2.0F;
    result.lifetime_max_seconds = 2.0F;
    result.speed_max = 0.0F;
    result.gravity = 0.0F;
    result.size_min = 0.25F;
    result.size_max = 0.25F;
    return result;
}

} // namespace

int main() {
    using namespace heartstead;
    renderer::Renderer renderer;
    initialize_renderer(renderer);
    const std::vector prototypes{
        prototype("base:particles/one", 0),
        prototype("base:particles/two", 1),
        prototype("base:particles/three", 2),
    };
    renderer::ParticleSystemConfig system_config;
    system_config.maximum_particles = 8;
    system_config.maximum_emitters = 2;
    system_config.maximum_queued_events = 8;
    system_config.maximum_spawns_per_update = 8;
    auto system = renderer::CpuParticleSystem::create(system_config, prototypes);
    assert(system);
    for (std::size_t index = 0; index < prototypes.size(); ++index) {
        assert(system.value().queue_event(
            {prototypes[index].id, world::WorldPosition{static_cast<double>(index), 0.0, -5.0},
             {0.0F, 1.0F, 0.0F}, {}, 1, index + 1U}));
    }
    assert(system.value().update(1.0F / 60.0F));

    game::ParticlePresentation presentation;
    assert(presentation.initialize(renderer));
    renderer::RenderCamera camera;
    camera.yaw_radians = 0.4F;
    camera.pitch_radians = -0.2F;
    assert(camera.set_aspect_ratio(640.0F / 360.0F));
    auto synchronized = presentation.synchronize(renderer, system.value(), camera);
    assert(synchronized);
    assert(synchronized.value().inserted_particles == 3);
    assert(synchronized.value().material_groups == 3);
    auto frame = renderer.render(camera);
    assert(frame);
    assert(renderer.stats().submitted_instances == 3);
    assert(renderer.stats().instance_draw_calls == 1);

    assert(system.value().update(1.0F / 60.0F));
    synchronized = presentation.synchronize(renderer, system.value(), camera);
    assert(synchronized);
    assert(synchronized.value().updated_particles == 3);

    system.value().clear();
    synchronized = presentation.synchronize(renderer, system.value(), camera);
    assert(synchronized);
    assert(synchronized.value().removed_particles == 3);
    assert(synchronized.value().retained_particles == 0);

    assert(presentation.shutdown(renderer));
    assert(renderer.shutdown());
    return 0;
}
