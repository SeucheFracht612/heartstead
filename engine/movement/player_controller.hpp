#pragma once

#include "engine/animation/locomotion_animation.hpp"
#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/movement/character_collision.hpp"
#include "engine/movement/player_input.hpp"
#include "engine/world/coords/world_position.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace heartstead::movement {

enum class PlayerControllerMode : std::uint8_t {
    grounded,
    airborne,
    dashing,
    rolling,
    mantling,
    climbing,
    swimming,
};

enum class EncumbranceTier : std::uint8_t { light, medium, heavy, over_encumbered };

enum class ScriptedMovementKind : std::uint8_t { none, dash, roll, vault, mantle };

enum class MovementEventKind : std::uint8_t {
    jumped,
    dashed,
    roll_started,
    vault_started,
    mantle_grabbed,
    mantle_completed,
    landed,
    fall_impact,
    entered_fluid,
    left_fluid,
};

enum class PlayerTransitionCause : std::uint8_t {
    jump,
    fall,
    land,
    dash,
    dash_complete,
    roll,
    roll_complete,
    vault,
    mantle,
    mantle_complete,
    enter_ladder,
    leave_ladder,
    enter_fluid,
    leave_fluid,
};

struct PlayerControllerTransitionRule {
    PlayerControllerMode from = PlayerControllerMode::grounded;
    PlayerControllerMode to = PlayerControllerMode::airborne;
    PlayerTransitionCause cause = PlayerTransitionCause::fall;
};

struct MovementEvent {
    MovementEventKind kind = MovementEventKind::landed;
    std::uint64_t tick = 0;
    double value = 0.0;
    double damage_multiplier = 1.0;
};

struct PlayerMovementConfig {
    double standing_width = 0.6;
    double standing_height = 1.8;
    double crouch_height = 1.2;
    double roll_height = 1.0;
    double eye_height = 1.62;
    double auto_step_height = 0.6;
    double walk_speed = 4.5;
    double sprint_speed = 7.5;
    double crouch_speed = 2.2;
    double swim_speed = 3.2;
    double ladder_speed = 3.0;
    double ground_acceleration = 55.0;
    double ground_deceleration = 60.0;
    double air_acceleration = 12.0;
    double swim_acceleration = 14.0;
    double swim_vertical_acceleration = 18.0;
    double swim_vertical_drag = 3.5;
    double swim_resting_submersion = 0.85;
    double swim_enter_submersion = 0.10;
    double swim_exit_submersion = 0.05;
    double gravity = 24.0;
    double jump_apex = 1.25;
    double dash_distance = 4.0;
    double light_roll_distance = 3.5;
    double medium_roll_distance = 3.0;
    double heavy_roll_distance = 2.2;
    double mantle_max_height = 2.0;
    double vault_max_height = 1.05;
    std::int32_t health_pool_milli = 100'000;
    std::int32_t stamina_pool_milli = 100'000;
    std::int32_t stamina_regen_milli_per_second = 14'000;
    std::int32_t sprint_drain_milli_per_second = 5'000;
    std::int32_t swim_drain_milli_per_second = 4'000;
    std::int32_t jump_cost_milli = 8'000;
    std::int32_t dash_cost_milli = 12'000;
    std::int32_t roll_cost_milli = 20'000;
    std::int32_t mantle_cost_milli = 10'000;

    [[nodiscard]] core::Status validate() const;
};

struct PlayerMovementModifiers {
    std::uint32_t load_per_mille = 0;
    std::uint32_t stamina_regen_per_mille = 1000;
};

struct PlayerControllerState {
    world::WorldPosition position;
    math::Vec3d velocity{};
    PlayerControllerMode mode = PlayerControllerMode::airborne;
    ScriptedMovementKind scripted_kind = ScriptedMovementKind::none;
    EncumbranceTier encumbrance = EncumbranceTier::light;
    bool crouched = false;
    bool grounded = false;
    bool exhausted = false;
    bool invulnerable = false;
    std::int16_t yaw_centidegrees = 0;
    std::int16_t pitch_centidegrees = 0;
    std::int32_t health_milli = 100'000;
    std::int32_t stamina_milli = 100'000;
    std::uint8_t dash_charges = 2;
    bool air_dash_available = true;
    std::uint16_t mode_ticks = 0;
    std::uint16_t mode_duration_ticks = 0;
    std::uint16_t jump_buffer_ticks = 0;
    std::uint16_t coyote_ticks = 0;
    std::uint16_t roll_buffer_ticks = 0;
    std::uint16_t stamina_regen_delay_ticks = 0;
    std::uint16_t landing_roll_window_ticks = 0;
    std::uint8_t stamina_drain_remainder = 0;
    std::uint8_t stamina_regen_remainder = 0;
    std::optional<double> pending_fall_distance;
    world::WorldPosition fall_origin;
    math::Vec3d scripted_direction{};
    world::WorldPosition scripted_start;
    world::WorldPosition scripted_target;
    animation::ReplicatedLocomotionAnimation locomotion_animation;
    std::uint64_t last_input_sequence = 0;
    std::uint64_t simulation_tick = 0;

    [[nodiscard]] core::Status validate(const PlayerMovementConfig& config) const;
};

struct PlayerControllerTickDiagnostics {
    math::Vec3d requested_displacement{};
    math::Vec3d applied_displacement{};
    math::Vec3d depenetration_displacement{};
    bool hit_x = false;
    bool hit_y = false;
    bool hit_z = false;
    bool hit_ceiling = false;
    bool stepped = false;
    bool blocked_by_unloaded_chunk = false;
};

struct PlayerControllerTickResult {
    PlayerControllerState state;
    std::vector<MovementEvent> events;
    PlayerControllerTickDiagnostics diagnostics;
};

class PlayerController {
  public:
    explicit PlayerController(PlayerMovementConfig config = {});

    [[nodiscard]] core::Result<PlayerControllerTickResult>
    tick(const PlayerControllerState& previous, const PlayerInputFrame& input,
         const PlayerMovementModifiers& modifiers, ICharacterCollisionWorld& collision) const;

    [[nodiscard]] const PlayerMovementConfig& config() const noexcept;
    [[nodiscard]] static EncumbranceTier encumbrance_tier(std::uint32_t load_per_mille) noexcept;
    [[nodiscard]] CharacterShape shape_for(const PlayerControllerState& state) const noexcept;

  private:
    PlayerMovementConfig config_;
};

[[nodiscard]] std::string_view player_controller_mode_name(PlayerControllerMode mode) noexcept;
[[nodiscard]] std::string_view encumbrance_tier_name(EncumbranceTier tier) noexcept;
[[nodiscard]] std::string_view movement_event_kind_name(MovementEventKind kind) noexcept;
[[nodiscard]] const std::vector<PlayerControllerTransitionRule>&
default_player_controller_transitions();
[[nodiscard]] bool player_controller_transition_allowed(PlayerControllerMode from,
                                                        PlayerControllerMode to,
                                                        PlayerTransitionCause cause) noexcept;

} // namespace heartstead::movement
