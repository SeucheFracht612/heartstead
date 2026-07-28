#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"
#include "engine/simulation/world_time.hpp"

namespace heartstead::renderer {

struct DayNightCycleConfig {
    float solar_noon_fraction = 0.5F;
    float sun_path_azimuth_radians = 0.55F;
    float twilight_elevation_width = 0.12F;
    float maximum_sun_intensity = 0.9F;
    math::Vec3f night_ambient{0.018F, 0.024F, 0.052F};
    math::Vec3f day_ambient{0.30F, 0.34F, 0.40F};
    math::Vec3f night_horizon{0.008F, 0.014F, 0.040F};
    math::Vec3f day_horizon{0.30F, 0.48F, 0.70F};
    math::Vec3f twilight_ambient{0.055F, 0.020F, 0.008F};
    math::Vec3f twilight_horizon{0.36F, 0.105F, 0.018F};
    float fog_start = 384.0F;
    float fog_end = 512.0F;

    [[nodiscard]] core::Status validate() const;
};

struct DayNightEnvironment {
    rhi::RenderEnvironmentData render;
    float day_fraction = 0.0F;
    float solar_elevation = -1.0F;
    float daylight = 0.0F;
};

[[nodiscard]] core::Result<DayNightEnvironment>
evaluate_day_night(simulation::WorldTick world_time,
                   const simulation::WorldTimeConfig& world_time_config,
                   const DayNightCycleConfig& cycle = {});

} // namespace heartstead::renderer
