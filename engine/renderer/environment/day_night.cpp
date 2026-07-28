#include "engine/renderer/environment/day_night.hpp"

#include "engine/math/vector.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace heartstead::renderer {

namespace {

[[nodiscard]] bool nonnegative(math::Vec3f value) noexcept {
    return value.is_finite() && value.x >= 0.0F && value.y >= 0.0F && value.z >= 0.0F;
}

[[nodiscard]] float smoothstep(float edge0, float edge1, float value) noexcept {
    const auto normalized = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return normalized * normalized * (3.0F - 2.0F * normalized);
}

[[nodiscard]] math::Vec3f mix(math::Vec3f left, math::Vec3f right, float amount) noexcept {
    return left * (1.0F - amount) + right * amount;
}

} // namespace

core::Status DayNightCycleConfig::validate() const {
    if (!std::isfinite(solar_noon_fraction) || solar_noon_fraction < 0.0F ||
        solar_noon_fraction >= 1.0F || !std::isfinite(sun_path_azimuth_radians) ||
        !std::isfinite(twilight_elevation_width) || twilight_elevation_width <= 0.0F ||
        twilight_elevation_width > 1.0F || !std::isfinite(maximum_sun_intensity) ||
        maximum_sun_intensity < 0.0F || !nonnegative(night_ambient) || !nonnegative(day_ambient) ||
        !nonnegative(night_horizon) || !nonnegative(day_horizon) ||
        !nonnegative(twilight_ambient) || !nonnegative(twilight_horizon) ||
        !std::isfinite(fog_start) || !std::isfinite(fog_end) || fog_start < 0.0F ||
        fog_end <= fog_start) {
        return core::Status::failure(
            "day_night.invalid_config",
            "day/night timing, colors, intensity, and fog ranges must be finite and valid");
    }
    return core::Status::ok();
}

core::Result<DayNightEnvironment>
evaluate_day_night(simulation::WorldTick world_time,
                   const simulation::WorldTimeConfig& world_time_config,
                   const DayNightCycleConfig& cycle) {
    auto status = world_time_config.validate();
    if (!status) {
        return core::Result<DayNightEnvironment>::failure(status.error().code,
                                                          status.error().message);
    }
    status = cycle.validate();
    if (!status) {
        return core::Result<DayNightEnvironment>::failure(status.error().code,
                                                          status.error().message);
    }
    auto ticks_per_day = world_time_config.ticks_per_day();
    if (!ticks_per_day) {
        return core::Result<DayNightEnvironment>::failure(ticks_per_day.error().code,
                                                          ticks_per_day.error().message);
    }

    const auto tick_in_day = world_time % ticks_per_day.value();
    const auto day_fraction =
        static_cast<double>(tick_in_day) / static_cast<double>(ticks_per_day.value());
    constexpr auto two_pi = std::numbers::pi_v<double> * 2.0;
    const auto orbit = (day_fraction - static_cast<double>(cycle.solar_noon_fraction)) * two_pi +
                       std::numbers::pi_v<double> * 0.5;
    const auto elevation = static_cast<float>(std::sin(orbit));
    const auto horizontal = static_cast<float>(std::cos(orbit));
    const auto path_cos = std::cos(cycle.sun_path_azimuth_radians);
    const auto path_sin = std::sin(cycle.sun_path_azimuth_radians);

    DayNightEnvironment result;
    result.day_fraction = static_cast<float>(day_fraction);
    result.solar_elevation = elevation;
    result.daylight =
        smoothstep(-cycle.twilight_elevation_width, cycle.twilight_elevation_width, elevation);
    result.render.sun_direction = {horizontal * path_cos, elevation, horizontal * path_sin};
    result.render.sun_intensity =
        cycle.maximum_sun_intensity * std::max(elevation, 0.0F) * result.daylight;

    const auto twilight = std::exp(-std::abs(elevation) / cycle.twilight_elevation_width) *
                          smoothstep(-cycle.twilight_elevation_width, 0.02F, elevation);
    result.render.ambient_color = mix(cycle.night_ambient, cycle.day_ambient, result.daylight) +
                                  cycle.twilight_ambient * twilight;
    result.render.fog_color = mix(cycle.night_horizon, cycle.day_horizon, result.daylight) +
                              cycle.twilight_horizon * twilight;
    result.render.fog_start = cycle.fog_start;
    result.render.fog_end = cycle.fog_end;
    status = rhi::validate_render_environment(result.render);
    if (!status) {
        return core::Result<DayNightEnvironment>::failure(status.error().code,
                                                          status.error().message);
    }
    return core::Result<DayNightEnvironment>::success(result);
}

} // namespace heartstead::renderer
