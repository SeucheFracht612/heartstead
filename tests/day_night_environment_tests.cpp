#include "engine/renderer/environment/day_night.hpp"

#include <cassert>
#include <cmath>

namespace {

using namespace heartstead;

bool near(float left, float right, float tolerance = 0.0001F) {
    return std::abs(left - right) <= tolerance;
}

void test_cycle_tracks_authoritative_world_time() {
    simulation::WorldTimeConfig time;
    const auto hour = time.ticks_per_hour();
    const auto day = time.ticks_per_day();
    assert(hour && day);

    const auto midnight = renderer::evaluate_day_night(0, time);
    const auto dawn = renderer::evaluate_day_night(hour.value() * 6U, time);
    const auto noon = renderer::evaluate_day_night(hour.value() * 12U, time);
    const auto dusk = renderer::evaluate_day_night(hour.value() * 18U, time);
    assert(midnight && dawn && noon && dusk);
    assert(midnight.value().solar_elevation < -0.99F);
    assert(midnight.value().render.sun_intensity == 0.0F);
    assert(dawn.value().solar_elevation > -0.0001F);
    assert(dawn.value().solar_elevation < 0.0001F);
    assert(noon.value().solar_elevation > 0.99F);
    assert(noon.value().render.sun_intensity > dawn.value().render.sun_intensity);
    assert(near(noon.value().render.ambient_color.x, 0.30F, 0.001F));
    assert(near(noon.value().render.fog_color.z, 0.70F, 0.001F));
    assert(noon.value().render.ambient_color.x > midnight.value().render.ambient_color.x);
    assert(near(midnight.value().render.ambient_color.z, 0.052F));
    assert(near(midnight.value().render.fog_color.x, 0.008F));
    assert(near(dawn.value().daylight, 0.5F));
    assert(dusk.value().render.fog_color.x > noon.value().render.fog_color.x);

    const auto wrapped = renderer::evaluate_day_night(day.value(), time);
    assert(wrapped);
    assert(near(wrapped.value().day_fraction, midnight.value().day_fraction));
    assert(near(wrapped.value().solar_elevation, midnight.value().solar_elevation));
    assert(near(wrapped.value().render.ambient_color.x, midnight.value().render.ambient_color.x));
}

void test_full_day_time_lapse_is_continuous() {
    simulation::WorldTimeConfig time;
    const auto minute = time.ticks_per_hour().value() / 60U;
    const auto day = time.ticks_per_day().value();
    auto previous = renderer::evaluate_day_night(0, time);
    assert(previous);
    for (simulation::WorldTick tick = minute; tick <= day; tick += minute) {
        auto current = renderer::evaluate_day_night(tick, time);
        assert(current);
        assert(std::abs(current.value().render.ambient_color.x -
                        previous.value().render.ambient_color.x) < 0.02F);
        assert(std::abs(current.value().render.fog_color.x - previous.value().render.fog_color.x) <
               0.04F);
        assert(std::abs(current.value().render.sun_intensity -
                        previous.value().render.sun_intensity) < 0.02F);
        previous = current;
    }
}

void test_cycle_supports_nonstandard_day_lengths() {
    simulation::WorldTimeConfig time;
    time.hours_per_day = 10;
    const auto hour = time.ticks_per_hour();
    assert(hour);
    const auto noon = renderer::evaluate_day_night(hour.value() * 5U, time);
    assert(noon);
    assert(near(noon.value().day_fraction, 0.5F));
    assert(noon.value().solar_elevation > 0.99F);
}

void test_cycle_rejects_invalid_configuration() {
    simulation::WorldTimeConfig invalid_time;
    invalid_time.ticks_per_second = 0;
    assert(!renderer::evaluate_day_night(0, invalid_time));

    renderer::DayNightCycleConfig invalid_cycle;
    invalid_cycle.twilight_elevation_width = 0.0F;
    assert(!renderer::evaluate_day_night(0, {}, invalid_cycle));
}

} // namespace

int main() {
    test_cycle_tracks_authoritative_world_time();
    test_full_day_time_lapse_is_continuous();
    test_cycle_supports_nonstandard_day_lengths();
    test_cycle_rejects_invalid_configuration();
    return 0;
}
