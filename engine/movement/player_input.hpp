#pragma once

#include "engine/core/result.hpp"
#include "engine/platform/platform.hpp"
#include "engine/simulation/fixed_step.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::movement {

inline constexpr std::uint16_t player_input_version = 1;
inline constexpr double max_player_look_sensitivity_centidegrees_per_pixel = 36'000.0;

enum class PlayerInputButton : std::uint32_t {
    jump = 1u << 0u,
    sprint = 1u << 1u,
    crouch = 1u << 2u,
    dash = 1u << 3u,
    roll = 1u << 4u,
    interact = 1u << 5u,
};

[[nodiscard]] constexpr std::uint32_t input_button_bit(PlayerInputButton button) noexcept {
    return static_cast<std::uint32_t>(button);
}

struct PlayerInputFrame {
    std::uint16_t version = player_input_version;
    std::uint64_t tick = 0;
    std::uint64_t sequence = 0;
    std::int16_t move_x = 0;
    std::int16_t move_z = 0;
    std::int16_t yaw_centidegrees = 0;
    std::int16_t pitch_centidegrees = 0;
    std::uint32_t held_buttons = 0;
    std::uint32_t pressed_buttons = 0;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] bool held(PlayerInputButton button) const noexcept;
    [[nodiscard]] bool pressed(PlayerInputButton button) const noexcept;
    friend bool operator==(const PlayerInputFrame&, const PlayerInputFrame&) = default;
};

class PlayerInputTextCodec {
  public:
    [[nodiscard]] static std::string encode(const PlayerInputFrame& input);
    [[nodiscard]] static core::Result<PlayerInputFrame> decode(std::string_view payload);
};

struct PlayerInputBindings {
    platform::KeyCode forward = platform::KeyCode::w;
    platform::KeyCode back = platform::KeyCode::s;
    platform::KeyCode left = platform::KeyCode::a;
    platform::KeyCode right = platform::KeyCode::d;
    platform::KeyCode jump = platform::KeyCode::space;
    platform::KeyCode sprint = platform::KeyCode::left_shift;
    platform::KeyCode crouch = platform::KeyCode::left_control;
    platform::KeyCode dash = platform::KeyCode::q;
    platform::KeyCode roll = platform::KeyCode::left_alt;
    platform::KeyCode interact = platform::KeyCode::e;
};

class PlayerInputSampler {
  public:
    explicit PlayerInputSampler(PlayerInputBindings bindings = {});

    [[nodiscard]] PlayerInputFrame sample(const platform::WindowInputSnapshot& snapshot,
                                          std::uint64_t tick, bool include_pressed = true);
    void set_look_sensitivity(double centidegrees_per_pixel) noexcept;
    void set_orientation(double yaw_centidegrees, double pitch_centidegrees) noexcept;

  private:
    PlayerInputBindings bindings_;
    std::uint64_t next_sequence_ = 1;
    double yaw_centidegrees_ = 0.0;
    double pitch_centidegrees_ = 0.0;
    double look_sensitivity_ = 12.0;
};

struct FixedStepPlayerInputFrame {
    simulation::FixedStepFrame fixed_step;
    std::vector<PlayerInputFrame> inputs;
};

// Collects render-frame input and emits exactly one movement input per simulation step. This
// prevents prediction speed and input queue depth from depending on the render frame rate.
class FixedStepPlayerInputScheduler {
  public:
    explicit FixedStepPlayerInputScheduler(simulation::FixedStepConfig fixed_step = {},
                                           PlayerInputBindings bindings = {});

    [[nodiscard]] core::Result<FixedStepPlayerInputFrame>
    advance(const platform::WindowInputSnapshot& snapshot, std::uint64_t frame_time_us,
            bool gameplay_enabled = true);
    void set_look_sensitivity(double centidegrees_per_pixel) noexcept;
    void set_orientation(double yaw_centidegrees, double pitch_centidegrees) noexcept;
    void reset(std::uint64_t tick = 0) noexcept;

  private:
    void accumulate(const platform::WindowInputSnapshot& snapshot, bool gameplay_enabled);

    simulation::FixedStepClock fixed_step_;
    PlayerInputSampler sampler_;
    std::vector<platform::KeyCode> down_keys_;
    std::vector<platform::KeyCode> pressed_keys_;
    std::int64_t mouse_delta_x_ = 0;
    std::int64_t mouse_delta_y_ = 0;
};

} // namespace heartstead::movement
