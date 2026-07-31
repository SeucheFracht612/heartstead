#include "engine/renderer/environment/environment_profile.hpp"

#include <cassert>
#include <cmath>
#include <vector>

namespace {

using namespace heartstead;

renderer::EnvironmentProfile profile(std::string_view id, std::int32_t priority,
                                     math::Vec3f ambient) {
    renderer::EnvironmentProfile result;
    result.id = *core::PrototypeId::parse(id);
    result.display_name = std::string(id);
    result.selector.priority = priority;
    result.render.ambient_color = ambient;
    return result;
}

void test_selector_blending_is_deterministic() {
    renderer::EnvironmentProfileRegistry registry;
    auto baseline = profile("test:environments/baseline", -2, {0.1F, 0.2F, 0.3F});
    assert(registry.add(std::move(baseline)));
    auto meadow = profile("test:environments/meadow", 2, {0.4F, 0.5F, 0.6F});
    meadow.selector.biomes = {"meadow"};
    assert(registry.add(std::move(meadow)));
    auto rain = profile("test:environments/rain", 3, {0.05F, 0.08F, 0.1F});
    rain.selector.weather = {"rain"};
    rain.weather.precipitation = renderer::EnvironmentPrecipitation::rain;
    rain.weather.precipitation_intensity = 0.8F;
    rain.weather.wetness = 1.0F;
    assert(registry.add(std::move(rain)));

    renderer::EnvironmentBlendContext context;
    context.biome = "meadow";
    context.weather = "rain";
    const auto first = registry.evaluate(context);
    const auto second = registry.evaluate(context);
    assert(first && second);
    assert(first.value().contributions == second.value().contributions);
    assert(first.value().weather.precipitation ==
           renderer::EnvironmentPrecipitation::rain);
    assert(first.value().weather.precipitation_intensity > 0.4F);
    assert(first.value().render.ambient_color.x < 0.2F);
    assert(first.value().contributions.size() == 3);
}

void test_wrapped_time_and_altitude_fades() {
    renderer::EnvironmentProfileRegistry registry;
    auto night = profile("test:environments/night", 0, {0.02F, 0.03F, 0.06F});
    night.selector.time_start = 0.8F;
    night.selector.time_end = 0.2F;
    night.selector.time_fade = 0.1F;
    night.selector.altitude_min = 20.0F;
    night.selector.altitude_max = 100.0F;
    night.selector.altitude_fade = 10.0F;
    assert(registry.add(std::move(night)));

    renderer::EnvironmentBlendContext context;
    context.day_fraction = 0.95F;
    context.altitude = 25.0F;
    assert(registry.evaluate(context));
    context.day_fraction = 0.5F;
    assert(!registry.evaluate(context));
    context.day_fraction = 0.24F;
    context.altitude = 15.0F;
    assert(registry.evaluate(context));
}

void test_local_volume_and_underwater_override() {
    renderer::EnvironmentProfileRegistry registry;
    auto baseline = profile("test:environments/baseline", 0, {0.3F, 0.3F, 0.3F});
    assert(registry.add(std::move(baseline)));
    auto grotto = profile("test:environments/grotto", 0, {0.08F, 0.18F, 0.22F});
    grotto.water.deep_color = {0.01F, 0.04F, 0.08F};
    grotto.water.underwater_fog_distance = 12.0F;
    assert(registry.add(std::move(grotto)));

    renderer::EnvironmentBlendContext context;
    context.underwater = true;
    context.local_volumes.push_back(
        {*core::PrototypeId::parse("test:environments/grotto"), 1.0F});
    const auto evaluated = registry.evaluate(context);
    assert(evaluated);
    assert(evaluated.value().underwater);
    assert(std::abs(evaluated.value().render.fog_start) < 0.0001F);
    assert(evaluated.value().render.fog_end < 14.0F);
    assert(evaluated.value().contributions.front().profile ==
           *core::PrototypeId::parse("test:environments/grotto"));
}

void test_generic_profile_parsing() {
    modding::GenericPrototype generic;
    generic.kind = std::string(modding::PrototypeKinds::environment_profile);
    generic.id = *core::PrototypeId::parse("test:environments/authored");
    generic.display_name = "Authored";
    generic.fields = {
        {"selector.biomes", "forest,meadow"},
        {"selector.underground", "false"},
        {"sun.direction", "0.2,0.9,0.3"},
        {"wind.direction", "1.0,0.0,0.5"},
        {"wind.speed", "3.5"},
        {"weather.precipitation", "snow"},
        {"weather.intensity", "0.6"},
        {"water.foam_strength", "0.8"},
        {"exposure.automatic", "true"},
    };
    const auto parsed = renderer::environment_profile_from_generic(generic);
    assert(parsed);
    assert(parsed.value().selector.biomes.size() == 2);
    assert(parsed.value().selector.underground == false);
    assert(parsed.value().weather.precipitation ==
           renderer::EnvironmentPrecipitation::snow);
    assert(std::abs(parsed.value().wind.speed - 3.5F) < 0.0001F);
}

} // namespace

int main() {
    test_selector_blending_is_deterministic();
    test_wrapped_time_and_altitude_fades();
    test_local_volume_and_underwater_override();
    test_generic_profile_parsing();
    return 0;
}
