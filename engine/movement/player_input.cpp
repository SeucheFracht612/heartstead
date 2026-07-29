#include "engine/movement/player_input.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace heartstead::movement {

namespace {

constexpr std::uint32_t known_buttons =
    input_button_bit(PlayerInputButton::jump) | input_button_bit(PlayerInputButton::sprint) |
    input_button_bit(PlayerInputButton::crouch) | input_button_bit(PlayerInputButton::dash) |
    input_button_bit(PlayerInputButton::roll) | input_button_bit(PlayerInputButton::interact);

[[nodiscard]] bool contains(const std::vector<platform::KeyCode>& values,
                            platform::KeyCode key) noexcept {
    return std::ranges::find(values, key) != values.end();
}

template <typename T>
[[nodiscard]] core::Result<T> parse_integer(std::string_view text, std::string_view field) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return core::Result<T>::failure("player_input.invalid_number",
                                        "player input field is invalid: " + std::string(field));
    }
    return core::Result<T>::success(value);
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t first = 0;
    while (first <= text.size()) {
        const auto last = text.find('|', first);
        parts.push_back(text.substr(first, last == std::string_view::npos ? text.size() - first
                                                                          : last - first));
        if (last == std::string_view::npos) {
            break;
        }
        first = last + 1;
    }
    return parts;
}

[[nodiscard]] std::int16_t clamp_angle(double value, double minimum, double maximum) noexcept {
    return static_cast<std::int16_t>(std::lround(std::clamp(value, minimum, maximum)));
}

