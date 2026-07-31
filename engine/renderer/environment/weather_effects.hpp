#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/environment/environment_profile.hpp"
#include "engine/renderer/particles/particle_system.hpp"
#include "engine/world/coords/world_position.hpp"

#include <array>
#include <cstdint>

namespace heartstead::renderer {

struct WeatherEffectsConfig {
    float rain_particles_per_second = 900.0F;
    float snow_particles_per_second = 420.0F;
    float ash_particles_per_second = 180.0F;
    float spore_particles_per_second = 120.0F;
    float horizontal_radius = 18.0F;
    float minimum_spawn_height = 8.0F;
    float maximum_spawn_height = 20.0F;
    std::uint32_t maximum_spawns_per_update = 1'024;
    std::uint64_t deterministic_seed = 0x4853'5745'4154'4845ULL;

    [[nodiscard]] core::Status validate() const noexcept;
};

struct WeatherEffectsStats {
    EnvironmentPrecipitation precipitation = EnvironmentPrecipitation::none;
    float intensity = 0.0F;
    std::uint32_t emitted_this_update = 0;
    std::uint64_t emitted_total = 0;
    std::uint64_t budget_limited_particles = 0;
};

class WeatherEffects {
  public:
    [[nodiscard]] core::Status initialize(CpuParticleSystem& particles,
                                          WeatherEffectsConfig config = {});
    [[nodiscard]] core::Status update(const EvaluatedEnvironment& environment,
                                      const world::WorldPosition& viewpoint,
                                      float delta_seconds);
    void reset() noexcept;

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] const WeatherEffectsStats& stats() const noexcept;

  private:
    [[nodiscard]] const core::PrototypeId*
    prototype_for(EnvironmentPrecipitation precipitation) const noexcept;
    [[nodiscard]] float rate_for(EnvironmentPrecipitation precipitation) const noexcept;

    CpuParticleSystem* particles_ = nullptr;
    WeatherEffectsConfig config_{};
    std::array<core::PrototypeId, 4> prototypes_{};
    float spawn_accumulator_ = 0.0F;
    std::uint64_t emission_serial_ = 0;
    WeatherEffectsStats stats_{};
};

} // namespace heartstead::renderer
