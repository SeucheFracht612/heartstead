#include "engine/renderer/lighting/cascaded_shadows.hpp"
#include "engine/renderer/lighting/clustered_lighting.hpp"
#include "engine/renderer/rhi/render_device.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <memory>
#include <vector>

int main() {
    using namespace heartstead;
    using namespace heartstead::renderer;

    rhi::RenderDeviceDesc device_desc;
    device_desc.backend = rhi::RenderBackend::headless;
    device_desc.initial_extent = {1280, 720};
    auto created_device = rhi::create_render_device(device_desc);
    assert(created_device);
    auto device = std::move(created_device).value();
    const auto baseline = device->live_resource_count();

    RenderCamera camera;
    camera.local_position = {0.0F, 2.0F, 0.0F};
    assert(camera.set_aspect_ratio(1280.0F / 720.0F));
    assert(camera.update_matrices());

    ClusteredLightingConfig clustered_config;
    clustered_config.maximum_lights = 32;
    clustered_config.maximum_lights_per_tile = 8;
    clustered_config.local_shadow_budget = 2;
    ClusteredLightingSystem clustered(*device);
    assert(clustered.initialize({1280, 720}, clustered_config));

    std::vector<RenderLightInstance> lights;
    for (std::uint32_t index = 0; index < 5; ++index) {
        RenderLightInstance light;
        light.id = {index + 1U, 1U};
        light.kind = index % 2U == 0U ? RenderLightKind::spot : RenderLightKind::point;
        light.camera_relative_position = {static_cast<float>(index) - 2.0F, 2.0F, -10.0F};
        light.direction = {0.0F, -1.0F, 0.0F};
        light.color = {1.0F, 0.5F, 0.2F};
        light.intensity = 20.0F + static_cast<float>(index);
        light.radius = 8.0F;
        light.casts_shadow = true;
        light.gameplay_importance = index == 4U ? 4.0F : 1.0F;
        lights.push_back(light);
    }
    assert(clustered.update(lights, camera));
    assert(clustered.stats().submitted_lights == lights.size());
    assert(clustered.stats().populated_tiles > 0);
    assert(clustered.stats().selected_shadow_lights == 2);
    assert(clustered.selected_shadow_lights().front().id == lights.back().id);
    assert(clustered.gpu_lights().size() == lights.size());
    std::uint32_t assigned_shadow_slots = 0;
    for (std::size_t index = 0; index < lights.size(); ++index) {
        const auto slot = clustered.gpu_lights()[index].spot_shadow[2];
        if (slot > 0.0F) {
            ++assigned_shadow_slots;
            assert(lights[index].kind == RenderLightKind::spot);
            assert(slot == 1.0F || slot == 2.0F);
        }
    }
    assert(assigned_shadow_slots == 2U);
    assert(clustered.resize({640, 360}));
    assert(clustered.update(lights, camera));

    auto near_plane_light = lights.front();
    near_plane_light.camera_relative_position = camera.local_position - camera.forward() * 0.5F;
    near_plane_light.radius = 2.0F;
    near_plane_light.casts_shadow = false;
    assert(clustered.update(std::span{&near_plane_light, 1}, camera));
    assert(clustered.stats().populated_tiles > 0U);

    CascadedShadowSystem shadows(*device);
    assert(shadows.initialize());
    rhi::RenderEnvironmentData environment;
    assert(shadows.update(camera, environment));
    const auto first = shadows.gpu_data();
    assert(first.split_distances[0] > camera.near_plane);
    for (std::size_t index = 1; index < 4; ++index) {
        assert(first.split_distances[index] > first.split_distances[index - 1]);
        assert(first.light_view_projection[index].is_finite());
    }
    camera.local_position.x += 0.0001F;
    assert(camera.update_matrices());
    const std::array selected_spotlights{lights[4], lights[2]};
    environment.sky_diffuse_intensity = 0.8F;
    environment.environment_specular_intensity = 1.2F;
    environment.environment_rotation_radians = 0.4F;
    environment.wind_velocity = {2.0F, 0.0F, 1.0F};
    environment.wetness = 0.75F;
    environment.water_absorption_distance = 6.0F;
    assert(shadows.update(camera, environment, selected_spotlights));
    const auto second = shadows.gpu_data();
    assert(std::abs(second.environment_parameters[0] - 0.8F) < 1.0e-6F);
    assert(std::abs(second.environment_parameters[1] - 1.2F) < 1.0e-6F);
    assert(std::abs(second.wind_parameters[0] - 2.0F) < 1.0e-6F);
    assert(std::abs(second.weather_parameters[1] - 0.75F) < 1.0e-6F);
    assert(std::abs(second.water_shallow_absorption[3] - 6.0F) < 1.0e-6F);
    assert(std::abs(second.camera_position[0] - camera.local_position.x) < 1.0e-6F);
    assert(second.local_light_view_projection[0].is_finite());
    assert(second.local_parameters[0][3] == 1.0F);

    assert(shadows.shutdown());
    assert(clustered.shutdown());
    assert(device->live_resource_count() == baseline);
    assert(device->wait_idle());
    return 0;
}
