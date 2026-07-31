#pragma once

#include "engine/core/result.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer {

enum class EnvironmentPrecipitation : std::uint8_t {
    none,
    rain,
    snow,
    ash,
    spores,
};

[[nodiscard]] std::string_view
environment_precipitation_name(EnvironmentPrecipitation precipitation) noexcept;
[[nodiscard]] std::optional<EnvironmentPrecipitation>
parse_environment_precipitation(std::string_view name) noexcept;

struct EnvironmentProfileSelector {
    std::vector<std::string> biomes;
    std::vector<std::string> weather;
    float time_start = 0.0F;
    float time_end = 1.0F;
    float time_fade = 0.0F;
    float altitude_min = -1'000'000.0F;
    float altitude_max = 1'000'000.0F;
    float altitude_fade = 0.0F;
    std::optional<bool> underground;
    std::optional<bool> aerial;
    std::optional<bool> ocean;
    std::optional<bool> underwater;
    std::int32_t priority = 0;

    [[nodiscard]] core::Status validate() const;
};

struct EnvironmentAtmosphere {
    math::Vec3f sun_color{1.0F, 0.96F, 0.88F};
    math::Vec3f moon_color{0.38F, 0.46F, 0.68F};
    float moon_intensity = 0.0F;
    float star_intensity = 0.0F;
    math::Vec3f sky_zenith{0.12F, 0.30F, 0.58F};
    math::Vec3f sky_horizon{0.38F, 0.56F, 0.72F};
    float rayleigh_strength = 1.0F;
    float mie_strength = 0.08F;
    float aerial_perspective = 0.0F;
    float height_fog_density = 0.0F;
    float height_fog_falloff = 0.1F;
    float local_fog_density = 0.0F;
    float underground_fog_density = 0.0F;
    float underwater_fog_density = 0.08F;
};

struct EnvironmentWind {
    math::Vec3f direction{1.0F, 0.0F, 0.0F};
    float speed = 0.0F;
    float gust_strength = 0.0F;
    float gust_frequency = 0.2F;
    float turbulence = 0.0F;

    [[nodiscard]] math::Vec3f velocity() const noexcept;
};

struct EnvironmentWeather {
    EnvironmentPrecipitation precipitation = EnvironmentPrecipitation::none;
    float precipitation_intensity = 0.0F;
    float cloud_coverage = 0.0F;
    float cloud_density = 0.0F;
    float storm_intensity = 0.0F;
    float visibility = 1.0F;
    float wetness = 0.0F;
    float snow = 0.0F;
};

struct EnvironmentWater {
    math::Vec3f shallow_color{0.16F, 0.46F, 0.58F};
    math::Vec3f deep_color{0.015F, 0.10F, 0.18F};
    math::Vec3f scattering_color{0.08F, 0.34F, 0.42F};
    math::Vec3f foam_color{0.86F, 0.94F, 0.92F};
    float absorption_distance = 8.0F;
    float scattering_strength = 0.25F;
    float refraction_strength = 0.025F;
    float normal_strength = 0.45F;
    float normal_speed = 0.08F;
    float fresnel_f0 = 0.02F;
    float foam_strength = 0.5F;
    float shoreline_foam_distance = 0.3F;
    float ripple_strength = 0.2F;
    float large_water_lod_distance = 256.0F;
    float underwater_fog_distance = 24.0F;
    math::Vec3f underwater_grade{0.55F, 0.82F, 0.88F};
};

struct EnvironmentColorGrade {
    math::Vec3f lift{};
    math::Vec3f gain{1.0F, 1.0F, 1.0F};
    float saturation = 1.0F;
    float contrast = 1.0F;
};

struct EnvironmentProfile {
    core::PrototypeId id;
    std::string display_name;
    EnvironmentProfileSelector selector;
    rhi::RenderEnvironmentData render;
    rhi::RenderExposureSettings exposure;
    EnvironmentAtmosphere atmosphere;
    EnvironmentWind wind;
    EnvironmentWeather weather;
    EnvironmentWater water;
    EnvironmentColorGrade color_grade;
    std::vector<std::string> hazard_visuals;

    [[nodiscard]] core::Status validate() const;
};

struct LocalEnvironmentContribution {
    core::PrototypeId profile;
    float weight = 0.0F;
};

struct EnvironmentBlendContext {
    std::string biome;
    std::string weather;
    float day_fraction = 0.5F;
    float elapsed_seconds = 0.0F;
    float altitude = 0.0F;
    bool underground = false;
    bool aerial = false;
    bool ocean = false;
    bool underwater = false;
    std::vector<LocalEnvironmentContribution> local_volumes;
};

struct EnvironmentBlendContribution {
    core::PrototypeId profile;
    float normalized_weight = 0.0F;

    friend bool operator==(const EnvironmentBlendContribution&,
                           const EnvironmentBlendContribution&) = default;
};

struct EvaluatedEnvironment {
    rhi::RenderEnvironmentData render;
    rhi::RenderExposureSettings exposure;
    EnvironmentAtmosphere atmosphere;
    EnvironmentWind wind;
    EnvironmentWeather weather;
    EnvironmentWater water;
    EnvironmentColorGrade color_grade;
    std::vector<std::string> hazard_visuals;
    std::vector<EnvironmentBlendContribution> contributions;
    bool underwater = false;
};

class EnvironmentProfileRegistry {
  public:
    [[nodiscard]] core::Status add(EnvironmentProfile profile);
    [[nodiscard]] const EnvironmentProfile*
    find(const core::PrototypeId& id) const noexcept;
    [[nodiscard]] std::span<const EnvironmentProfile> profiles() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] core::Result<EvaluatedEnvironment>
    evaluate(const EnvironmentBlendContext& context) const;

  private:
    std::vector<EnvironmentProfile> profiles_;
};

[[nodiscard]] core::Result<EnvironmentProfile>
environment_profile_from_generic(const modding::GenericPrototype& prototype);
[[nodiscard]] core::Result<EnvironmentProfileRegistry>
environment_profile_registry_from_prototypes(const modding::PrototypeRegistry& prototypes);

} // namespace heartstead::renderer