[[nodiscard]] std::int32_t clamp_mouse_delta(std::int64_t value) noexcept {
    return static_cast<std::int32_t>(
        std::clamp(value, static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
                   static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
}

void append_unique_keys(std::vector<platform::KeyCode>& destination,
                        const std::vector<platform::KeyCode>& source) {
    for (const auto key : source) {
        if (!contains(destination, key)) {
            destination.push_back(key);
        }
    }
}

} // namespace

core::Status PlayerInputFrame::validate() const {
    if (version != player_input_version) {
        return core::Status::failure("player_input.unsupported_version",
                                     "player input version is not supported");
    }
    if (tick == 0 || sequence == 0) {
        return core::Status::failure("player_input.invalid_identity",
                                     "player input tick and sequence must be non-zero");
    }
    if (move_x == std::numeric_limits<std::int16_t>::min() ||
        move_z == std::numeric_limits<std::int16_t>::min()) {
        return core::Status::failure("player_input.invalid_axis",
                                     "player input axes must be normalized signed values");
    }
    if (pitch_centidegrees < -8'900 || pitch_centidegrees > 8'900) {
        return core::Status::failure("player_input.invalid_pitch",
                                     "player input pitch must be between -89 and 89 degrees");
    }
    if (((held_buttons | pressed_buttons) & ~known_buttons) != 0) {
        return core::Status::failure("player_input.unknown_button",
                                     "player input contains unknown button bits");
    }
    if ((pressed_buttons & ~held_buttons) != 0) {
        return core::Status::failure("player_input.invalid_pressed_button",
                                     "pressed player input buttons must also be held");
    }
    return core::Status::ok();
}

bool PlayerInputFrame::held(PlayerInputButton button) const noexcept {
    return (held_buttons & input_button_bit(button)) != 0;
}

bool PlayerInputFrame::pressed(PlayerInputButton button) const noexcept {
    return (pressed_buttons & input_button_bit(button)) != 0;
}

std::string PlayerInputTextCodec::encode(const PlayerInputFrame& input) {
    return std::to_string(input.version) + '|' + std::to_string(input.tick) + '|' +
           std::to_string(input.sequence) + '|' + std::to_string(input.move_x) + '|' +
           std::to_string(input.move_z) + '|' + std::to_string(input.yaw_centidegrees) + '|' +
           std::to_string(input.pitch_centidegrees) + '|' + std::to_string(input.held_buttons) +
           '|' + std::to_string(input.pressed_buttons);
}

core::Result<PlayerInputFrame> PlayerInputTextCodec::decode(std::string_view payload) {
    if (payload.empty() || payload.size() > 256) {
        return core::Result<PlayerInputFrame>::failure("player_input.invalid_payload_size",
                                                       "player input payload size is invalid");
    }
    const auto parts = split(payload);
    if (parts.size() != 9) {
        return core::Result<PlayerInputFrame>::failure("player_input.invalid_payload",
                                                       "player input payload must have 9 fields");
    }
    auto version = parse_integer<std::uint16_t>(parts[0], "version");
    auto tick = parse_integer<std::uint64_t>(parts[1], "tick");
    auto sequence = parse_integer<std::uint64_t>(parts[2], "sequence");
    auto move_x = parse_integer<std::int16_t>(parts[3], "move_x");
    auto move_z = parse_integer<std::int16_t>(parts[4], "move_z");
    auto yaw = parse_integer<std::int16_t>(parts[5], "yaw");
    auto pitch = parse_integer<std::int16_t>(parts[6], "pitch");
    auto held = parse_integer<std::uint32_t>(parts[7], "held_buttons");
    auto pressed = parse_integer<std::uint32_t>(parts[8], "pressed_buttons");
    if (!version || !tick || !sequence || !move_x || !move_z || !yaw || !pitch || !held ||
        !pressed) {
        const auto* error = !version    ? &version.error()
                            : !tick     ? &tick.error()
                            : !sequence ? &sequence.error()
                            : !move_x   ? &move_x.error()
                            : !move_z   ? &move_z.error()
                            : !yaw      ? &yaw.error()
                            : !pitch    ? &pitch.error()
                            : !held     ? &held.error()
                                        : &pressed.error();
        return core::Result<PlayerInputFrame>::failure(error->code, error->message);
    }

    PlayerInputFrame result{version.value(), tick.value(),   sequence.value(),
                            move_x.value(),  move_z.value(), yaw.value(),
                            pitch.value(),   held.value(),   pressed.value()};
    auto status = result.validate();
    if (!status) {
        return core::Result<PlayerInputFrame>::failure(status.error().code, status.error().message);
    }
    return core::Result<PlayerInputFrame>::success(result);
}

PlayerInputSampler::PlayerInputSampler(PlayerInputBindings bindings) : bindings_(bindings) {}

PlayerInputFrame PlayerInputSampler::sample(const platform::WindowInputSnapshot& snapshot,
                                            std::uint64_t tick, bool include_pressed) {
    const auto axis = [&snapshot](platform::KeyCode positive, platform::KeyCode negative) {
        return static_cast<std::int32_t>(contains(snapshot.down_keys, positive)) -
               static_cast<std::int32_t>(contains(snapshot.down_keys, negative));
    };
    auto x = axis(bindings_.right, bindings_.left);
    auto z = axis(bindings_.forward, bindings_.back);
    if (x != 0 && z != 0) {
        constexpr auto diagonal = 23'170;
        x *= diagonal;
        z *= diagonal;
    } else {
        x *= 32'767;
        z *= 32'767;
    }

    // The renderer's view convention has camera-right opposite world +X while yaw zero faces
    // world +Z. A rightward mouse motion must therefore decrease player yaw.
    const auto yaw_delta = -static_cast<double>(snapshot.mouse_delta_x) * look_sensitivity_;
    const auto pitch_delta = static_cast<double>(snapshot.mouse_delta_y) * look_sensitivity_;
    const auto next_yaw = yaw_centidegrees_ + yaw_delta;
    const auto next_pitch = pitch_centidegrees_ - pitch_delta;
    if (std::isfinite(next_yaw)) {
        yaw_centidegrees_ = std::remainder(next_yaw, 36'000.0);
    }
    if (std::isfinite(next_pitch)) {
        pitch_centidegrees_ = std::clamp(next_pitch, -8'900.0, 8'900.0);
    }

    PlayerInputFrame frame;
    frame.tick = tick;
    frame.sequence = next_sequence_++;
    frame.move_x = static_cast<std::int16_t>(x);
    frame.move_z = static_cast<std::int16_t>(z);
    frame.yaw_centidegrees = clamp_angle(yaw_centidegrees_, -18'000.0, 18'000.0);
    frame.pitch_centidegrees = clamp_angle(pitch_centidegrees_, -8'900.0, 8'900.0);

    const std::array pairs{
        std::pair{bindings_.jump, PlayerInputButton::jump},
        std::pair{bindings_.sprint, PlayerInputButton::sprint},
        std::pair{bindings_.crouch, PlayerInputButton::crouch},
        std::pair{bindings_.dash, PlayerInputButton::dash},
        std::pair{bindings_.roll, PlayerInputButton::roll},
        std::pair{bindings_.interact, PlayerInputButton::interact},
    };
    for (const auto& [key, button] : pairs) {
        if (contains(snapshot.down_keys, key)) {
            frame.held_buttons |= input_button_bit(button);
        }
        if (include_pressed && contains(snapshot.pressed_keys, key)) {
            frame.held_buttons |= input_button_bit(button);
            frame.pressed_buttons |= input_button_bit(button);
        }
    }
    return frame;
}

void PlayerInputSampler::set_look_sensitivity(double centidegrees_per_pixel) noexcept {
    if (std::isfinite(centidegrees_per_pixel) && centidegrees_per_pixel > 0.0 &&
        centidegrees_per_pixel <= max_player_look_sensitivity_centidegrees_per_pixel) {
        look_sensitivity_ = centidegrees_per_pixel;
    }
}

void PlayerInputSampler::set_orientation(double yaw_centidegrees,
                                         double pitch_centidegrees) noexcept {
    if (!std::isfinite(yaw_centidegrees) || !std::isfinite(pitch_centidegrees)) {
        return;
    }
    yaw_centidegrees_ = std::remainder(yaw_centidegrees, 36'000.0);
    pitch_centidegrees_ = std::clamp(pitch_centidegrees, -8'900.0, 8'900.0);
}

FixedStepPlayerInputScheduler::FixedStepPlayerInputScheduler(
    simulation::FixedStepConfig fixed_step, PlayerInputBindings bindings)
    : fixed_step_(fixed_step), sampler_(bindings) {}

core::Result<FixedStepPlayerInputFrame>
FixedStepPlayerInputScheduler::advance(const platform::WindowInputSnapshot& snapshot,
                                       std::uint64_t frame_time_us, bool gameplay_enabled) {
    accumulate(snapshot, gameplay_enabled);
    auto fixed_frame = fixed_step_.advance(frame_time_us);
    if (!fixed_frame) {
        return core::Result<FixedStepPlayerInputFrame>::failure(fixed_frame.error().code,
                                                                fixed_frame.error().message);
    }

    FixedStepPlayerInputFrame result;
    result.fixed_step = fixed_frame.value();
    result.inputs.reserve(result.fixed_step.step_count);
    for (std::uint32_t step = 0; step < result.fixed_step.step_count; ++step) {
        platform::WindowInputSnapshot sampled;
        sampled.window_id = snapshot.window_id;
        sampled.down_keys = down_keys_;
        if (step == 0) {
            sampled.pressed_keys = pressed_keys_;
            sampled.mouse_delta_x = clamp_mouse_delta(mouse_delta_x_);
            sampled.mouse_delta_y = clamp_mouse_delta(mouse_delta_y_);
        }
        result.inputs.push_back(
            sampler_.sample(sampled, result.fixed_step.first_tick + step, step == 0));
    }
    if (result.fixed_step.step_count > 0) {
        pressed_keys_.clear();
        mouse_delta_x_ = 0;
        mouse_delta_y_ = 0;
    }
    return core::Result<FixedStepPlayerInputFrame>::success(std::move(result));
}

void FixedStepPlayerInputScheduler::set_look_sensitivity(
    double centidegrees_per_pixel) noexcept {
    sampler_.set_look_sensitivity(centidegrees_per_pixel);
}

void FixedStepPlayerInputScheduler::set_orientation(double yaw_centidegrees,
                                                    double pitch_centidegrees) noexcept {
    sampler_.set_orientation(yaw_centidegrees, pitch_centidegrees);
}

void FixedStepPlayerInputScheduler::accumulate(
    const platform::WindowInputSnapshot& snapshot, bool gameplay_enabled) {
    if (!gameplay_enabled) {
        down_keys_.clear();
        pressed_keys_.clear();
        mouse_delta_x_ = 0;
        mouse_delta_y_ = 0;
        return;
    }
    down_keys_ = snapshot.down_keys;
    append_unique_keys(pressed_keys_, snapshot.pressed_keys);
    const auto saturating_add = [](std::int64_t current, std::int32_t delta) {
        if (delta > 0 && current > std::numeric_limits<std::int64_t>::max() - delta) {
            return std::numeric_limits<std::int64_t>::max();
        }
        if (delta < 0 && current < std::numeric_limits<std::int64_t>::min() - delta) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return current + delta;
    };
    mouse_delta_x_ = saturating_add(mouse_delta_x_, snapshot.mouse_delta_x);
    mouse_delta_y_ = saturating_add(mouse_delta_y_, snapshot.mouse_delta_y);
}

} // namespace heartstead::movement
