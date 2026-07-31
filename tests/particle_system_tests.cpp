#include "engine/modding/generic_prototype.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/renderer/environment/weather_effects.hpp"
#include "engine/renderer/particles/particle_prototype.hpp"
#include "engine/renderer/particles/particle_system.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

heartstead::renderer::ParticlePrototype make_prototype(std::string_view id,
                                                       std::uint8_t material_group = 0) {
    using namespace heartstead;
    renderer::ParticlePrototype prototype;
    prototype.id = *core::PrototypeId::parse(id);
    prototype.material_group = material_group;
    prototype.lifetime_min_seconds = 2.0F;
    prototype.lifetime_max_seconds = 2.0F;
    prototype.speed_min = 1.0F;
    prototype.speed_max = 1.0F;
    prototype.direction_spread = 0.4F;
    prototype.gravity = -1.0F;
    prototype.drag = 0.1F;
    prototype.size_min = 0.2F;
    prototype.size_max = 0.2F;
    prototype.end_size_multiplier = 2.0F;
    prototype.start_color = {1.0F, 0.5F, 0.25F, 1.0F};
    prototype.end_color = {0.2F, 0.1F, 0.0F, 0.0F};
    prototype.atlas_columns = 2;
    prototype.atlas_rows = 2;
    prototype.atlas_frame_count = 4;
    prototype.atlas_frames_per_second = 8.0F;
    assert(prototype.validate());
    return prototype;
}

heartstead::renderer::CpuParticleSystem make_system(std::uint32_t capacity = 64) {
    heartstead::renderer::ParticleSystemConfig config;
    config.maximum_particles = capacity;
    config.maximum_emitters = 4;
    config.maximum_queued_events = 4;
    config.maximum_spawns_per_update = capacity;
    const std::vector prototypes{make_prototype("base:particles/test")};
    auto system = heartstead::renderer::CpuParticleSystem::create(config, prototypes);
    assert(system);
    return std::move(system).value();
}

void assert_particle_equal(const heartstead::renderer::ParticleState& left,
                           const heartstead::renderer::ParticleState& right) {
    assert(left.serial == right.serial);
    assert(left.prototype_id == right.prototype_id);
    assert(left.previous_position == right.previous_position);
    assert(left.position == right.position);
    assert(left.velocity == right.velocity);
    assert(left.age_seconds == right.age_seconds);
    assert(left.lifetime_seconds == right.lifetime_seconds);
    assert(left.start_size == right.start_size);
    assert(left.roll_degrees == right.roll_degrees);
    assert(left.atlas_frame() == right.atlas_frame());
}

} // namespace

