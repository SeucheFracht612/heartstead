#include "engine/renderer/environment/weather_effects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>

namespace heartstead::renderer {

namespace {

[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] float random_unit(std::uint64_t value) noexcept {
    return static_cast<float>(mix(value) >> 40U) /
           static_cast<float>(0x00ff'ffffU);
}

[[nodiscard]] bool valid_rate(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 100'000.0F;
}

} // namespace

core::Status WeatherEffectsConfig::validate() const noexcept {
    if (!valid_rate(rain_particles_per_second) ||
        !valid_rate(snow_particles_per_second) ||
        !valid_rate(ash_particles_per_second) ||
        !valid_rate(spore_particles_per_second) ||
        !std::isfinite(horizontal_radius) || horizontal_radius <= 0.0F ||
        horizontal_radius > 1'024.0F || !std::isfinite(minimum_spawn_height) ||
        !std::isfinite(maximum_spawn_height) || minimum_spawn_height < 0.0F ||
        maximum_spawn_height <= minimum_spawn_height ||
        maximum_spawn_height > 1'024.0F || maximum_spawns_per_update == 0U ||
        maximum_spawns_per_update > 16'384U || deterministic_seed == 0U) {
        return core::Status::failure(
            "weather_effects.invalid_config",
            "weather effects require finite rates, a bounded spawn volume and a non-zero seed");
    }
    return core::Status::ok();
}

core::Status WeatherEffects::initialize(CpuParticleSystem& particles,
                                        WeatherEffectsConfig config) {
    if (is_initialized()) {
        return core::Status::failure("weather_effects.already_initialized",
                                     "weather effects are already initialized");
    }
    auto status = config.validate();
    if (!status) {
        return status;
    }
    constexpr std::array ids{
        "base:particles/rain_drop",
        "base:particles/snow_flake",
        "base:particles/dust_mote",
        "base:particles/spore",
    };
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto parsed = core::PrototypeId::parse(ids[index]);
        if (!parsed || particles.find_prototype(*parsed) == nullptr) {
            return core::Status::failure(
                "weather_effects.missing_prototype",
                std::string("weather particle prototype is missing: ") + ids[index]);
        }
        prototypes_[index] = *parsed;
    }
    particles_ = &particles;
    config_ = config;
    return core::Status::ok();
}

const core::PrototypeId*
WeatherEffects::prototype_for(EnvironmentPrecipitation precipitation) const noexcept {
    switch (precipitation) {
    case EnvironmentPrecipitation::rain:
        return &prototypes_[0];
    case EnvironmentPrecipitation::snow:
        return &prototypes_[1];
    case EnvironmentPrecipitation::ash:
        return &prototypes_[2];
    case EnvironmentPrecipitation::spores:
        return &prototypes_[3];
    case EnvironmentPrecipitation::none:
        return nullptr;
    }
    return nullptr;
}

float WeatherEffects::rate_for(EnvironmentPrecipitation precipitation) const noexcept {
    switch (precipitation) {
    case EnvironmentPrecipitation::rain:
        return config_.rain_particles_per_second;
    case EnvironmentPrecipitation::snow:
        return config_.snow_particles_per_second;
    case EnvironmentPrecipitation::ash:
        return config_.ash_particles_per_second;
    case EnvironmentPrecipitation::spores:
        return config_.spore_particles_per_second;
    case EnvironmentPrecipitation::none:
        return 0.0F;
    }
    return 0.0F;
}

core::Status WeatherEffects::update(const EvaluatedEnvironment& environment,
                                    const world::WorldPosition& viewpoint,
                                    float delta_seconds) {
    if (!is_initialized() || !viewpoint.is_valid() || !std::isfinite(delta_seconds) ||
        delta_seconds <= 0.0F || delta_seconds > 0.25F) {
        return core::Status::failure(
            "weather_effects.invalid_update",
            "weather effects update requires initialization, a valid viewpoint and delta");
    }
    stats_.precipitation = environment.weather.precipitation;
    stats_.intensity =
        std::clamp(environment.weather.precipitation_intensity, 0.0F, 1.0F);
    stats_.emitted_this_update = 0;
    particles_->set_environment(environment.wind.velocity(), viewpoint);

    const auto* prototype = prototype_for(stats_.precipitation);
    if (prototype == nullptr || stats_.intensity <= 0.0001F) {
        spawn_accumulator_ = 0.0F;
        return core::Status::ok();
    }
    spawn_accumulator_ +=
        rate_for(stats_.precipitation) * stats_.intensity * delta_seconds;
    const auto requested = static_cast<std::uint32_t>(
        std::min(std::floor(static_cast<double>(spawn_accumulator_)),
                 static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
    const auto emitted = std::min(requested, config_.maximum_spawns_per_update);
    spawn_accumulator_ -= static_cast<float>(requested);
    if (requested > emitted) {
        stats_.budget_limited_particles += requested - emitted;
    }

    for (std::uint32_t index = 0; index < emitted; ++index) {
        const auto serial = ++emission_serial_;
        const auto radius =
            std::sqrt(random_unit(config_.deterministic_seed ^ serial)) *
            config_.horizontal_radius;
        const auto angle =
            random_unit(config_.deterministic_seed ^ mix(serial)) *
            2.0F * std::numbers::pi_v<float>;
        const auto height =
            config_.minimum_spawn_height +
            random_unit(config_.deterministic_seed ^ mix(serial + 1U)) *
                (config_.maximum_spawn_height - config_.minimum_spawn_height);
        auto position = world::WorldPosition::from_anchor(
            viewpoint.anchor,
            viewpoint.local_offset +
                math::Vec3d{static_cast<double>(std::cos(angle) * radius),
                            static_cast<double>(height),
                            static_cast<double>(std::sin(angle) * radius)});
        if (!position) {
            return core::Status::failure(position.error().code,
                                         position.error().message);
        }
        const auto wind = environment.wind.velocity();
        math::Vec3f direction{wind.x * 0.025F, -1.0F, wind.z * 0.025F};
        if (stats_.precipitation == EnvironmentPrecipitation::ash ||
            stats_.precipitation == EnvironmentPrecipitation::spores) {
            direction = {wind.x * 0.15F, 0.3F, wind.z * 0.15F};
        }
        ParticleEmitEvent event;
        event.prototype_id = *prototype;
        event.position = position.value();
        event.direction = direction;
        event.inherited_velocity = wind * 0.18F;
        event.count = 1;
        event.seed = mix(config_.deterministic_seed ^ serial);
        if (event.seed == 0U) {
            event.seed = 1U;
        }
        auto status = particles_->queue_event(std::move(event));
        if (!status) {
            return status;
        }
    }
    stats_.emitted_this_update = emitted;
    stats_.emitted_total += emitted;
    return core::Status::ok();
}

void WeatherEffects::reset() noexcept {
    particles_ = nullptr;
    config_ = {};
    prototypes_ = {};
    spawn_accumulator_ = 0.0F;
    emission_serial_ = 0;
    stats_ = {};
}

bool WeatherEffects::is_initialized() const noexcept {
    return particles_ != nullptr;
}

const WeatherEffectsStats& WeatherEffects::stats() const noexcept {
    return stats_;
}

} // namespace heartstead::renderer
