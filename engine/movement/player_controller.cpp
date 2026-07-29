#include "engine/movement/player_controller.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace heartstead::movement {

namespace {

constexpr double tick_seconds = 1.0 / 60.0;
constexpr std::uint16_t jump_buffer_duration = 7;
constexpr std::uint16_t coyote_duration = 6;
constexpr std::uint16_t dash_duration = 9;
constexpr std::uint16_t light_roll_duration = 21;
constexpr std::uint16_t medium_roll_duration = 27;
constexpr std::uint16_t heavy_roll_duration = 36;
constexpr std::uint16_t roll_recovery_duration = 12;
constexpr std::uint16_t roll_iframe_duration = 15;
constexpr std::uint16_t stamina_regen_delay = 42;
constexpr std::uint16_t landing_roll_duration = 9;
constexpr std::uint16_t vault_duration = 15;
constexpr std::uint16_t mantle_duration = 24;
constexpr double state_quantum = 1.0 / 65'536.0;

const std::vector<PlayerControllerTransitionRule> transition_rules{
    {PlayerControllerMode::grounded, PlayerControllerMode::airborne, PlayerTransitionCause::jump},
    {PlayerControllerMode::grounded, PlayerControllerMode::airborne, PlayerTransitionCause::fall},
    {PlayerControllerMode::grounded, PlayerControllerMode::dashing, PlayerTransitionCause::dash},
    {PlayerControllerMode::grounded, PlayerControllerMode::rolling, PlayerTransitionCause::roll},
    {PlayerControllerMode::grounded, PlayerControllerMode::mantling, PlayerTransitionCause::vault},
    {PlayerControllerMode::grounded, PlayerControllerMode::climbing,
     PlayerTransitionCause::enter_ladder},
    {PlayerControllerMode::grounded, PlayerControllerMode::swimming,
     PlayerTransitionCause::enter_fluid},
    {PlayerControllerMode::airborne, PlayerControllerMode::grounded, PlayerTransitionCause::land},
    {PlayerControllerMode::airborne, PlayerControllerMode::dashing, PlayerTransitionCause::dash},
    {PlayerControllerMode::airborne, PlayerControllerMode::mantling, PlayerTransitionCause::mantle},
    {PlayerControllerMode::airborne, PlayerControllerMode::climbing,
     PlayerTransitionCause::enter_ladder},
    {PlayerControllerMode::airborne, PlayerControllerMode::swimming,
     PlayerTransitionCause::enter_fluid},
    {PlayerControllerMode::dashing, PlayerControllerMode::dashing, PlayerTransitionCause::dash},
    {PlayerControllerMode::dashing, PlayerControllerMode::grounded,
     PlayerTransitionCause::dash_complete},
    {PlayerControllerMode::dashing, PlayerControllerMode::airborne,
     PlayerTransitionCause::dash_complete},
    {PlayerControllerMode::rolling, PlayerControllerMode::grounded,
     PlayerTransitionCause::roll_complete},
    {PlayerControllerMode::rolling, PlayerControllerMode::airborne,
     PlayerTransitionCause::roll_complete},
    {PlayerControllerMode::mantling, PlayerControllerMode::grounded,
     PlayerTransitionCause::mantle_complete},
    {PlayerControllerMode::mantling, PlayerControllerMode::airborne, PlayerTransitionCause::fall},
    {PlayerControllerMode::climbing, PlayerControllerMode::airborne,
     PlayerTransitionCause::leave_ladder},
    {PlayerControllerMode::climbing, PlayerControllerMode::grounded, PlayerTransitionCause::land},
    {PlayerControllerMode::climbing, PlayerControllerMode::swimming,
     PlayerTransitionCause::enter_fluid},
    {PlayerControllerMode::swimming, PlayerControllerMode::airborne,
     PlayerTransitionCause::leave_fluid},
    {PlayerControllerMode::swimming, PlayerControllerMode::grounded, PlayerTransitionCause::land},
    {PlayerControllerMode::swimming, PlayerControllerMode::climbing,
     PlayerTransitionCause::enter_ladder},
};

struct RollTuning {
    double distance = 3.5;
    std::uint16_t travel_ticks = light_roll_duration;
};

[[nodiscard]] double quantize(double value) noexcept {
    return std::round(value / state_quantum) * state_quantum;
}

[[nodiscard]] math::Vec3d quantize(math::Vec3d value) noexcept {
    return {quantize(value.x), quantize(value.y), quantize(value.z)};
}

[[nodiscard]] core::Result<world::WorldPosition>
quantize_position(const world::WorldPosition& position) {
    return world::WorldPosition::from_anchor(position.anchor, quantize(position.local_offset));
}

[[nodiscard]] double horizontal_length(math::Vec3d value) noexcept {
    return std::hypot(value.x, value.z);
}

[[nodiscard]] animation::LocomotionAnimationKind locomotion_kind(const PlayerControllerState& state,
                                                                 bool sprinting) noexcept {
    if (state.mode == PlayerControllerMode::swimming) {
        return animation::LocomotionAnimationKind::swim;
    }
    if (state.mode == PlayerControllerMode::airborne && !state.grounded) {
        return state.velocity.y > 0.05 ? animation::LocomotionAnimationKind::jump
                                       : animation::LocomotionAnimationKind::fall;
    }
    if (state.grounded && horizontal_length(state.velocity) > 0.05) {
        return sprinting ? animation::LocomotionAnimationKind::run
                         : animation::LocomotionAnimationKind::walk;
    }
    return animation::LocomotionAnimationKind::idle;
}

void update_locomotion_animation(const PlayerControllerState& previous,
                                 PlayerControllerState& state, bool sprinting) noexcept {
    const auto kind = locomotion_kind(state, sprinting);
    auto& locomotion = state.locomotion_animation;
    if (kind != previous.locomotion_animation.kind) {
        locomotion.kind = kind;
        locomotion.phase = 0;
        locomotion.transition_from = previous.locomotion_animation.kind;
        locomotion.transition_from_phase = previous.locomotion_animation.phase;
        locomotion.transition_tick = state.simulation_tick;
        return;
    }
    locomotion = previous.locomotion_animation;
    locomotion.kind = kind;
    if (kind == animation::LocomotionAnimationKind::idle) {
        locomotion.phase = 0;
        return;
    }
    if (kind == animation::LocomotionAnimationKind::jump ||
        kind == animation::LocomotionAnimationKind::fall) {
        constexpr std::uint64_t phase_per_tick = 65'536U / 60U;
        const auto tick_delta =
            state.simulation_tick > previous.simulation_tick
                ? std::min<std::uint64_t>(state.simulation_tick - previous.simulation_tick, 4U)
                : 1U;
        locomotion.phase = static_cast<std::uint16_t>(std::min<std::uint64_t>(
            65'535U, static_cast<std::uint64_t>(locomotion.phase) + phase_per_tick * tick_delta));
        return;
    }
    const auto displacement =
        state.position.relative_to(previous.position.anchor) - previous.position.local_offset;
    const auto distance = kind == animation::LocomotionAnimationKind::swim
                              ? math::length(displacement)
                              : std::hypot(displacement.x, displacement.z);
    constexpr double walk_cycles_per_meter = 0.55;
    constexpr double run_cycles_per_meter = 0.42;
    constexpr double swim_cycles_per_meter = 0.35;
    const auto cycles_per_meter =
        kind == animation::LocomotionAnimationKind::walk  ? walk_cycles_per_meter
        : kind == animation::LocomotionAnimationKind::run ? run_cycles_per_meter
                                                          : swim_cycles_per_meter;
    const auto advance =
        static_cast<std::uint64_t>(std::llround(distance * cycles_per_meter * 65'536.0));
    locomotion.phase = static_cast<std::uint16_t>(
        (static_cast<std::uint64_t>(locomotion.phase) + advance) & 0xFFFFU);
}

[[nodiscard]] math::Vec3d normalized_horizontal(math::Vec3d value,
                                                math::Vec3d fallback = {0.0, 0.0, 1.0}) noexcept {
    value.y = 0.0;
    const auto length = horizontal_length(value);
    if (length <= std::numeric_limits<double>::epsilon()) {
        return fallback;
    }
    return {value.x / length, 0.0, value.z / length};
}

[[nodiscard]] math::Vec3d facing_direction(std::int16_t yaw_centidegrees) noexcept {
    constexpr double degrees_to_radians = 3.14159265358979323846 / 180.0;
    const auto radians = static_cast<double>(yaw_centidegrees) * 0.01 * degrees_to_radians;
    return {std::sin(radians), 0.0, std::cos(radians)};
}

[[nodiscard]] math::Vec3d input_direction(const PlayerInputFrame& input) noexcept {
    const auto forward = facing_direction(input.yaw_centidegrees);
    const math::Vec3d right{forward.z, 0.0, -forward.x};
    const auto x = static_cast<double>(input.move_x) / 32'767.0;
    const auto z = static_cast<double>(input.move_z) / 32'767.0;
    const auto value = right * x + forward * z;
    const auto length = horizontal_length(value);
    return length > 1.0 ? value / length : value;
}

[[nodiscard]] double move_towards(double current, double target, double maximum_delta) noexcept {
    if (std::abs(target - current) <= maximum_delta) {
        return target;
    }
    return current + std::copysign(maximum_delta, target - current);
}

[[nodiscard]] RollTuning roll_tuning(const PlayerMovementConfig& config,
                                     EncumbranceTier tier) noexcept {
    switch (tier) {
    case EncumbranceTier::light:
        return {config.light_roll_distance, light_roll_duration};
    case EncumbranceTier::medium:
        return {config.medium_roll_distance, medium_roll_duration};
    case EncumbranceTier::heavy:
    case EncumbranceTier::over_encumbered:
        return {config.heavy_roll_distance, heavy_roll_duration};
    }
    return {config.light_roll_distance, light_roll_duration};
}

[[nodiscard]] double movement_scale(EncumbranceTier tier) noexcept {
    return tier == EncumbranceTier::over_encumbered ? 0.5 : 1.0;
}

[[nodiscard]] std::uint32_t sprint_drain_multiplier(EncumbranceTier tier) noexcept {
    switch (tier) {
    case EncumbranceTier::light:
        return 1000;
    case EncumbranceTier::medium:
        return 1250;
    case EncumbranceTier::heavy:
    case EncumbranceTier::over_encumbered:
        return 1500;
    }
    return 1000;
}

[[nodiscard]] double discrete_jump_impulse(double gravity, double apex) noexcept {
    double lower = 0.0;
    double upper = 32.0;
    for (int iteration = 0; iteration < 48; ++iteration) {
        const auto candidate = (lower + upper) * 0.5;
        auto velocity = candidate;
        auto height = 0.0;
        auto maximum_height = 0.0;
        for (int tick = 0; tick < 240 && velocity > 0.0; ++tick) {
            velocity -= gravity * tick_seconds;
            height += velocity * tick_seconds;
            maximum_height = std::max(maximum_height, height);
        }
        if (maximum_height < apex) {
            lower = candidate;
        } else {
            upper = candidate;
        }
    }
    return (lower + upper) * 0.5;
}

void spend_stamina(PlayerControllerState& state, std::int32_t amount,
                   const PlayerMovementConfig& config) noexcept {
    state.stamina_milli = std::max(0, state.stamina_milli - amount);
    state.stamina_regen_delay_ticks = stamina_regen_delay;
    state.stamina_regen_remainder = 0;
    if (state.stamina_milli == 0) {
        state.exhausted = true;
    }
    state.stamina_milli = std::min(state.stamina_milli, config.stamina_pool_milli);
}

[[nodiscard]] bool can_spend(const PlayerControllerState& state, std::int32_t amount) noexcept {
    return !state.exhausted && amount >= 0 && state.stamina_milli >= amount;
}

void drain_rate(PlayerControllerState& state, std::int32_t milli_per_second,
                const PlayerMovementConfig& config) noexcept {
    if (milli_per_second <= 0) {
        return;
    }
    const auto numerator = milli_per_second + state.stamina_drain_remainder;
    const auto amount = numerator / 60;
    state.stamina_drain_remainder = static_cast<std::uint8_t>(numerator % 60);
    spend_stamina(state, amount, config);
}

void regenerate(PlayerControllerState& state, std::int32_t milli_per_second,
                const PlayerMovementConfig& config) noexcept {
    if (milli_per_second <= 0 || state.stamina_milli >= config.stamina_pool_milli) {
        return;
    }
    const auto numerator = milli_per_second + state.stamina_regen_remainder;
    const auto amount = numerator / 60;
    state.stamina_regen_remainder = static_cast<std::uint8_t>(numerator % 60);
    state.stamina_milli = std::min(config.stamina_pool_milli, state.stamina_milli + amount);
}

[[nodiscard]] double vertical_distance(const world::WorldPosition& high,
                                       const world::WorldPosition& low) noexcept {
    return high.relative_to(low.anchor).y - low.local_offset.y;
}

void append_pending_fall(PlayerControllerState& state, std::vector<MovementEvent>& events,
                         bool mitigated) {
    if (!state.pending_fall_distance.has_value()) {
        return;
    }
    events.push_back({MovementEventKind::fall_impact, state.simulation_tick,
                      *state.pending_fall_distance, mitigated ? 0.5 : 1.0});
    state.pending_fall_distance.reset();
    state.landing_roll_window_ticks = 0;
}

[[nodiscard]] core::Result<math::Vec3d> delta_to(const world::WorldPosition& from,
                                                 const world::WorldPosition& to) {
    if (!from.is_valid() || !to.is_valid()) {
        return core::Result<math::Vec3d>::failure("player_controller.invalid_scripted_position",
                                                  "scripted positions must be valid");
    }
    return core::Result<math::Vec3d>::success(to.relative_to(from.anchor) - from.local_offset);
}

} // namespace

core::Status PlayerMovementConfig::validate() const {
    const std::array dimensions{standing_width, standing_height, crouch_height,
                                roll_height,    eye_height,      auto_step_height};
    const std::array rates{walk_speed,          sprint_speed,
                           crouch_speed,        swim_speed,
                           ladder_speed,        ground_acceleration,
                           ground_deceleration, air_acceleration,
                           swim_acceleration,   swim_vertical_acceleration,
                           swim_vertical_drag,  gravity,
                           jump_apex,           dash_distance,
                           light_roll_distance, medium_roll_distance,
                           heavy_roll_distance, mantle_max_height,
                           vault_max_height};
    if (!std::ranges::all_of(dimensions,
                             [](double value) { return std::isfinite(value) && value > 0.0; }) ||
        !std::ranges::all_of(rates,
                             [](double value) { return std::isfinite(value) && value > 0.0; })) {
        return core::Status::failure("player_controller.invalid_config",
                                     "movement dimensions and rates must be finite and positive");
    }
    if (!std::isfinite(swim_resting_submersion) || !std::isfinite(swim_enter_submersion) ||
        !std::isfinite(swim_exit_submersion) || swim_resting_submersion <= 0.0 ||
        swim_resting_submersion > 1.0 || swim_enter_submersion <= 0.0 ||
        swim_enter_submersion > 1.0 || swim_exit_submersion < 0.0 ||
        swim_exit_submersion >= swim_enter_submersion) {
        return core::Status::failure(
            "player_controller.invalid_swim_config",
            "swim submersion thresholds must be finite, normalized, and hysteretic");
    }
    if (crouch_height > standing_height || roll_height > crouch_height ||
        eye_height >= standing_height || auto_step_height >= standing_height ||
        walk_speed > sprint_speed || vault_max_height > mantle_max_height) {
        return core::Status::failure("player_controller.invalid_relationship",
                                     "movement configuration dimensions or speeds conflict");
    }
    const std::array costs{health_pool_milli,
                           stamina_pool_milli,
                           stamina_regen_milli_per_second,
                           sprint_drain_milli_per_second,
                           swim_drain_milli_per_second,
                           jump_cost_milli,
                           dash_cost_milli,
                           roll_cost_milli,
                           mantle_cost_milli};
    if (!std::ranges::all_of(costs, [](std::int32_t value) { return value > 0; })) {
        return core::Status::failure("player_controller.invalid_vitals_config",
                                     "movement health and stamina values must be positive");
    }
    return core::Status::ok();
}

core::Status PlayerControllerState::validate(const PlayerMovementConfig& config) const {
    if (!position.is_valid() || !fall_origin.is_valid() || !scripted_start.is_valid() ||
        !scripted_target.is_valid() || !velocity.is_finite() || !scripted_direction.is_finite()) {
        return core::Status::failure("player_controller.invalid_state_position",
                                     "player controller state contains an invalid position/vector");
    }
    if (health_milli < 0 || health_milli > config.health_pool_milli || stamina_milli < 0 ||
        stamina_milli > config.stamina_pool_milli || dash_charges > 2 ||
        pitch_centidegrees < -8'900 || pitch_centidegrees > 8'900) {
        return core::Status::failure("player_controller.invalid_state",
                                     "player controller state values are out of range");
    }
    if (pending_fall_distance.has_value() &&
        (!std::isfinite(*pending_fall_distance) || *pending_fall_distance < 0.0)) {
        return core::Status::failure("player_controller.invalid_fall_distance",
                                     "pending fall distance must be finite and non-negative");
    }
    auto animation_status = locomotion_animation.validate(simulation_tick);
    if (!animation_status) {
        return animation_status;
    }
    return core::Status::ok();
}

PlayerController::PlayerController(PlayerMovementConfig config) : config_(config) {}

core::Result<PlayerControllerTickResult>
PlayerController::tick(const PlayerControllerState& previous, const PlayerInputFrame& input,
                       const PlayerMovementModifiers& modifiers,
                       ICharacterCollisionWorld& collision) const {
    auto config_status = config_.validate();
    auto input_status = input.validate();
    auto state_status = previous.validate(config_);
    if (!config_status || !input_status || !state_status ||
        input.sequence < previous.last_input_sequence || input.tick <= previous.simulation_tick ||
        modifiers.stamina_regen_per_mille > 10'000) {
        const auto* error = !config_status  ? &config_status.error()
                            : !input_status ? &input_status.error()
                            : !state_status ? &state_status.error()
                                            : nullptr;
        return core::Result<PlayerControllerTickResult>::failure(
            error != nullptr ? error->code : "player_controller.invalid_tick",
            error != nullptr ? error->message
                             : "player controller input ordering or modifiers are invalid");
    }

    PlayerControllerTickResult result;
    auto& state = result.state;
    state = previous;
    state.simulation_tick = input.tick;
    state.last_input_sequence = input.sequence;
    state.yaw_centidegrees = input.yaw_centidegrees;
    state.pitch_centidegrees = input.pitch_centidegrees;
    state.encumbrance = encumbrance_tier(modifiers.load_per_mille);
    state.invulnerable = false;
    state.mode_ticks = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(static_cast<std::uint32_t>(state.mode_ticks) + 1u,
                                std::numeric_limits<std::uint16_t>::max()));

    if (input.pressed(PlayerInputButton::jump)) {
        state.jump_buffer_ticks = jump_buffer_duration;
    } else if (state.jump_buffer_ticks > 0) {
        --state.jump_buffer_ticks;
    }
    if (input.pressed(PlayerInputButton::roll)) {
        state.roll_buffer_ticks = landing_roll_duration;
    } else if (state.roll_buffer_ticks > 0) {
        --state.roll_buffer_ticks;
    }
    if (state.stamina_regen_delay_ticks > 0) {
        --state.stamina_regen_delay_ticks;
    }
    if (state.landing_roll_window_ticks > 0) {
        --state.landing_roll_window_ticks;
        if (state.landing_roll_window_ticks == 0) {
            append_pending_fall(state, result.events, false);
        }
    }
    if (state.exhausted && state.stamina_milli >= 15'000) {
        state.exhausted = false;
    }

    auto current_shape = shape_for(state);
    auto fluid_submersion = collision.fluid_submersion(state.position, current_shape);
    auto touching_ladder = collision.touches_tag(state.position, current_shape, "ladder");
    if (!fluid_submersion || !touching_ladder) {
        const auto& error = !fluid_submersion ? fluid_submersion.error() : touching_ladder.error();
        return core::Result<PlayerControllerTickResult>::failure(error.code, error.message);
    }
    const auto was_swimming = previous.mode == PlayerControllerMode::swimming;
    const auto swim_threshold =
        was_swimming ? config_.swim_exit_submersion : config_.swim_enter_submersion;
    const auto should_swim = fluid_submersion.value() > swim_threshold;

    const auto direction = input_direction(input);
    const auto facing = facing_direction(input.yaw_centidegrees);
    const auto movement_modifier = movement_scale(state.encumbrance);
    const auto begin_mode = [&state](PlayerControllerMode mode, ScriptedMovementKind kind,
                                     std::uint16_t duration) {
        state.mode = mode;
        state.scripted_kind = kind;
        state.mode_ticks = 0;
        state.mode_duration_ticks = duration;
        state.scripted_start = state.position;
    };

    const auto start_roll = [&]() {
        if (!state.grounded || !can_spend(state, config_.roll_cost_milli)) {
            return false;
        }
        const auto tuning = roll_tuning(config_, state.encumbrance);
        spend_stamina(state, config_.roll_cost_milli, config_);
        begin_mode(PlayerControllerMode::rolling, ScriptedMovementKind::roll,
                   static_cast<std::uint16_t>(tuning.travel_ticks + roll_recovery_duration));
        state.scripted_direction = normalized_horizontal(direction, facing);
        state.crouched = true;
        state.roll_buffer_ticks = 0;
        result.events.push_back({MovementEventKind::roll_started, input.tick});
        append_pending_fall(state, result.events, true);
        return true;
    };

    const auto start_dash = [&]() {
        const auto airborne = !state.grounded;
        if (state.dash_charges == 0 || !can_spend(state, config_.dash_cost_milli) ||
            (airborne &&
             (!state.air_dash_available || state.encumbrance != EncumbranceTier::light))) {
            return false;
        }
        spend_stamina(state, config_.dash_cost_milli, config_);
        --state.dash_charges;
        if (airborne) {
            state.air_dash_available = false;
        }
        begin_mode(PlayerControllerMode::dashing, ScriptedMovementKind::dash, dash_duration);
        state.scripted_direction = normalized_horizontal(direction, facing);
        state.velocity = {};
        result.events.push_back({MovementEventKind::dashed, input.tick});
        return true;
    };

    if (state.mode != PlayerControllerMode::rolling &&
        state.mode != PlayerControllerMode::mantling && input.pressed(PlayerInputButton::roll) &&
        state.grounded && !should_swim) {
        (void)start_roll();
    }
    if (state.mode != PlayerControllerMode::rolling &&
        state.mode != PlayerControllerMode::mantling && input.pressed(PlayerInputButton::dash) &&
        !should_swim) {
        (void)start_dash();
    }

    if (state.mode == PlayerControllerMode::dashing) {
        const auto delta =
            state.scripted_direction * (config_.dash_distance * movement_modifier / dash_duration);
        auto moved = collision.move(state.position, shape_for(state), delta);
        if (!moved) {
            return core::Result<PlayerControllerTickResult>::failure(moved.error().code,
                                                                     moved.error().message);
        }
        state.position = moved.value().position;
        state.velocity = delta / tick_seconds;
        const auto blocked = moved.value().hit_x || moved.value().hit_z;
        if (state.mode_ticks + 1 >= state.mode_duration_ticks || blocked) {
            state.grounded = moved.value().grounded;
            state.mode =
                state.grounded ? PlayerControllerMode::grounded : PlayerControllerMode::airborne;
            state.scripted_kind = ScriptedMovementKind::none;
            state.mode_ticks = 0;
            state.velocity = {};
        }
    } else if (state.mode == PlayerControllerMode::rolling) {
        const auto tuning = roll_tuning(config_, state.encumbrance);
        state.invulnerable = state.mode_ticks < roll_iframe_duration;
        math::Vec3d delta{};
        if (state.mode_ticks < tuning.travel_ticks) {
            delta = state.scripted_direction *
                    (tuning.distance * movement_modifier / tuning.travel_ticks);
        }
        auto moved = collision.move(state.position, shape_for(state), delta, 0.0, false);
        if (!moved) {
            return core::Result<PlayerControllerTickResult>::failure(moved.error().code,
                                                                     moved.error().message);
        }
        state.position = moved.value().position;
        state.velocity = delta / tick_seconds;
        state.grounded = moved.value().grounded;
        if (state.mode_ticks + 1 >= state.mode_duration_ticks) {
            state.mode =
                state.grounded ? PlayerControllerMode::grounded : PlayerControllerMode::airborne;
            state.scripted_kind = ScriptedMovementKind::none;
            state.mode_ticks = 0;
            state.velocity = {};
            state.crouched = input.held(PlayerInputButton::crouch);
            if (!state.crouched) {
                auto standing_clear = collision.overlaps(
                    state.position, {config_.standing_width, config_.standing_height});
                if (!standing_clear) {
                    return core::Result<PlayerControllerTickResult>::failure(
                        standing_clear.error().code, standing_clear.error().message);
                }
                state.crouched = standing_clear.value();
            }
        }
    } else if (state.mode == PlayerControllerMode::mantling) {
        auto remaining = delta_to(state.position, state.scripted_target);
        if (!remaining) {
            return core::Result<PlayerControllerTickResult>::failure(remaining.error().code,
                                                                     remaining.error().message);
        }
        const auto ticks_left = std::max<std::uint16_t>(
            1, static_cast<std::uint16_t>(state.mode_duration_ticks - state.mode_ticks));
        math::Vec3d delta{};
        const auto half = static_cast<std::uint16_t>(state.mode_duration_ticks / 2);
        if (state.mode_ticks < half) {
            delta.y = remaining.value().y / std::max<std::uint16_t>(1, half - state.mode_ticks);
        } else {
            delta = remaining.value() / static_cast<double>(ticks_left);
        }
        auto moved = collision.move(state.position, shape_for(state), delta);
        if (!moved) {
            return core::Result<PlayerControllerTickResult>::failure(moved.error().code,
                                                                     moved.error().message);
        }
        state.position = moved.value().position;
        state.velocity = delta / tick_seconds;
        if (moved.value().hit_x || moved.value().hit_y || moved.value().hit_z) {
            state.mode = PlayerControllerMode::airborne;
            state.grounded = false;
            state.scripted_kind = ScriptedMovementKind::none;
            state.mode_ticks = 0;
            state.velocity = {};
        } else if (state.mode_ticks + 1 >= state.mode_duration_ticks) {
            state.position = state.scripted_target;
            state.mode = PlayerControllerMode::grounded;
            state.grounded = true;
            state.scripted_kind = ScriptedMovementKind::none;
            state.mode_ticks = 0;
            state.velocity = {};
            result.events.push_back({MovementEventKind::mantle_completed, input.tick});
        }
    } else if (should_swim) {
        state.mode = PlayerControllerMode::swimming;
        state.grounded = false;
        const auto vertical = static_cast<double>(input.held(PlayerInputButton::jump)) -
                              static_cast<double>(input.held(PlayerInputButton::crouch));
        const auto target = direction * config_.swim_speed * movement_modifier;
        state.velocity.x =
            move_towards(state.velocity.x, target.x, config_.swim_acceleration * tick_seconds);
        state.velocity.z =
            move_towards(state.velocity.z, target.z, config_.swim_acceleration * tick_seconds);
        const auto drag = std::max(0.0, 1.0 - config_.swim_vertical_drag *
                                                  fluid_submersion.value() * tick_seconds);
        state.velocity.y *= drag;
        const auto buoyancy =
            config_.gravity * (fluid_submersion.value() / config_.swim_resting_submersion - 1.0);
        state.velocity.y +=
            (buoyancy + vertical * config_.swim_vertical_acceleration) * tick_seconds;
        state.velocity.y = std::clamp(state.velocity.y, -config_.swim_speed, config_.swim_speed);
        const auto horizontal_speed = horizontal_length(state.velocity);
        if (horizontal_speed > config_.swim_speed) {
            const auto scale = config_.swim_speed / horizontal_speed;
            state.velocity.x *= scale;
            state.velocity.z *= scale;
        }
        auto moved = collision.move(state.position, shape_for(state), state.velocity * tick_seconds,
                                    config_.auto_step_height);
        if (!moved) {
            return core::Result<PlayerControllerTickResult>::failure(moved.error().code,
                                                                     moved.error().message);
        }
        state.position = moved.value().position;
        if (moved.value().hit_x) {
            state.velocity.x = 0.0;
        }
        if (moved.value().hit_y) {
            state.velocity.y = 0.0;
        }
        if (moved.value().hit_z) {
            state.velocity.z = 0.0;
        }
        if (horizontal_length(direction) > 0.0 || vertical != 0.0) {
            drain_rate(state, config_.swim_drain_milli_per_second, config_);
        }
    } else if (touching_ladder.value() && input.held(PlayerInputButton::interact)) {
        state.mode = PlayerControllerMode::climbing;
        state.grounded = false;
        state.velocity = {direction.x * config_.ladder_speed * 0.25,
                          static_cast<double>(input.move_z) / 32'767.0 * config_.ladder_speed,
                          direction.z * config_.ladder_speed * 0.25};
        if (input.pressed(PlayerInputButton::jump)) {
            state.mode = PlayerControllerMode::airborne;
            state.velocity = facing * -2.5;
            state.velocity.y = discrete_jump_impulse(config_.gravity, config_.jump_apex) * 0.75;
        }
        auto moved =
            collision.move(state.position, shape_for(state), state.velocity * tick_seconds);
        if (!moved) {
            return core::Result<PlayerControllerTickResult>::failure(moved.error().code,
                                                                     moved.error().message);
        }
        state.position = moved.value().position;
    } else {
        const auto was_grounded = state.grounded;
        if (state.grounded) {
            state.coyote_ticks = coyote_duration;
            state.dash_charges = 2;
            state.air_dash_available = true;
        } else if (state.coyote_ticks > 0) {
            --state.coyote_ticks;
        }

        if (state.grounded && input.held(PlayerInputButton::sprint) &&
            horizontal_length(direction) > 0.0 && state.stamina_milli >= 10'000 &&
            !state.exhausted) {
            auto ledge = collision.probe_ledge(state.position, shape_for(state), direction,
                                               config_.vault_max_height);
            if (!ledge) {
                return core::Result<PlayerControllerTickResult>::failure(ledge.error().code,
                                                                         ledge.error().message);
            }
            if (ledge.value().has_value()) {
                begin_mode(PlayerControllerMode::mantling, ScriptedMovementKind::vault,
                           vault_duration);
                state.scripted_target = ledge.value()->target_feet;
                result.events.push_back(
                    {MovementEventKind::vault_started, input.tick, ledge.value()->ledge_height});
            }
        }

        if (state.mode != PlayerControllerMode::mantling && !state.grounded &&
            input.held(PlayerInputButton::jump) && can_spend(state, config_.mantle_cost_milli)) {
            auto ledge = collision.probe_ledge(state.position, shape_for(state), facing,
                                               config_.mantle_max_height);
            if (!ledge) {
                return core::Result<PlayerControllerTickResult>::failure(ledge.error().code,
                                                                         ledge.error().message);
            }
            if (ledge.value().has_value()) {
                spend_stamina(state, config_.mantle_cost_milli, config_);
                begin_mode(PlayerControllerMode::mantling, ScriptedMovementKind::mantle,
                           mantle_duration);
                state.scripted_target = ledge.value()->target_feet;
                state.velocity = {};
                result.events.push_back(
                    {MovementEventKind::mantle_grabbed, input.tick, ledge.value()->ledge_height});
            }
        }

        if (state.mode != PlayerControllerMode::mantling) {
            if (state.jump_buffer_ticks > 0 && (state.grounded || state.coyote_ticks > 0) &&
                can_spend(state, config_.jump_cost_milli)) {
                spend_stamina(state, config_.jump_cost_milli, config_);
                state.velocity.y = discrete_jump_impulse(config_.gravity, config_.jump_apex);
                state.grounded = false;
                state.mode = PlayerControllerMode::airborne;
                state.jump_buffer_ticks = 0;
                state.coyote_ticks = 0;
                state.fall_origin = state.position;
                result.events.push_back({MovementEventKind::jumped, input.tick});
            }

            state.crouched = input.held(PlayerInputButton::crouch);
            if (!state.crouched) {
                auto blocked = collision.overlaps(
                    state.position, {config_.standing_width, config_.standing_height});
                if (!blocked) {
                    return core::Result<PlayerControllerTickResult>::failure(
                        blocked.error().code, blocked.error().message);
                }
                state.crouched = blocked.value();
            }

            const auto sprinting = state.grounded && !state.crouched &&
                                   input.held(PlayerInputButton::sprint) &&
                                   state.stamina_milli >= 10'000 && !state.exhausted;
            const auto speed = (state.crouched ? config_.crouch_speed
                                : sprinting    ? config_.sprint_speed
                                               : config_.walk_speed) *
                               movement_modifier;
            const auto target = direction * speed;
            const auto has_input = horizontal_length(direction) > 0.0;
            const auto acceleration = state.grounded ? (has_input ? config_.ground_acceleration
                                                                  : config_.ground_deceleration)
                                                     : config_.air_acceleration;
            state.velocity.x =
                move_towards(state.velocity.x, target.x, acceleration * tick_seconds);
            state.velocity.z =
                move_towards(state.velocity.z, target.z, acceleration * tick_seconds);
            if (!state.grounded) {
                state.velocity.y -= config_.gravity * tick_seconds;
            } else if (state.velocity.y < 0.0) {
                state.velocity.y = 0.0;
            }

            if (!state.grounded && vertical_distance(state.fall_origin, state.position) < 0.0) {
                state.fall_origin = state.position;
            }
            auto moved = collision.move(
                state.position, shape_for(state), state.velocity * tick_seconds,
                state.grounded ? config_.auto_step_height : 0.0, state.crouched && state.grounded);
            if (!moved) {
                return core::Result<PlayerControllerTickResult>::failure(moved.error().code,
                                                                         moved.error().message);
            }
            state.position = moved.value().position;
            if (moved.value().hit_x) {
                state.velocity.x = 0.0;
            }
            if (moved.value().hit_z) {
                state.velocity.z = 0.0;
            }
            if (moved.value().hit_y) {
                state.velocity.y = 0.0;
            }
            state.grounded = moved.value().grounded;
            state.mode =
                state.grounded ? PlayerControllerMode::grounded : PlayerControllerMode::airborne;

            if (!was_grounded && state.grounded) {
                const auto fall_distance =
                    std::max(0.0, vertical_distance(state.fall_origin, state.position));
                result.events.push_back({MovementEventKind::landed, input.tick, fall_distance});
                if (fall_distance > 4.0) {
                    state.pending_fall_distance = fall_distance;
                    state.landing_roll_window_ticks = landing_roll_duration;
                }
                state.dash_charges = 2;
                state.air_dash_available = true;
                if (state.roll_buffer_ticks > 0) {
                    (void)start_roll();
                }
            } else if (was_grounded && !state.grounded) {
                state.fall_origin = previous.position;
            }
            if (sprinting && horizontal_length(state.velocity) > 0.0) {
                const auto rate = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(config_.sprint_drain_milli_per_second) *
                    sprint_drain_multiplier(state.encumbrance) / 1000);
                drain_rate(state, rate, config_);
            }
        }
    }

    if (state.stamina_regen_delay_ticks == 0 &&
        state.encumbrance != EncumbranceTier::over_encumbered &&
        state.mode != PlayerControllerMode::swimming) {
        const auto rate = static_cast<std::int32_t>(
            static_cast<std::int64_t>(config_.stamina_regen_milli_per_second) *
            modifiers.stamina_regen_per_mille / 1000);
        regenerate(state, rate, config_);
    }
    if (state.stamina_milli == 0) {
        state.exhausted = true;
    } else if (state.exhausted && state.stamina_milli >= 15'000) {
        state.exhausted = false;
    }
    const auto is_swimming = state.mode == PlayerControllerMode::swimming;
    if (!was_swimming && is_swimming) {
        result.events.push_back(
            {MovementEventKind::entered_fluid, input.tick, fluid_submersion.value()});
    } else if (was_swimming && !is_swimming) {
        result.events.push_back(
            {MovementEventKind::left_fluid, input.tick, fluid_submersion.value()});
    }

    state.velocity = quantize(state.velocity);
    auto quantized_position = quantize_position(state.position);
    if (!quantized_position) {
        return core::Result<PlayerControllerTickResult>::failure(
            quantized_position.error().code, quantized_position.error().message);
    }
    state.position = quantized_position.value();
    const auto presentation_sprinting =
        state.grounded && !state.crouched && input.held(PlayerInputButton::sprint) &&
        horizontal_length(state.velocity) > config_.walk_speed * movement_modifier * 0.95;
    update_locomotion_animation(previous, state, presentation_sprinting);
    auto final_status = state.validate(config_);
    if (!final_status) {
        return core::Result<PlayerControllerTickResult>::failure(final_status.error().code,
                                                                 final_status.error().message);
    }
    return core::Result<PlayerControllerTickResult>::success(std::move(result));
}

const PlayerMovementConfig& PlayerController::config() const noexcept {
    return config_;
}

EncumbranceTier PlayerController::encumbrance_tier(std::uint32_t load_per_mille) noexcept {
    if (load_per_mille < 400) {
        return EncumbranceTier::light;
    }
    if (load_per_mille < 800) {
        return EncumbranceTier::medium;
    }
    if (load_per_mille <= 1000) {
        return EncumbranceTier::heavy;
    }
    return EncumbranceTier::over_encumbered;
}

CharacterShape PlayerController::shape_for(const PlayerControllerState& state) const noexcept {
    if (state.mode == PlayerControllerMode::rolling) {
        return {config_.standing_width, config_.roll_height};
    }
    if (state.crouched) {
        return {config_.standing_width, config_.crouch_height};
    }
    return {config_.standing_width, config_.standing_height};
}

std::string_view player_controller_mode_name(PlayerControllerMode mode) noexcept {
    switch (mode) {
    case PlayerControllerMode::grounded:
        return "grounded";
    case PlayerControllerMode::airborne:
        return "airborne";
    case PlayerControllerMode::dashing:
        return "dashing";
    case PlayerControllerMode::rolling:
        return "rolling";
    case PlayerControllerMode::mantling:
        return "mantling";
    case PlayerControllerMode::climbing:
        return "climbing";
    case PlayerControllerMode::swimming:
        return "swimming";
    }
    return "unknown";
}

std::string_view encumbrance_tier_name(EncumbranceTier tier) noexcept {
    switch (tier) {
    case EncumbranceTier::light:
        return "light";
    case EncumbranceTier::medium:
        return "medium";
    case EncumbranceTier::heavy:
        return "heavy";
    case EncumbranceTier::over_encumbered:
        return "over_encumbered";
    }
    return "unknown";
}

std::string_view movement_event_kind_name(MovementEventKind kind) noexcept {
    switch (kind) {
    case MovementEventKind::jumped:
        return "jumped";
    case MovementEventKind::dashed:
        return "dashed";
    case MovementEventKind::roll_started:
        return "roll_started";
    case MovementEventKind::vault_started:
        return "vault_started";
    case MovementEventKind::mantle_grabbed:
        return "mantle_grabbed";
    case MovementEventKind::mantle_completed:
        return "mantle_completed";
    case MovementEventKind::landed:
        return "landed";
    case MovementEventKind::fall_impact:
        return "fall_impact";
    case MovementEventKind::entered_fluid:
        return "entered_fluid";
    case MovementEventKind::left_fluid:
        return "left_fluid";
    }
    return "unknown";
}

const std::vector<PlayerControllerTransitionRule>& default_player_controller_transitions() {
    return transition_rules;
}

bool player_controller_transition_allowed(PlayerControllerMode from, PlayerControllerMode to,
                                          PlayerTransitionCause cause) noexcept {
    return std::ranges::any_of(transition_rules, [&](const auto& rule) {
        return rule.from == from && rule.to == to && rule.cause == cause;
    });
}

} // namespace heartstead::movement