int main() {
    using namespace heartstead;

    auto first = make_system();
    auto second = make_system();
    const renderer::ParticleEmitEvent event{
        *core::PrototypeId::parse("base:particles/test"),
        world::WorldPosition{9'000'000'000'000.25, 4.0, -12.5},
        {0.0F, 1.0F, 0.0F},
        {0.25F, 0.0F, -0.5F},
        12,
        0x1234ULL,
    };
    assert(first.queue_event(event));
    assert(second.queue_event(event));
    for (std::uint32_t tick = 0; tick < 20; ++tick) {
        assert(first.update(1.0F / 60.0F));
        assert(second.update(1.0F / 60.0F));
        assert(first.particles().size() == second.particles().size());
        for (std::size_t index = 0; index < first.particles().size(); ++index) {
            assert_particle_equal(first.particles()[index], second.particles()[index]);
        }
    }
    assert(first.stats().active_particles == 12);
    assert(first.particles().front().size() > 0.2F);
    assert(first.particles().front().color()[3] < 1.0F);
    assert(first.particles().front().atlas_frame() != 0);

    auto bounded = make_system(4);
    auto too_many = event;
    too_many.count = 10;
    assert(bounded.queue_event(too_many));
    assert(bounded.update(1.0F / 60.0F));
    assert(bounded.stats().active_particles == 4);
    assert(bounded.stats().dropped_particles == 6);

    auto emitter_system = make_system();
    renderer::ParticleEmitterDesc emitter;
    emitter.prototype_id = event.prototype_id;
    emitter.position = event.position;
    emitter.lifetime_seconds = 0.25F;
    emitter.rate_per_second = 10.0F;
    emitter.burst_count = 2;
    emitter.seed = 44;
    auto emitter_id = emitter_system.create_emitter(emitter);
    assert(emitter_id);
    assert(emitter_system.update(0.1F));
    assert(emitter_system.stats().spawned_this_update == 3);
    assert(emitter_system.stats().active_emitters == 1);
    assert(emitter_system.update_emitter(emitter_id.value(), world::WorldPosition{1.0, 2.0, 3.0},
                                         {1.0F, 1.0F, 0.0F}));
    assert(emitter_system.update(0.1F));
    assert(emitter_system.stats().spawned_this_update == 1);
    assert(emitter_system.update(0.1F));
    assert(emitter_system.stats().active_emitters == 0);
    assert(!emitter_system.destroy_emitter(emitter_id.value()));

    modding::GenericPrototype generic{
        std::string(modding::PrototypeKinds::particle),
        *core::PrototypeId::parse("base:particles/parsed"),
        "Parsed",
        {},
        std::unordered_map<std::string, std::string>{
            {"kind", "particle"},
            {"material_group", "2"},
            {"lifetime_min_seconds", "0.2"},
            {"lifetime_max_seconds", "0.6"},
            {"start_color", "1.0,0.5,0.25,1.0"},
            {"end_color", "0.1,0.2,0.3,0.0"},
            {"atlas_columns", "4"},
            {"atlas_rows", "2"},
            {"atlas_frame_count", "8"},
            {"atlas_frames_per_second", "12"},
            {"blend_mode", "premultiplied_alpha"},
            {"shading", "emissive"},
            {"geometry", "mesh"},
            {"alignment", "velocity"},
            {"simulation_space", "local"},
            {"collision", "voxel"},
            {"mesh_group", "3"},
            {"emissive_intensity", "4.5"},
            {"wind_response", "1.25"},
            {"soft_fade_distance", "0.4"},
            {"velocity_stretch", "1.8"},
            {"collision_radius", "0.08"},
            {"collision_restitution", "0.6"},
            {"lod_start_distance", "30"},
            {"lod_end_distance", "90"},
            {"maximum_live_particles", "123"},
            {"spawn_budget_per_update", "17"},
            {"priority", "3"},
        },
    };
    auto parsed = renderer::particle_prototype_from_generic(generic);
    assert(parsed);
    assert(parsed.value().material_group == 2);
    assert(parsed.value().atlas_frame_count == 8);
    assert(parsed.value().blend_mode == renderer::ParticleBlendMode::premultiplied_alpha);
    assert(parsed.value().shading == renderer::ParticleShading::emissive);
    assert(parsed.value().geometry == renderer::ParticleGeometry::mesh);
    assert(parsed.value().alignment == renderer::ParticleAlignment::velocity);
    assert(parsed.value().simulation_space == renderer::ParticleSimulationSpace::local);
    assert(parsed.value().collision_mode == renderer::ParticleCollisionMode::voxel);
    assert(parsed.value().mesh_group == 3);
    assert(parsed.value().emissive_intensity == 4.5F);
    assert(parsed.value().wind_response == 1.25F);
    assert(parsed.value().soft_fade_distance == 0.4F);
    assert(parsed.value().maximum_live_particles == 123);
    assert(parsed.value().spawn_budget_per_update == 17);
    generic.fields["start_color"] = "1,2,3";
    assert(!renderer::particle_prototype_from_generic(generic));

    std::vector<renderer::ParticlePrototype> weather_prototypes;
    for (const auto id : {"base:particles/rain_drop", "base:particles/snow_flake",
                          "base:particles/dust_mote", "base:particles/spore"}) {
        auto prototype = make_prototype(id);
        prototype.lod_start_distance = 128.0F;
        prototype.lod_end_distance = 256.0F;
        weather_prototypes.push_back(prototype);
    }
    renderer::ParticleSystemConfig weather_system_config;
    weather_system_config.maximum_particles = 64;
    weather_system_config.maximum_emitters = 1;
    weather_system_config.maximum_queued_events = 64;
    weather_system_config.maximum_spawns_per_update = 64;
    auto weather_particles =
        renderer::CpuParticleSystem::create(weather_system_config, weather_prototypes);
    assert(weather_particles);
    renderer::WeatherEffects weather;
    renderer::WeatherEffectsConfig weather_config;
    weather_config.rain_particles_per_second = 60.0F;
    weather_config.maximum_spawns_per_update = 8;
    weather_config.horizontal_radius = 12.0F;
    weather_config.minimum_spawn_height = 6.0F;
    weather_config.maximum_spawn_height = 10.0F;
    assert(weather.initialize(weather_particles.value(), weather_config));
    renderer::EvaluatedEnvironment rainy_environment;
    rainy_environment.weather.precipitation =
        renderer::EnvironmentPrecipitation::rain;
    rainy_environment.weather.precipitation_intensity = 0.5F;
    rainy_environment.wind.direction = {1.0F, 0.0F, 0.0F};
    rainy_environment.wind.speed = 4.0F;
    const world::WorldPosition weather_viewpoint{
        5'000'000'000.0, 80.0, -5'000'000'000.0};
    assert(weather.update(rainy_environment, weather_viewpoint, 0.1F));
    assert(weather.stats().emitted_this_update == 3);
    assert(weather_particles.value().stats().queued_events == 3);
    assert(weather_particles.value().update(0.1F));
    assert(weather_particles.value().stats().active_particles == 3);
    for (const auto& particle : weather_particles.value().particles()) {
        const auto delta =
            particle.position.relative_to(weather_viewpoint.anchor) -
            weather_viewpoint.local_offset;
        assert(std::abs(delta.x) <= 16.0);
        assert(delta.y >= 3.0 && delta.y <= 12.0);
        assert(std::abs(delta.z) <= 16.0);
    }
    rainy_environment.weather.precipitation =
        renderer::EnvironmentPrecipitation::none;
    assert(weather.update(rainy_environment, weather_viewpoint, 0.1F));
    assert(weather.stats().emitted_this_update == 0);

    return 0;
}
