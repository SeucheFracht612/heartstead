#include "engine/movement/remote_player_interpolation.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace heartstead::movement {

namespace {

[[nodiscard]] double interpolate_angle(double from, double to, double alpha) noexcept {
    auto delta = std::fmod(to - from, 36'000.0);
    if (delta > 18'000.0) {
        delta -= 36'000.0;
    } else if (delta < -18'000.0) {
        delta += 36'000.0;
    }
    return from + delta * alpha;
}

[[nodiscard]] std::uint16_t interpolate_phase(std::uint16_t from, std::uint16_t to,
                                              double alpha) noexcept {
    constexpr std::int32_t period = 65'536;
    constexpr std::int32_t half_period = period / 2;
    auto delta = static_cast<std::int32_t>(to) - static_cast<std::int32_t>(from);
    if (delta > half_period) {
        delta -= period;
    } else if (delta < -half_period) {
        delta += period;
    }
    auto result = static_cast<std::int64_t>(
        std::llround(static_cast<double>(from) + static_cast<double>(delta) * alpha));
    result %= period;
    if (result < 0) {
        result += period;
    }
    return static_cast<std::uint16_t>(result);
}

} // namespace

RemotePlayerInterpolator::RemotePlayerInterpolator(std::uint32_t delay_ticks, std::size_t capacity)
    : delay_ticks_(delay_ticks), capacity_(capacity) {}

core::Status RemotePlayerInterpolator::push(PlayerControllerSnapshot snapshot,
                                            const PlayerMovementConfig& config) {
    auto status = snapshot.validate(config);
    if (!status) {
        return status;
    }
    if (capacity_ < 2 || capacity_ > 4096 || delay_ticks_ > 600) {
        return core::Status::failure("remote_interpolation.invalid_config",
                                     "remote interpolation configuration is invalid");
    }
    if (!snapshots_.empty() &&
        snapshot.state.simulation_tick <= snapshots_.back().state.simulation_tick) {
        return core::Status::failure("remote_interpolation.out_of_order",
                                     "remote snapshots must have increasing ticks");
    }
    if (snapshots_.size() == capacity_) {
        snapshots_.pop_front();
    }
    snapshots_.push_back(std::move(snapshot));
    return core::Status::ok();
}

core::Result<PlayerControllerState>
RemotePlayerInterpolator::sample(std::uint64_t render_tick) const {
    if (snapshots_.empty()) {
        return core::Result<PlayerControllerState>::failure(
            "remote_interpolation.empty", "remote interpolation has no snapshots");
    }
    const auto target_tick = render_tick > delay_ticks_ ? render_tick - delay_ticks_ : 0;
    if (target_tick <= snapshots_.front().state.simulation_tick) {
        return core::Result<PlayerControllerState>::success(snapshots_.front().state);
    }
    if (target_tick >= snapshots_.back().state.simulation_tick) {
        return core::Result<PlayerControllerState>::success(snapshots_.back().state);
    }

    const auto upper = std::ranges::find_if(snapshots_, [target_tick](const auto& snapshot) {
        return snapshot.state.simulation_tick >= target_tick;
    });
    const auto lower = std::prev(upper);
    const auto tick_span = upper->state.simulation_tick - lower->state.simulation_tick;
    const auto alpha = static_cast<double>(target_tick - lower->state.simulation_tick) /
                       static_cast<double>(tick_span);
    auto result = lower->state;
    const auto relative = upper->state.position.relative_to(lower->state.position.anchor) -
                          lower->state.position.local_offset;
    if (math::length(relative) > 32.0) {
        return core::Result<PlayerControllerState>::success(upper->state);
    }
    auto position = world::WorldPosition::from_anchor(
        lower->state.position.anchor, lower->state.position.local_offset + relative * alpha);
    if (!position) {
        return core::Result<PlayerControllerState>::failure(position.error().code,
                                                            position.error().message);
    }
    result.position = position.value();
    result.velocity = lower->state.velocity * (1.0 - alpha) + upper->state.velocity * alpha;
    result.yaw_centidegrees = static_cast<std::int16_t>(std::lround(
        interpolate_angle(lower->state.yaw_centidegrees, upper->state.yaw_centidegrees, alpha)));
    result.pitch_centidegrees = static_cast<std::int16_t>(
        std::lround(static_cast<double>(lower->state.pitch_centidegrees) * (1.0 - alpha) +
                    static_cast<double>(upper->state.pitch_centidegrees) * alpha));
    result.health_milli = static_cast<std::int32_t>(
        std::lround(static_cast<double>(lower->state.health_milli) * (1.0 - alpha) +
                    static_cast<double>(upper->state.health_milli) * alpha));
    result.stamina_milli = static_cast<std::int32_t>(
        std::lround(static_cast<double>(lower->state.stamina_milli) * (1.0 - alpha) +
                    static_cast<double>(upper->state.stamina_milli) * alpha));
    if (lower->state.locomotion_animation.kind == upper->state.locomotion_animation.kind) {
        result.locomotion_animation.phase =
            interpolate_phase(lower->state.locomotion_animation.phase,
                              upper->state.locomotion_animation.phase, alpha);
    } else if (alpha >= 0.5) {
        result.locomotion_animation = upper->state.locomotion_animation;
    }
    result.simulation_tick = target_tick;
    return core::Result<PlayerControllerState>::success(std::move(result));
}

void RemotePlayerInterpolator::clear() noexcept {
    snapshots_.clear();
}

std::size_t RemotePlayerInterpolator::size() const noexcept {
    return snapshots_.size();
}

std::uint32_t RemotePlayerInterpolator::delay_ticks() const noexcept {
    return delay_ticks_;
}

} // namespace heartstead::movement
