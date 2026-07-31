#include "engine/renderer/environment/environment_profile.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>

namespace heartstead::renderer {

namespace {

[[nodiscard]] const std::string* field(const modding::GenericPrototype& prototype,
                                       std::string_view key) {
    const auto found = prototype.fields.find(std::string(key));
    return found == prototype.fields.end() ? nullptr : &found->second;
}

template <typename Value>
[[nodiscard]] core::Result<Value> number_field(const modding::GenericPrototype& prototype,
                                               std::string_view key, Value fallback) {
    const auto* text = field(prototype, key);
    if (text == nullptr) {
        return core::Result<Value>::success(fallback);
    }
    Value value{};
    const auto [end, error] =
        std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc{} || end != text->data() + text->size() ||
        (std::is_floating_point_v<Value> && !std::isfinite(value))) {
        return core::Result<Value>::failure("environment_profile.invalid_number",
                                            std::string(key) + " must be a finite number");
    }
    return core::Result<Value>::success(value);
}

[[nodiscard]] core::Result<bool> bool_field(const modding::GenericPrototype& prototype,
                                            std::string_view key, bool fallback) {
    const auto* text = field(prototype, key);
    if (text == nullptr) {
        return core::Result<bool>::success(fallback);
    }
    if (*text == "true") {
        return core::Result<bool>::success(true);
    }
    if (*text == "false") {
        return core::Result<bool>::success(false);
    }
    return core::Result<bool>::failure("environment_profile.invalid_bool",
                                       std::string(key) + " must be true or false");
}

[[nodiscard]] core::Result<math::Vec3f>
vector_field(const modding::GenericPrototype& prototype, std::string_view key,
             math::Vec3f fallback) {
    const auto* text = field(prototype, key);
    if (text == nullptr) {
        return core::Result<math::Vec3f>::success(fallback);
    }
    math::Vec3f result;
    std::string_view remaining = *text;
    for (std::size_t index = 0; index < 3; ++index) {
        const auto separator = remaining.find(',');
        const auto token = remaining.substr(0, separator);
        float value = 0.0F;
        const auto [end, error] =
            std::from_chars(token.data(), token.data() + token.size(), value);
        if (error != std::errc{} || end != token.data() + token.size() ||
            !std::isfinite(value) ||
            ((separator == std::string_view::npos) != (index == 2U))) {
            return core::Result<math::Vec3f>::failure(
                "environment_profile.invalid_vector",
                std::string(key) + " must contain three comma-separated finite values");
        }
        (&result.x)[index] = value;
        if (separator != std::string_view::npos) {
            remaining.remove_prefix(separator + 1U);
        }
    }
    return core::Result<math::Vec3f>::success(result);
}

[[nodiscard]] std::vector<std::string> list_field(const modding::GenericPrototype& prototype,
                                                  std::string_view key) {
    std::vector<std::string> result;
    const auto* text = field(prototype, key);
    if (text == nullptr || text->empty()) {
        return result;
    }
    std::string_view remaining = *text;
    while (!remaining.empty()) {
        const auto separator = remaining.find(',');
        const auto token = remaining.substr(0, separator);
        if (!token.empty()) {
            result.emplace_back(token);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1U);
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

[[nodiscard]] bool nonnegative(math::Vec3f value) noexcept {
    return value.is_finite() && value.x >= 0.0F && value.y >= 0.0F && value.z >= 0.0F;
}

[[nodiscard]] bool unit_range(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] float smoothstep(float value) noexcept {
    const auto clamped = std::clamp(value, 0.0F, 1.0F);
    return clamped * clamped * (3.0F - 2.0F * clamped);
}

[[nodiscard]] float wrapped_distance(float first, float second) noexcept {
    const auto direct = std::abs(first - second);
    return std::min(direct, 1.0F - direct);
}

[[nodiscard]] float time_weight(const EnvironmentProfileSelector& selector,
                                float day_fraction) noexcept {
    if (selector.time_start == 0.0F && selector.time_end == 1.0F) {
        return 1.0F;
    }
    const auto time = day_fraction - std::floor(day_fraction);
    const auto inside = selector.time_start <= selector.time_end
                            ? time >= selector.time_start && time <= selector.time_end
                            : time >= selector.time_start || time <= selector.time_end;
    if (inside) {
        return 1.0F;
    }
    if (selector.time_fade <= 0.0F) {
        return 0.0F;
    }
    const auto distance =
        std::min(wrapped_distance(time, selector.time_start),
                 wrapped_distance(time, selector.time_end));
    return smoothstep(1.0F - distance / selector.time_fade);
}

[[nodiscard]] float altitude_weight(const EnvironmentProfileSelector& selector,
                                    float altitude) noexcept {
    if (altitude >= selector.altitude_min && altitude <= selector.altitude_max) {
        return 1.0F;
    }
    if (selector.altitude_fade <= 0.0F) {
        return 0.0F;
    }
    const auto distance = altitude < selector.altitude_min
                              ? selector.altitude_min - altitude
                              : altitude - selector.altitude_max;
    return smoothstep(1.0F - distance / selector.altitude_fade);
}

[[nodiscard]] bool contains(const std::vector<std::string>& values,
                            std::string_view value) noexcept {
    return values.empty() || std::ranges::binary_search(values, value);
}

[[nodiscard]] float selector_weight(const EnvironmentProfileSelector& selector,
                                    const EnvironmentBlendContext& context) noexcept {
    if (!contains(selector.biomes, context.biome) ||
        !contains(selector.weather, context.weather) ||
        (selector.underground.has_value() &&
         *selector.underground != context.underground) ||
        (selector.aerial.has_value() && *selector.aerial != context.aerial) ||
        (selector.ocean.has_value() && *selector.ocean != context.ocean) ||
        (selector.underwater.has_value() &&
         *selector.underwater != context.underwater)) {
        return 0.0F;
    }
    const auto weight =
        time_weight(selector, context.day_fraction) * altitude_weight(selector, context.altitude);
    const auto priority_scale =
        std::exp2(static_cast<float>(std::clamp(selector.priority, -8, 8)));
    return weight * priority_scale;
}

[[nodiscard]] float mix(float left, float right, float amount) noexcept {
    return left + (right - left) * amount;
}

[[nodiscard]] math::Vec3f mix(math::Vec3f left, math::Vec3f right, float amount) noexcept {
    return left + (right - left) * amount;
}

[[nodiscard]] math::Vec3f multiply(math::Vec3f left, math::Vec3f right) noexcept {
    return {left.x * right.x, left.y * right.y, left.z * right.z};
}

void blend_into(EvaluatedEnvironment& target, const EnvironmentProfile& source,
                float amount) {
    target.render.sun_direction =
        mix(target.render.sun_direction, source.render.sun_direction, amount);
    target.render.sun_intensity =
        mix(target.render.sun_intensity, source.render.sun_intensity, amount);
    target.render.ambient_color =
        mix(target.render.ambient_color, source.render.ambient_color, amount);
    target.render.fog_start = mix(target.render.fog_start, source.render.fog_start, amount);
    target.render.fog_color = mix(target.render.fog_color, source.render.fog_color, amount);
    target.render.fog_end = mix(target.render.fog_end, source.render.fog_end, amount);
    target.render.sky_diffuse_intensity =
        mix(target.render.sky_diffuse_intensity, source.render.sky_diffuse_intensity, amount);
    target.render.environment_specular_intensity =
        mix(target.render.environment_specular_intensity,
            source.render.environment_specular_intensity, amount);
    target.render.environment_rotation_radians =
        mix(target.render.environment_rotation_radians,
            source.render.environment_rotation_radians, amount);

    target.exposure.exposure_stops =
        mix(target.exposure.exposure_stops, source.exposure.exposure_stops, amount);
    target.exposure.white_point =
        mix(target.exposure.white_point, source.exposure.white_point, amount);
    target.exposure.saturation =
        mix(target.exposure.saturation, source.exposure.saturation, amount);
    target.exposure.contrast = mix(target.exposure.contrast, source.exposure.contrast, amount);
    target.exposure.bloom_intensity =
        mix(target.exposure.bloom_intensity, source.exposure.bloom_intensity, amount);
    target.exposure.target_luminance =
        mix(target.exposure.target_luminance, source.exposure.target_luminance, amount);
    target.exposure.adaptation_speed =
        mix(target.exposure.adaptation_speed, source.exposure.adaptation_speed, amount);
    target.exposure.minimum_auto_stops =
        mix(target.exposure.minimum_auto_stops, source.exposure.minimum_auto_stops, amount);
    target.exposure.maximum_auto_stops =
        mix(target.exposure.maximum_auto_stops, source.exposure.maximum_auto_stops, amount);

    auto& atmosphere = target.atmosphere;
    atmosphere.sun_color = mix(atmosphere.sun_color, source.atmosphere.sun_color, amount);
    atmosphere.moon_color = mix(atmosphere.moon_color, source.atmosphere.moon_color, amount);
    atmosphere.moon_intensity =
        mix(atmosphere.moon_intensity, source.atmosphere.moon_intensity, amount);
    atmosphere.star_intensity =
        mix(atmosphere.star_intensity, source.atmosphere.star_intensity, amount);
    atmosphere.sky_zenith = mix(atmosphere.sky_zenith, source.atmosphere.sky_zenith, amount);
    atmosphere.sky_horizon = mix(atmosphere.sky_horizon, source.atmosphere.sky_horizon, amount);
    atmosphere.rayleigh_strength =
        mix(atmosphere.rayleigh_strength, source.atmosphere.rayleigh_strength, amount);
    atmosphere.mie_strength =
        mix(atmosphere.mie_strength, source.atmosphere.mie_strength, amount);
    atmosphere.aerial_perspective =
        mix(atmosphere.aerial_perspective, source.atmosphere.aerial_perspective, amount);
    atmosphere.height_fog_density =
        mix(atmosphere.height_fog_density, source.atmosphere.height_fog_density, amount);
    atmosphere.height_fog_falloff =
        mix(atmosphere.height_fog_falloff, source.atmosphere.height_fog_falloff, amount);
    atmosphere.local_fog_density =
        mix(atmosphere.local_fog_density, source.atmosphere.local_fog_density, amount);
    atmosphere.underground_fog_density =
        mix(atmosphere.underground_fog_density,
            source.atmosphere.underground_fog_density, amount);
    atmosphere.underwater_fog_density =
        mix(atmosphere.underwater_fog_density,
            source.atmosphere.underwater_fog_density, amount);

    target.wind.direction = mix(target.wind.direction, source.wind.direction, amount);
    target.wind.speed = mix(target.wind.speed, source.wind.speed, amount);
    target.wind.gust_strength =
        mix(target.wind.gust_strength, source.wind.gust_strength, amount);
    target.wind.gust_frequency =
        mix(target.wind.gust_frequency, source.wind.gust_frequency, amount);
    target.wind.turbulence = mix(target.wind.turbulence, source.wind.turbulence, amount);

    target.weather.precipitation_intensity =
        mix(target.weather.precipitation_intensity,
            source.weather.precipitation_intensity, amount);
    target.weather.cloud_coverage =
        mix(target.weather.cloud_coverage, source.weather.cloud_coverage, amount);
    target.weather.cloud_density =
        mix(target.weather.cloud_density, source.weather.cloud_density, amount);
    target.weather.storm_intensity =
        mix(target.weather.storm_intensity, source.weather.storm_intensity, amount);
    target.weather.visibility =
        mix(target.weather.visibility, source.weather.visibility, amount);
    target.weather.wetness = mix(target.weather.wetness, source.weather.wetness, amount);
    target.weather.snow = mix(target.weather.snow, source.weather.snow, amount);

    auto& water = target.water;
    water.shallow_color = mix(water.shallow_color, source.water.shallow_color, amount);
    water.deep_color = mix(water.deep_color, source.water.deep_color, amount);
    water.scattering_color =
        mix(water.scattering_color, source.water.scattering_color, amount);
    water.foam_color = mix(water.foam_color, source.water.foam_color, amount);
    water.absorption_distance =
        mix(water.absorption_distance, source.water.absorption_distance, amount);
    water.scattering_strength =
        mix(water.scattering_strength, source.water.scattering_strength, amount);
    water.refraction_strength =
        mix(water.refraction_strength, source.water.refraction_strength, amount);
    water.normal_strength = mix(water.normal_strength, source.water.normal_strength, amount);
    water.normal_speed = mix(water.normal_speed, source.water.normal_speed, amount);
    water.fresnel_f0 = mix(water.fresnel_f0, source.water.fresnel_f0, amount);
    water.foam_strength = mix(water.foam_strength, source.water.foam_strength, amount);
    water.shoreline_foam_distance =
        mix(water.shoreline_foam_distance, source.water.shoreline_foam_distance, amount);
    water.ripple_strength =
        mix(water.ripple_strength, source.water.ripple_strength, amount);
    water.large_water_lod_distance =
        mix(water.large_water_lod_distance, source.water.large_water_lod_distance, amount);
    water.underwater_fog_distance =
        mix(water.underwater_fog_distance, source.water.underwater_fog_distance, amount);
    water.underwater_grade =
        mix(water.underwater_grade, source.water.underwater_grade, amount);

    target.color_grade.lift =
        mix(target.color_grade.lift, source.color_grade.lift, amount);
    target.color_grade.gain =
        mix(target.color_grade.gain, source.color_grade.gain, amount);
    target.color_grade.saturation =
        mix(target.color_grade.saturation, source.color_grade.saturation, amount);
    target.color_grade.contrast =
        mix(target.color_grade.contrast, source.color_grade.contrast, amount);
}

template <typename Value>
[[nodiscard]] core::Status assign_number(Value& destination,
                                         core::Result<Value> parsed) {
    if (!parsed) {
        return core::Status::failure(parsed.error().code, parsed.error().message);
    }
    destination = parsed.value();
    return core::Status::ok();
}

[[nodiscard]] core::Status assign_vector(math::Vec3f& destination,
                                         core::Result<math::Vec3f> parsed) {
    if (!parsed) {
        return core::Status::failure(parsed.error().code, parsed.error().message);
    }
    destination = parsed.value();
    return core::Status::ok();
}

} // namespace

std::string_view
environment_precipitation_name(EnvironmentPrecipitation precipitation) noexcept {
    switch (precipitation) {
    case EnvironmentPrecipitation::none:
        return "none";
    case EnvironmentPrecipitation::rain:
        return "rain";
    case EnvironmentPrecipitation::snow:
        return "snow";
    case EnvironmentPrecipitation::ash:
        return "ash";
    case EnvironmentPrecipitation::spores:
        return "spores";
    }
    return "unknown";
}

std::optional<EnvironmentPrecipitation>
parse_environment_precipitation(std::string_view name) noexcept {
    if (name == "none") {
        return EnvironmentPrecipitation::none;
    }
    if (name == "rain") {
        return EnvironmentPrecipitation::rain;
    }
    if (name == "snow") {
        return EnvironmentPrecipitation::snow;
    }
    if (name == "ash") {
        return EnvironmentPrecipitation::ash;
    }
    if (name == "spores") {
        return EnvironmentPrecipitation::spores;
    }
    return std::nullopt;
}

core::Status EnvironmentProfileSelector::validate() const {
    if (!unit_range(time_start) || !unit_range(time_end) || !unit_range(time_fade) ||
        !std::isfinite(altitude_min) || !std::isfinite(altitude_max) ||
        altitude_max < altitude_min || !std::isfinite(altitude_fade) ||
        altitude_fade < 0.0F || priority < -32 || priority > 32 ||
        std::ranges::any_of(biomes, [](const auto& value) { return value.empty(); }) ||
        std::ranges::any_of(weather, [](const auto& value) { return value.empty(); })) {
        return core::Status::failure(
            "environment_profile.invalid_selector",
            "environment selector ranges, names, fades, and priority must be valid");
    }
    return core::Status::ok();
}

math::Vec3f EnvironmentWind::velocity() const noexcept {
    const auto length_squared = math::length_squared(direction);
    return length_squared > 1.0e-8F
               ? direction / std::sqrt(length_squared) * speed
               : math::Vec3f{};
}

core::Status EnvironmentProfile::validate() const {
    auto status = selector.validate();
    if (!status) {
        return status;
    }
    status = rhi::validate_render_environment(render);
    if (!status) {
        return status;
    }
    status = rhi::validate_render_exposure(exposure);
    if (!status) {
        return status;
    }
    const auto finite_nonnegative = [](float value) {
        return std::isfinite(value) && value >= 0.0F;
    };
    if (!id.is_valid() || display_name.empty() || !nonnegative(atmosphere.sun_color) ||
        !nonnegative(atmosphere.moon_color) || !nonnegative(atmosphere.sky_zenith) ||
        !nonnegative(atmosphere.sky_horizon) ||
        !finite_nonnegative(atmosphere.moon_intensity) ||
        !finite_nonnegative(atmosphere.star_intensity) ||
        !finite_nonnegative(atmosphere.rayleigh_strength) ||
        !finite_nonnegative(atmosphere.mie_strength) ||
        !finite_nonnegative(atmosphere.aerial_perspective) ||
        !finite_nonnegative(atmosphere.height_fog_density) ||
        !finite_nonnegative(atmosphere.height_fog_falloff) ||
        !finite_nonnegative(atmosphere.local_fog_density) ||
        !finite_nonnegative(atmosphere.underground_fog_density) ||
        !finite_nonnegative(atmosphere.underwater_fog_density) ||
        !wind.direction.is_finite() || !finite_nonnegative(wind.speed) ||
        !finite_nonnegative(wind.gust_strength) ||
        !finite_nonnegative(wind.gust_frequency) ||
        !finite_nonnegative(wind.turbulence) ||
        !unit_range(weather.precipitation_intensity) ||
        !unit_range(weather.cloud_coverage) || !unit_range(weather.cloud_density) ||
        !unit_range(weather.storm_intensity) || !unit_range(weather.visibility) ||
        !unit_range(weather.wetness) || !unit_range(weather.snow) ||
        !nonnegative(water.shallow_color) || !nonnegative(water.deep_color) ||
        !nonnegative(water.scattering_color) || !nonnegative(water.foam_color) ||
        !finite_nonnegative(water.absorption_distance) ||
        water.absorption_distance <= 0.0F ||
        !finite_nonnegative(water.scattering_strength) ||
        !finite_nonnegative(water.refraction_strength) ||
        !finite_nonnegative(water.normal_strength) ||
        !finite_nonnegative(water.normal_speed) || !unit_range(water.fresnel_f0) ||
        !finite_nonnegative(water.foam_strength) ||
        !finite_nonnegative(water.shoreline_foam_distance) ||
        !finite_nonnegative(water.ripple_strength) ||
        !finite_nonnegative(water.large_water_lod_distance) ||
        !finite_nonnegative(water.underwater_fog_distance) ||
        !nonnegative(water.underwater_grade) || !color_grade.lift.is_finite() ||
        !nonnegative(color_grade.gain) || !finite_nonnegative(color_grade.saturation) ||
        !finite_nonnegative(color_grade.contrast) ||
        std::ranges::any_of(hazard_visuals,
                            [](const auto& value) { return value.empty(); })) {
        return core::Status::failure(
            "environment_profile.invalid_values",
            "environment atmosphere, weather, water, wind, exposure, and grading values "
            "must be finite and within their documented ranges");
    }
    return core::Status::ok();
}

core::Status EnvironmentProfileRegistry::add(EnvironmentProfile profile) {
    auto status = profile.validate();
    if (!status) {
        return status;
    }
    if (find(profile.id) != nullptr) {
        return core::Status::failure("environment_profile.duplicate",
                                     "duplicate environment profile id: " +
                                         profile.id.value());
    }
    profiles_.push_back(std::move(profile));
    std::ranges::sort(profiles_,
                      [](const auto& left, const auto& right) {
                          return left.id.value() < right.id.value();
                      });
    return core::Status::ok();
}

const EnvironmentProfile*
EnvironmentProfileRegistry::find(const core::PrototypeId& id) const noexcept {
    const auto found =
        std::ranges::lower_bound(profiles_, id.value(), {}, [](const auto& profile) {
            return profile.id.value();
        });
    return found == profiles_.end() || found->id != id ? nullptr : &*found;
}

std::span<const EnvironmentProfile> EnvironmentProfileRegistry::profiles() const noexcept {
    return profiles_;
}

std::size_t EnvironmentProfileRegistry::size() const noexcept {
    return profiles_.size();
}

core::Result<EvaluatedEnvironment>
EnvironmentProfileRegistry::evaluate(const EnvironmentBlendContext& context) const {
    if (!std::isfinite(context.day_fraction) ||
        !std::isfinite(context.elapsed_seconds) || context.elapsed_seconds < 0.0F ||
        !std::isfinite(context.altitude)) {
        return core::Result<EvaluatedEnvironment>::failure(
            "environment_profile.invalid_context",
            "environment blend time, elapsed seconds, and altitude must be finite and valid");
    }
    struct Weighted {
        const EnvironmentProfile* profile = nullptr;
        float weight = 0.0F;
    };
    std::vector<Weighted> weighted;
    weighted.reserve(profiles_.size() + context.local_volumes.size());
    std::unordered_map<std::string, std::size_t> by_id;
    for (const auto& profile : profiles_) {
        const auto weight = selector_weight(profile.selector, context);
        if (weight > 0.0F) {
            by_id.emplace(profile.id.value(), weighted.size());
            weighted.push_back({&profile, weight});
        }
    }
    for (const auto& volume : context.local_volumes) {
        if (!std::isfinite(volume.weight) || volume.weight < 0.0F || volume.weight > 1.0F) {
            return core::Result<EvaluatedEnvironment>::failure(
                "environment_profile.invalid_volume_weight",
                "local environment volume weights must be in 0..1");
        }
        const auto* profile = find(volume.profile);
        if (profile == nullptr) {
            return core::Result<EvaluatedEnvironment>::failure(
                "environment_profile.missing_volume_profile",
                "local environment volume references a missing profile: " +
                    volume.profile.value());
        }
        if (const auto existing = by_id.find(profile->id.value()); existing != by_id.end()) {
            weighted[existing->second].weight += volume.weight * 16.0F;
        } else if (volume.weight > 0.0F) {
            by_id.emplace(profile->id.value(), weighted.size());
            weighted.push_back({profile, volume.weight * 16.0F});
        }
    }
    if (weighted.empty()) {
        return core::Result<EvaluatedEnvironment>::failure(
            "environment_profile.no_match",
            "no environment profile matches the supplied blend context");
    }
    float total_weight = 0.0F;
    for (const auto& value : weighted) {
        total_weight += value.weight;
    }
    if (!std::isfinite(total_weight) || total_weight <= 0.0F) {
        return core::Result<EvaluatedEnvironment>::failure(
            "environment_profile.invalid_weight",
            "environment profile blend produced an invalid total weight");
    }
    std::ranges::sort(weighted, [](const auto& left, const auto& right) {
        if (left.weight != right.weight) {
            return left.weight > right.weight;
        }
        return left.profile->id.value() < right.profile->id.value();
    });

    EvaluatedEnvironment result;
    const auto* dominant = weighted.front().profile;
    result.render = dominant->render;
    result.exposure = dominant->exposure;
    result.atmosphere = dominant->atmosphere;
    result.wind = dominant->wind;
    result.weather = dominant->weather;
    result.water = dominant->water;
    result.color_grade = dominant->color_grade;
    result.hazard_visuals = dominant->hazard_visuals;
    result.underwater = context.underwater;
    result.contributions.reserve(weighted.size());

    auto accumulated = weighted.front().weight;
    result.contributions.push_back(
        {dominant->id, weighted.front().weight / total_weight});
    for (std::size_t index = 1; index < weighted.size(); ++index) {
        const auto amount = weighted[index].weight / (accumulated + weighted[index].weight);
        blend_into(result, *weighted[index].profile, amount);
        accumulated += weighted[index].weight;
        result.contributions.push_back(
            {weighted[index].profile->id, weighted[index].weight / total_weight});
    }
    const auto direction_length_squared = math::length_squared(result.render.sun_direction);
    result.render.sun_direction =
        direction_length_squared > 1.0e-8F
            ? result.render.sun_direction / std::sqrt(direction_length_squared)
            : dominant->render.sun_direction;
    const auto wind_length_squared = math::length_squared(result.wind.direction);
    result.wind.direction =
        wind_length_squared > 1.0e-8F
            ? result.wind.direction / std::sqrt(wind_length_squared)
            : dominant->wind.direction;
    result.weather.precipitation = dominant->weather.precipitation;
    result.exposure.tone_mapping = dominant->exposure.tone_mapping;
    result.exposure.automatic_exposure = dominant->exposure.automatic_exposure;
    result.exposure.saturation *= result.color_grade.saturation;
    result.exposure.contrast *= result.color_grade.contrast;
    result.render.sun_color = result.atmosphere.sun_color;
    result.render.elapsed_seconds = context.elapsed_seconds;
    result.render.sky_zenith_color = result.atmosphere.sky_zenith;
    result.render.sky_horizon_color = result.atmosphere.sky_horizon;
    result.render.cloud_coverage = result.weather.cloud_coverage;
    result.render.cloud_density = result.weather.cloud_density;
    result.render.aerial_perspective = result.atmosphere.aerial_perspective;
    result.render.wind_velocity = result.wind.velocity();
    result.render.wind_gust_strength = result.wind.gust_strength;
    result.render.wind_gust_frequency = result.wind.gust_frequency;
    result.render.wind_turbulence = result.wind.turbulence;
    result.render.precipitation_intensity =
        result.weather.precipitation_intensity;
    result.render.wetness = result.weather.wetness;
    result.render.snow = result.weather.snow;
    result.render.storm_intensity = result.weather.storm_intensity;
    result.render.visibility = result.weather.visibility;
    result.render.height_fog_density = result.atmosphere.height_fog_density;
    result.render.height_fog_falloff = result.atmosphere.height_fog_falloff;
    result.render.local_fog_density = result.atmosphere.local_fog_density;
    result.render.water_shallow_color = result.water.shallow_color;
    result.render.water_absorption_distance = result.water.absorption_distance;
    result.render.water_deep_color = result.water.deep_color;
    result.render.water_scattering_strength = result.water.scattering_strength;
    result.render.water_scattering_color = result.water.scattering_color;
    result.render.water_refraction_strength = result.water.refraction_strength;
    result.render.water_foam_color = result.water.foam_color;
    result.render.water_foam_strength = result.water.foam_strength;
    result.render.water_normal_strength = result.water.normal_strength;
    result.render.water_normal_speed = result.water.normal_speed;
    result.render.water_fresnel_f0 = result.water.fresnel_f0;
    result.render.water_ripple_strength = result.water.ripple_strength;
    result.render.underwater_fog_distance = result.water.underwater_fog_distance;
    result.render.underwater = context.underwater;
    if (context.underwater) {
        result.render.fog_color = result.water.deep_color;
        result.render.fog_start = 0.0F;
        result.render.fog_end = std::max(result.water.underwater_fog_distance, 0.1F);
        result.render.ambient_color =
            multiply(result.render.ambient_color, result.water.underwater_grade);
        result.exposure.saturation *= 0.82F;
    }
    auto status = rhi::validate_render_environment(result.render);
    if (!status) {
        return core::Result<EvaluatedEnvironment>::failure(status.error().code,
                                                            status.error().message);
    }
    status = rhi::validate_render_exposure(result.exposure);
    if (!status) {
        return core::Result<EvaluatedEnvironment>::failure(status.error().code,
                                                            status.error().message);
    }
    return core::Result<EvaluatedEnvironment>::success(std::move(result));
}

core::Result<EnvironmentProfile>
environment_profile_from_generic(const modding::GenericPrototype& prototype) {
    if (prototype.kind != modding::PrototypeKinds::environment_profile) {
        return core::Result<EnvironmentProfile>::failure(
            "environment_profile.invalid_kind",
            "environment profile prototype has the wrong kind");
    }
    EnvironmentProfile result;
    result.id = prototype.id;
    result.display_name = prototype.display_name;
    result.selector.biomes = list_field(prototype, "selector.biomes");
    result.selector.weather = list_field(prototype, "selector.weather");
    result.hazard_visuals = list_field(prototype, "hazard.visuals");

    const auto assign = [&](auto& destination, std::string_view key) -> core::Status {
        using Value = std::remove_cvref_t<decltype(destination)>;
        return assign_number(destination, number_field<Value>(prototype, key, destination));
    };
    const auto assign_vec = [&](math::Vec3f& destination,
                                std::string_view key) -> core::Status {
        return assign_vector(destination, vector_field(prototype, key, destination));
    };
    const std::array scalar_fields{
        std::pair{&result.selector.time_start, "selector.time_start"},
        std::pair{&result.selector.time_end, "selector.time_end"},
        std::pair{&result.selector.time_fade, "selector.time_fade"},
        std::pair{&result.selector.altitude_min, "selector.altitude_min"},
        std::pair{&result.selector.altitude_max, "selector.altitude_max"},
        std::pair{&result.selector.altitude_fade, "selector.altitude_fade"},
        std::pair{&result.render.sun_intensity, "sun.intensity"},
        std::pair{&result.render.fog_start, "fog.start"},
        std::pair{&result.render.fog_end, "fog.end"},
        std::pair{&result.render.sky_diffuse_intensity, "ambient.diffuse_intensity"},
        std::pair{&result.render.environment_specular_intensity,
                  "ambient.specular_intensity"},
        std::pair{&result.render.environment_rotation_radians, "ambient.rotation"},
        std::pair{&result.atmosphere.moon_intensity, "moon.intensity"},
        std::pair{&result.atmosphere.star_intensity, "stars.intensity"},
        std::pair{&result.atmosphere.rayleigh_strength, "atmosphere.rayleigh"},
        std::pair{&result.atmosphere.mie_strength, "atmosphere.mie"},
        std::pair{&result.atmosphere.aerial_perspective, "fog.aerial_perspective"},
        std::pair{&result.atmosphere.height_fog_density, "fog.height_density"},
        std::pair{&result.atmosphere.height_fog_falloff, "fog.height_falloff"},
        std::pair{&result.atmosphere.local_fog_density, "fog.local_density"},
        std::pair{&result.atmosphere.underground_fog_density, "fog.underground_density"},
        std::pair{&result.atmosphere.underwater_fog_density, "fog.underwater_density"},
        std::pair{&result.wind.speed, "wind.speed"},
        std::pair{&result.wind.gust_strength, "wind.gust_strength"},
        std::pair{&result.wind.gust_frequency, "wind.gust_frequency"},
        std::pair{&result.wind.turbulence, "wind.turbulence"},
        std::pair{&result.weather.precipitation_intensity, "weather.intensity"},
        std::pair{&result.weather.cloud_coverage, "weather.cloud_coverage"},
        std::pair{&result.weather.cloud_density, "weather.cloud_density"},
        std::pair{&result.weather.storm_intensity, "weather.storm_intensity"},
        std::pair{&result.weather.visibility, "weather.visibility"},
        std::pair{&result.weather.wetness, "weather.wetness"},
        std::pair{&result.weather.snow, "weather.snow"},
        std::pair{&result.water.absorption_distance, "water.absorption_distance"},
        std::pair{&result.water.scattering_strength, "water.scattering_strength"},
        std::pair{&result.water.refraction_strength, "water.refraction_strength"},
        std::pair{&result.water.normal_strength, "water.normal_strength"},
        std::pair{&result.water.normal_speed, "water.normal_speed"},
        std::pair{&result.water.fresnel_f0, "water.fresnel_f0"},
        std::pair{&result.water.foam_strength, "water.foam_strength"},
        std::pair{&result.water.shoreline_foam_distance, "water.shoreline_foam_distance"},
        std::pair{&result.water.ripple_strength, "water.ripple_strength"},
        std::pair{&result.water.large_water_lod_distance, "water.large_lod_distance"},
        std::pair{&result.water.underwater_fog_distance, "water.underwater_fog_distance"},
        std::pair{&result.color_grade.saturation, "grading.saturation"},
        std::pair{&result.color_grade.contrast, "grading.contrast"},
        std::pair{&result.exposure.exposure_stops, "exposure.stops"},
        std::pair{&result.exposure.white_point, "exposure.white_point"},
        std::pair{&result.exposure.saturation, "exposure.saturation"},
        std::pair{&result.exposure.contrast, "exposure.contrast"},
        std::pair{&result.exposure.bloom_intensity, "exposure.bloom"},
        std::pair{&result.exposure.target_luminance, "exposure.target_luminance"},
        std::pair{&result.exposure.adaptation_speed, "exposure.adaptation_speed"},
        std::pair{&result.exposure.minimum_auto_stops, "exposure.minimum_stops"},
        std::pair{&result.exposure.maximum_auto_stops, "exposure.maximum_stops"},
    };
    for (const auto& [destination, key] : scalar_fields) {
        auto status = assign(*destination, key);
        if (!status) {
            return core::Result<EnvironmentProfile>::failure(status.error().code,
                                                              status.error().message);
        }
    }
    auto priority = number_field<std::int32_t>(
        prototype, "selector.priority", result.selector.priority);
    if (!priority) {
        return core::Result<EnvironmentProfile>::failure(priority.error().code,
                                                          priority.error().message);
    }
    result.selector.priority = priority.value();

    const std::array vector_fields{
        std::pair{&result.render.sun_direction, "sun.direction"},
        std::pair{&result.render.ambient_color, "ambient.color"},
        std::pair{&result.render.fog_color, "fog.color"},
        std::pair{&result.atmosphere.sun_color, "sun.color"},
        std::pair{&result.atmosphere.moon_color, "moon.color"},
        std::pair{&result.atmosphere.sky_zenith, "sky.zenith_color"},
        std::pair{&result.atmosphere.sky_horizon, "sky.horizon_color"},
        std::pair{&result.wind.direction, "wind.direction"},
        std::pair{&result.water.shallow_color, "water.shallow_color"},
        std::pair{&result.water.deep_color, "water.deep_color"},
        std::pair{&result.water.scattering_color, "water.scattering_color"},
        std::pair{&result.water.foam_color, "water.foam_color"},
        std::pair{&result.water.underwater_grade, "water.underwater_grade"},
        std::pair{&result.color_grade.lift, "grading.lift"},
        std::pair{&result.color_grade.gain, "grading.gain"},
    };
    for (const auto& [destination, key] : vector_fields) {
        auto status = assign_vec(*destination, key);
        if (!status) {
            return core::Result<EnvironmentProfile>::failure(status.error().code,
                                                              status.error().message);
        }
    }
    const auto optional_selector = [&](std::string_view key,
                                       std::optional<bool>& destination) -> core::Status {
        if (field(prototype, key) == nullptr) {
            return core::Status::ok();
        }
        auto value = bool_field(prototype, key, false);
        if (!value) {
            return core::Status::failure(value.error().code, value.error().message);
        }
        destination = value.value();
        return core::Status::ok();
    };
    for (const auto [key, destination] :
         {std::pair{"selector.underground", &result.selector.underground},
          std::pair{"selector.aerial", &result.selector.aerial},
          std::pair{"selector.ocean", &result.selector.ocean},
          std::pair{"selector.underwater", &result.selector.underwater}}) {
        auto status = optional_selector(key, *destination);
        if (!status) {
            return core::Result<EnvironmentProfile>::failure(status.error().code,
                                                              status.error().message);
        }
    }
    auto automatic_exposure =
        bool_field(prototype, "exposure.automatic", result.exposure.automatic_exposure);
    if (!automatic_exposure) {
        return core::Result<EnvironmentProfile>::failure(
            automatic_exposure.error().code, automatic_exposure.error().message);
    }
    result.exposure.automatic_exposure = automatic_exposure.value();
    if (const auto* precipitation = field(prototype, "weather.precipitation");
        precipitation != nullptr) {
        const auto parsed = parse_environment_precipitation(*precipitation);
        if (!parsed.has_value()) {
            return core::Result<EnvironmentProfile>::failure(
                "environment_profile.invalid_precipitation",
                "weather.precipitation must be none, rain, snow, ash, or spores");
        }
        result.weather.precipitation = *parsed;
    }
    if (const auto* tone_mapping = field(prototype, "exposure.tone_mapping");
        tone_mapping != nullptr) {
        if (*tone_mapping == "none") {
            result.exposure.tone_mapping = rhi::RenderToneMapping::none;
        } else if (*tone_mapping == "reinhard") {
            result.exposure.tone_mapping = rhi::RenderToneMapping::reinhard;
        } else if (*tone_mapping == "aces") {
            result.exposure.tone_mapping = rhi::RenderToneMapping::aces_approx;
        } else if (*tone_mapping == "khronos-neutral") {
            result.exposure.tone_mapping = rhi::RenderToneMapping::khronos_pbr_neutral;
        } else {
            return core::Result<EnvironmentProfile>::failure(
                "environment_profile.invalid_tone_mapping",
                "exposure.tone_mapping must be none, reinhard, aces, or khronos-neutral");
        }
    }
    auto status = result.validate();
    if (!status) {
        return core::Result<EnvironmentProfile>::failure(status.error().code,
                                                          status.error().message);
    }
    return core::Result<EnvironmentProfile>::success(std::move(result));
}

core::Result<EnvironmentProfileRegistry>
environment_profile_registry_from_prototypes(const modding::PrototypeRegistry& prototypes) {
    EnvironmentProfileRegistry registry;
    for (const auto* prototype :
         prototypes.prototypes_of_kind(modding::PrototypeKinds::environment_profile)) {
        auto profile = environment_profile_from_generic(*prototype);
        if (!profile) {
            return core::Result<EnvironmentProfileRegistry>::failure(
                profile.error().code,
                prototype->source.generic_string() + ": " + profile.error().message);
        }
        auto status = registry.add(std::move(profile).value());
        if (!status) {
            return core::Result<EnvironmentProfileRegistry>::failure(
                status.error().code,
                prototype->source.generic_string() + ": " + status.error().message);
        }
    }
    return core::Result<EnvironmentProfileRegistry>::success(std::move(registry));
}

} // namespace heartstead::renderer
