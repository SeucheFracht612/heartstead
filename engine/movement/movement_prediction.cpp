#include "engine/movement/movement_prediction.hpp"

#include "engine/net/binary_message.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

namespace heartstead::movement {

namespace {

template <typename T>
[[nodiscard]] core::Result<T> parse_number(std::string_view text, std::string_view field) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return core::Result<T>::failure("movement_snapshot.invalid_number",
                                        "movement snapshot field is invalid: " +
                                            std::string(field));
    }
    return core::Result<T>::success(value);
}

template <>
core::Result<double> parse_number<double>(std::string_view text, std::string_view field) {
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        return core::Result<double>::failure("movement_snapshot.invalid_number",
                                             "movement snapshot field is invalid: " +
                                                 std::string(field));
    }
    return core::Result<double>::success(value);
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view text) {
    std::vector<std::string_view> result;
    std::size_t first = 0;
    while (first <= text.size()) {
        const auto last = text.find('|', first);
        result.push_back(text.substr(first, last == std::string_view::npos ? text.size() - first
                                                                           : last - first));
        if (last == std::string_view::npos) {
            break;
        }
        first = last + 1;
    }
    return result;
}

[[nodiscard]] double position_distance(const world::WorldPosition& lhs,
                                       const world::WorldPosition& rhs) noexcept {
    return math::length(lhs.relative_to(rhs.anchor) - rhs.local_offset);
}

void write_binary_position(net::BinaryMessageWriter& writer, const world::WorldPosition& position) {
    writer.i64(position.anchor.x);
    writer.i64(position.anchor.y);
    writer.i64(position.anchor.z);
    writer.f64(position.local_offset.x);
    writer.f64(position.local_offset.y);
    writer.f64(position.local_offset.z);
}

[[nodiscard]] bool read_binary_position(net::BinaryMessageReader& reader,
                                        world::WorldPosition& position) {
    world::BlockCoord anchor;
    math::Vec3d local;
    if (!reader.i64(anchor.x) || !reader.i64(anchor.y) || !reader.i64(anchor.z) ||
        !reader.f64(local.x) || !reader.f64(local.y) || !reader.f64(local.z)) {
        return false;
    }
    auto decoded = world::WorldPosition::from_anchor(anchor, local);
    if (!decoded) {
        return false;
    }
    position = decoded.value();
    return true;
}

} // namespace

core::Status PlayerInputBundle::validate() const {
    if (frames.empty() || frames.size() > 4) {
        return core::Status::failure("movement_input_bundle.invalid_count",
                                     "movement input bundle must contain between 1 and 4 frames");
    }
    std::uint64_t previous_sequence = 0;
    std::uint64_t previous_tick = 0;
    for (const auto& frame : frames) {
        auto status = frame.validate();
        if (!status) {
            return status;
        }
        if (frame.sequence <= previous_sequence || frame.tick <= previous_tick) {
            return core::Status::failure("movement_input_bundle.out_of_order",
                                         "movement input bundle frames must be increasing");
        }
        previous_sequence = frame.sequence;
        previous_tick = frame.tick;
    }
    return core::Status::ok();
}

std::string PlayerInputBundleTextCodec::encode(const PlayerInputBundle& bundle) {
    std::string result;
    for (std::size_t index = 0; index < bundle.frames.size(); ++index) {
        if (index > 0) {
            result.push_back(';');
        }
        result += PlayerInputTextCodec::encode(bundle.frames[index]);
    }
    return result;
}

core::Result<PlayerInputBundle> PlayerInputBundleTextCodec::decode(std::string_view payload) {
    if (payload.empty() || payload.size() > 1024) {
        return core::Result<PlayerInputBundle>::failure(
            "movement_input_bundle.invalid_payload_size",
            "movement input bundle payload size is invalid");
    }
    PlayerInputBundle bundle;
    std::size_t first = 0;
    while (first <= payload.size()) {
        const auto last = payload.find(';', first);
        auto frame = PlayerInputTextCodec::decode(payload.substr(
            first, last == std::string_view::npos ? payload.size() - first : last - first));
        if (!frame) {
            return core::Result<PlayerInputBundle>::failure(frame.error().code,
                                                            frame.error().message);
        }
        bundle.frames.push_back(frame.value());
        if (bundle.frames.size() > 4 || last == std::string_view::npos) {
            break;
        }
        first = last + 1;
    }
    auto status = bundle.validate();
    if (!status) {
        return core::Result<PlayerInputBundle>::failure(status.error().code,
                                                        status.error().message);
    }
    return core::Result<PlayerInputBundle>::success(std::move(bundle));
}

std::string PlayerInputBundleBinaryCodec::encode(const PlayerInputBundle& bundle) {
    net::BinaryMessageWriter writer;
    writer.u32(0x3142494dU);
    writer.u8(static_cast<std::uint8_t>(bundle.frames.size()));
    for (const auto& frame : bundle.frames) {
        writer.var_u64(frame.version);
        writer.var_u64(frame.tick);
        writer.var_u64(frame.sequence);
        writer.var_i64(frame.move_x);
        writer.var_i64(frame.move_z);
        writer.var_i64(frame.yaw_centidegrees);
        writer.var_i64(frame.pitch_centidegrees);
        writer.var_u64(frame.held_buttons);
        writer.var_u64(frame.pressed_buttons);
    }
    return writer.take();
}

core::Result<PlayerInputBundle> PlayerInputBundleBinaryCodec::decode(std::string_view payload) {
    net::BinaryMessageReader reader(payload);
    std::uint32_t magic = 0;
    std::uint8_t count = 0;
    if (!reader.u32(magic) || magic != 0x3142494dU || !reader.u8(count) || count == 0 ||
        count > 4) {
        return core::Result<PlayerInputBundle>::failure(
            "movement_input_bundle.invalid_binary_header",
            "movement input bundle binary header is invalid");
    }
    PlayerInputBundle bundle;
    bundle.frames.resize(count);
    for (auto& frame : bundle.frames) {
        std::uint64_t version = 0;
        std::uint64_t held_buttons = 0;
        std::uint64_t pressed_buttons = 0;
        std::int64_t move_x = 0;
        std::int64_t move_z = 0;
        std::int64_t yaw = 0;
        std::int64_t pitch = 0;
        if (!reader.var_u64(version) || !reader.var_u64(frame.tick) ||
            !reader.var_u64(frame.sequence) || !reader.var_i64(move_x) || !reader.var_i64(move_z) ||
            !reader.var_i64(yaw) || !reader.var_i64(pitch) || !reader.var_u64(held_buttons) ||
            !reader.var_u64(pressed_buttons) ||
            version > std::numeric_limits<std::uint16_t>::max() ||
            move_x < std::numeric_limits<std::int16_t>::min() ||
            move_x > std::numeric_limits<std::int16_t>::max() ||
            move_z < std::numeric_limits<std::int16_t>::min() ||
            move_z > std::numeric_limits<std::int16_t>::max() ||
            yaw < std::numeric_limits<std::int16_t>::min() ||
            yaw > std::numeric_limits<std::int16_t>::max() ||
            pitch < std::numeric_limits<std::int16_t>::min() ||
            pitch > std::numeric_limits<std::int16_t>::max() ||
            held_buttons > std::numeric_limits<std::uint32_t>::max() ||
            pressed_buttons > std::numeric_limits<std::uint32_t>::max()) {
            return core::Result<PlayerInputBundle>::failure(
                "movement_input_bundle.truncated_binary",
                "movement input bundle binary payload is truncated");
        }
        frame.version = static_cast<std::uint16_t>(version);
        frame.move_x = static_cast<std::int16_t>(move_x);
        frame.move_z = static_cast<std::int16_t>(move_z);
        frame.yaw_centidegrees = static_cast<std::int16_t>(yaw);
        frame.pitch_centidegrees = static_cast<std::int16_t>(pitch);
        frame.held_buttons = static_cast<std::uint32_t>(held_buttons);
        frame.pressed_buttons = static_cast<std::uint32_t>(pressed_buttons);
    }
    if (!reader.finished()) {
        return core::Result<PlayerInputBundle>::failure(
            "movement_input_bundle.trailing_binary",
            "movement input bundle binary payload has trailing bytes");
    }
    auto status = bundle.validate();
    if (!status) {
        return core::Result<PlayerInputBundle>::failure(status.error().code,
                                                        status.error().message);
    }
    return core::Result<PlayerInputBundle>::success(std::move(bundle));
}

core::Status PlayerControllerSnapshot::validate(const PlayerMovementConfig& config) const {
    if (version != player_controller_snapshot_version || !player_net_id.is_valid() ||
        !player_save_id.is_valid() || last_processed_input_sequence != state.last_input_sequence) {
        return core::Status::failure("movement_snapshot.invalid_identity",
                                     "movement snapshot identity or acknowledgement is invalid");
    }
    return state.validate(config);
}

std::string PlayerControllerSnapshotTextCodec::encode(const PlayerControllerSnapshot& snapshot) {
    const auto& state = snapshot.state;
    std::ostringstream output;
    output.precision(17);
    output << snapshot.version << '|' << snapshot.player_net_id.value() << '|'
           << snapshot.player_save_id.value() << '|' << snapshot.last_processed_input_sequence
           << '|' << snapshot.collision_world_revision << '|' << state.position.anchor.x << '|'
           << state.position.anchor.y << '|' << state.position.anchor.z << '|'
           << state.position.local_offset.x << '|' << state.position.local_offset.y << '|'
           << state.position.local_offset.z << '|' << state.velocity.x << '|' << state.velocity.y
           << '|' << state.velocity.z << '|' << static_cast<unsigned>(state.mode) << '|'
           << static_cast<unsigned>(state.scripted_kind) << '|'
           << static_cast<unsigned>(state.encumbrance) << '|' << state.crouched << '|'
           << state.grounded << '|' << state.exhausted << '|' << state.invulnerable << '|'
           << state.yaw_centidegrees << '|' << state.pitch_centidegrees << '|' << state.health_milli
           << '|' << state.stamina_milli << '|' << static_cast<unsigned>(state.dash_charges) << '|'
           << state.air_dash_available << '|' << state.mode_ticks << '|'
           << state.mode_duration_ticks << '|' << state.jump_buffer_ticks << '|'
           << state.coyote_ticks << '|' << state.roll_buffer_ticks << '|'
           << state.stamina_regen_delay_ticks << '|' << state.landing_roll_window_ticks << '|'
           << static_cast<unsigned>(state.stamina_drain_remainder) << '|'
           << static_cast<unsigned>(state.stamina_regen_remainder) << '|'
           << (state.pending_fall_distance.has_value() ? 1 : 0) << '|'
           << state.pending_fall_distance.value_or(0.0) << '|' << state.fall_origin.anchor.x << '|'
           << state.fall_origin.anchor.y << '|' << state.fall_origin.anchor.z << '|'
           << state.fall_origin.local_offset.x << '|' << state.fall_origin.local_offset.y << '|'
           << state.fall_origin.local_offset.z << '|' << state.scripted_direction.x << '|'
           << state.scripted_direction.y << '|' << state.scripted_direction.z << '|'
           << state.scripted_start.anchor.x << '|' << state.scripted_start.anchor.y << '|'
           << state.scripted_start.anchor.z << '|' << state.scripted_start.local_offset.x << '|'
           << state.scripted_start.local_offset.y << '|' << state.scripted_start.local_offset.z
           << '|' << state.scripted_target.anchor.x << '|' << state.scripted_target.anchor.y << '|'
           << state.scripted_target.anchor.z << '|' << state.scripted_target.local_offset.x << '|'
           << state.scripted_target.local_offset.y << '|' << state.scripted_target.local_offset.z
           << '|' << static_cast<unsigned>(state.locomotion_animation.kind) << '|'
           << state.locomotion_animation.phase << '|'
           << static_cast<unsigned>(state.locomotion_animation.transition_from) << '|'
           << state.locomotion_animation.transition_from_phase << '|'
           << state.locomotion_animation.transition_tick << '|' << state.simulation_tick;
    return output.str();
}

core::Result<PlayerControllerSnapshot>
PlayerControllerSnapshotTextCodec::decode(std::string_view payload,
                                          const PlayerMovementConfig& config) {
    if (payload.empty() || payload.size() > 4096) {
        return core::Result<PlayerControllerSnapshot>::failure(
            "movement_snapshot.invalid_payload_size", "movement snapshot payload size is invalid");
    }
    const auto fields = split(payload);
    if (fields.size() != 65) {
        return core::Result<PlayerControllerSnapshot>::failure(
            "movement_snapshot.invalid_payload", "movement snapshot must contain 65 fields");
    }
    std::size_t index = 0;
    const auto next_u64 = [&](std::string_view name) {
        return parse_number<std::uint64_t>(fields[index++], name);
    };
    const auto next_i64 = [&](std::string_view name) {
        return parse_number<std::int64_t>(fields[index++], name);
    };
    const auto next_i32 = [&](std::string_view name) {
        return parse_number<std::int32_t>(fields[index++], name);
    };
    const auto next_u32 = [&](std::string_view name) {
        return parse_number<std::uint32_t>(fields[index++], name);
    };
    const auto next_u16 = [&](std::string_view name) {
        return parse_number<std::uint16_t>(fields[index++], name);
    };
    const auto next_i16 = [&](std::string_view name) {
        return parse_number<std::int16_t>(fields[index++], name);
    };
    const auto next_double = [&](std::string_view name) {
        return parse_number<double>(fields[index++], name);
    };

    // Decode into wide temporaries so every conversion remains checked by the parser.
    auto version = next_u16("version");
    auto net_id = next_u64("net_id");
    auto save_id = next_u64("save_id");
    auto ack = next_u64("ack");
    auto revision = next_u64("revision");
    auto px = next_i64("position_x");
    auto py = next_i64("position_y");
    auto pz = next_i64("position_z");
    auto plx = next_double("position_local_x");
    auto ply = next_double("position_local_y");
    auto plz = next_double("position_local_z");
    auto vx = next_double("velocity_x");
    auto vy = next_double("velocity_y");
    auto vz = next_double("velocity_z");
    auto mode = next_u32("mode");
    auto scripted = next_u32("scripted_kind");
    auto encumbrance = next_u32("encumbrance");
    auto crouched = next_u32("crouched");
    auto grounded = next_u32("grounded");
    auto exhausted = next_u32("exhausted");
    auto invulnerable = next_u32("invulnerable");
    auto yaw = next_i16("yaw");
    auto pitch = next_i16("pitch");
    auto health = next_i32("health");
    auto stamina = next_i32("stamina");
    auto charges = next_u32("dash_charges");
    auto air_dash = next_u32("air_dash");
    auto mode_ticks = next_u16("mode_ticks");
    auto mode_duration = next_u16("mode_duration");
    auto jump_buffer = next_u16("jump_buffer");
    auto coyote = next_u16("coyote");
    auto roll_buffer = next_u16("roll_buffer");
    auto regen_delay = next_u16("regen_delay");
    auto landing_window = next_u16("landing_window");
    auto drain_remainder = next_u32("drain_remainder");
    auto regen_remainder = next_u32("regen_remainder");
    auto has_fall = next_u32("has_fall");
    auto fall_distance = next_double("fall_distance");
    auto fox = next_i64("fall_x");
    auto foy = next_i64("fall_y");
    auto foz = next_i64("fall_z");
    auto folx = next_double("fall_local_x");
    auto foly = next_double("fall_local_y");
    auto folz = next_double("fall_local_z");
    auto sdx = next_double("scripted_direction_x");
    auto sdy = next_double("scripted_direction_y");
    auto sdz = next_double("scripted_direction_z");
    auto ssx = next_i64("scripted_start_x");
    auto ssy = next_i64("scripted_start_y");
    auto ssz = next_i64("scripted_start_z");
    auto sslx = next_double("scripted_start_local_x");
    auto ssly = next_double("scripted_start_local_y");
    auto sslz = next_double("scripted_start_local_z");
    auto stx = next_i64("scripted_target_x");
    auto sty = next_i64("scripted_target_y");
    auto stz = next_i64("scripted_target_z");
    auto stlx = next_double("scripted_target_local_x");
    auto stly = next_double("scripted_target_local_y");
    auto stlz = next_double("scripted_target_local_z");
    auto locomotion_kind = next_u32("locomotion_kind");
    auto locomotion_phase = next_u16("locomotion_phase");
    auto locomotion_from = next_u32("locomotion_transition_from");
    auto locomotion_from_phase = next_u16("locomotion_transition_from_phase");
    auto locomotion_transition_tick = next_u64("locomotion_transition_tick");
    auto tick = next_u64("tick");

    const bool parsed =
        version && net_id && save_id && ack && revision && px && py && pz && plx && ply && plz &&
        vx && vy && vz && mode && scripted && encumbrance && crouched && grounded && exhausted &&
        invulnerable && yaw && pitch && health && stamina && charges && air_dash && mode_ticks &&
        mode_duration && jump_buffer && coyote && roll_buffer && regen_delay && landing_window &&
        drain_remainder && regen_remainder && has_fall && fall_distance && fox && foy && foz &&
        folx && foly && folz && sdx && sdy && sdz && ssx && ssy && ssz && sslx && ssly && sslz &&
        stx && sty && stz && stlx && stly && stlz && locomotion_kind && locomotion_phase &&
        locomotion_from && locomotion_from_phase && locomotion_transition_tick && tick;
    if (!parsed || mode.value() > static_cast<std::uint32_t>(PlayerControllerMode::swimming) ||
        scripted.value() > static_cast<std::uint32_t>(ScriptedMovementKind::mantle) ||
        encumbrance.value() > static_cast<std::uint32_t>(EncumbranceTier::over_encumbered) ||
        crouched.value() > 1 || grounded.value() > 1 || exhausted.value() > 1 ||
        invulnerable.value() > 1 || air_dash.value() > 1 || has_fall.value() > 1 ||
        charges.value() > 2 || drain_remainder.value() >= 60 || regen_remainder.value() >= 60 ||
        locomotion_kind.value() >
            static_cast<std::uint32_t>(animation::LocomotionAnimationKind::fall) ||
        locomotion_from.value() >
            static_cast<std::uint32_t>(animation::LocomotionAnimationKind::fall)) {
        return core::Result<PlayerControllerSnapshot>::failure(
            "movement_snapshot.invalid_payload", "movement snapshot fields are invalid");
    }

    auto position = world::WorldPosition::from_anchor({px.value(), py.value(), pz.value()},
                                                      {plx.value(), ply.value(), plz.value()});
    auto fall_origin = world::WorldPosition::from_anchor(
        {fox.value(), foy.value(), foz.value()}, {folx.value(), foly.value(), folz.value()});
    auto scripted_start_position = world::WorldPosition::from_anchor(
        {ssx.value(), ssy.value(), ssz.value()}, {sslx.value(), ssly.value(), sslz.value()});
    auto scripted_target_position = world::WorldPosition::from_anchor(
        {stx.value(), sty.value(), stz.value()}, {stlx.value(), stly.value(), stlz.value()});
    if (!position || !fall_origin || !scripted_start_position || !scripted_target_position) {
        return core::Result<PlayerControllerSnapshot>::failure(
            "movement_snapshot.invalid_position", "movement snapshot positions are invalid");
    }

    PlayerControllerSnapshot snapshot;
    snapshot.version = version.value();
    snapshot.player_net_id = core::NetId::from_value(net_id.value());
    snapshot.player_save_id = core::SaveId::from_value(save_id.value());
    snapshot.last_processed_input_sequence = ack.value();
    snapshot.collision_world_revision = revision.value();
    auto& state = snapshot.state;
    state.position = position.value();
    state.velocity = {vx.value(), vy.value(), vz.value()};
    state.mode = static_cast<PlayerControllerMode>(mode.value());
    state.scripted_kind = static_cast<ScriptedMovementKind>(scripted.value());
    state.encumbrance = static_cast<EncumbranceTier>(encumbrance.value());
    state.crouched = crouched.value() != 0;
    state.grounded = grounded.value() != 0;
    state.exhausted = exhausted.value() != 0;
    state.invulnerable = invulnerable.value() != 0;
    state.yaw_centidegrees = yaw.value();
    state.pitch_centidegrees = pitch.value();
    state.health_milli = health.value();
    state.stamina_milli = stamina.value();
    state.dash_charges = static_cast<std::uint8_t>(charges.value());
    state.air_dash_available = air_dash.value() != 0;
    state.mode_ticks = mode_ticks.value();
    state.mode_duration_ticks = mode_duration.value();
    state.jump_buffer_ticks = jump_buffer.value();
    state.coyote_ticks = coyote.value();
    state.roll_buffer_ticks = roll_buffer.value();
    state.stamina_regen_delay_ticks = regen_delay.value();
    state.landing_roll_window_ticks = landing_window.value();
    state.stamina_drain_remainder = static_cast<std::uint8_t>(drain_remainder.value());
    state.stamina_regen_remainder = static_cast<std::uint8_t>(regen_remainder.value());
    if (has_fall.value() != 0) {
        state.pending_fall_distance = fall_distance.value();
    }
    state.fall_origin = fall_origin.value();
    state.scripted_direction = {sdx.value(), sdy.value(), sdz.value()};
    state.scripted_start = scripted_start_position.value();
    state.scripted_target = scripted_target_position.value();
    state.locomotion_animation.kind =
        static_cast<animation::LocomotionAnimationKind>(locomotion_kind.value());
    state.locomotion_animation.phase = locomotion_phase.value();
    state.locomotion_animation.transition_from =
        static_cast<animation::LocomotionAnimationKind>(locomotion_from.value());
    state.locomotion_animation.transition_from_phase = locomotion_from_phase.value();
    state.locomotion_animation.transition_tick = locomotion_transition_tick.value();
    state.last_input_sequence = ack.value();
    state.simulation_tick = tick.value();

    auto status = snapshot.validate(config);
    if (!status) {
        return core::Result<PlayerControllerSnapshot>::failure(status.error().code,
                                                               status.error().message);
    }
    return core::Result<PlayerControllerSnapshot>::success(std::move(snapshot));
}

std::string PlayerControllerSnapshotBinaryCodec::encode(const PlayerControllerSnapshot& snapshot) {
    net::BinaryMessageWriter writer;
    writer.u32(0x3153504dU);
    writer.u16(snapshot.version);
    writer.u64(snapshot.player_net_id.value());
    writer.u64(snapshot.player_save_id.value());
    writer.u64(snapshot.last_processed_input_sequence);
    writer.u64(snapshot.collision_world_revision);
    const auto& state = snapshot.state;
    write_binary_position(writer, state.position);
    writer.f64(state.velocity.x);
    writer.f64(state.velocity.y);
    writer.f64(state.velocity.z);
    writer.u8(static_cast<std::uint8_t>(state.mode));
    writer.u8(static_cast<std::uint8_t>(state.scripted_kind));
    writer.u8(static_cast<std::uint8_t>(state.encumbrance));
    writer.boolean(state.crouched);
    writer.boolean(state.grounded);
    writer.boolean(state.exhausted);
    writer.boolean(state.invulnerable);
    writer.i16(state.yaw_centidegrees);
    writer.i16(state.pitch_centidegrees);
    writer.i32(state.health_milli);
    writer.i32(state.stamina_milli);
    writer.u8(state.dash_charges);
    writer.boolean(state.air_dash_available);
    writer.u16(state.mode_ticks);
    writer.u16(state.mode_duration_ticks);
    writer.u16(state.jump_buffer_ticks);
    writer.u16(state.coyote_ticks);
    writer.u16(state.roll_buffer_ticks);
    writer.u16(state.stamina_regen_delay_ticks);
    writer.u16(state.landing_roll_window_ticks);
    writer.u8(state.stamina_drain_remainder);
    writer.u8(state.stamina_regen_remainder);
    writer.boolean(state.pending_fall_distance.has_value());
    writer.f64(state.pending_fall_distance.value_or(0.0));
    if (state.pending_fall_distance.has_value()) {
        write_binary_position(writer, state.fall_origin);
    }
    if (state.scripted_kind != ScriptedMovementKind::none) {
        writer.f64(state.scripted_direction.x);
        writer.f64(state.scripted_direction.y);
        writer.f64(state.scripted_direction.z);
        write_binary_position(writer, state.scripted_start);
        write_binary_position(writer, state.scripted_target);
    }
    writer.u8(static_cast<std::uint8_t>(state.locomotion_animation.kind));
    writer.u16(state.locomotion_animation.phase);
    writer.u8(static_cast<std::uint8_t>(state.locomotion_animation.transition_from));
    writer.u16(state.locomotion_animation.transition_from_phase);
    writer.u64(state.locomotion_animation.transition_tick);
    writer.u64(state.simulation_tick);
    return writer.take();
}

core::Result<PlayerControllerSnapshot>
PlayerControllerSnapshotBinaryCodec::decode(std::string_view payload,
                                            const PlayerMovementConfig& config) {
    net::BinaryMessageReader reader(payload);
    std::uint32_t magic = 0;
    PlayerControllerSnapshot snapshot;
    std::uint64_t player_net_id = 0;
    std::uint64_t player_save_id = 0;
    auto& state = snapshot.state;
    std::uint8_t mode = 0;
    std::uint8_t scripted_kind = 0;
    std::uint8_t encumbrance = 0;
    std::uint8_t locomotion_kind = 0;
    std::uint8_t locomotion_from = 0;
    bool has_fall_distance = false;
    double fall_distance = 0.0;
    if (!reader.u32(magic) || magic != 0x3153504dU || !reader.u16(snapshot.version) ||
        !reader.u64(player_net_id) || !reader.u64(player_save_id) ||
        !reader.u64(snapshot.last_processed_input_sequence) ||
        !reader.u64(snapshot.collision_world_revision) ||
        !read_binary_position(reader, state.position) || !reader.f64(state.velocity.x) ||
        !reader.f64(state.velocity.y) || !reader.f64(state.velocity.z) || !reader.u8(mode) ||
        !reader.u8(scripted_kind) || !reader.u8(encumbrance) || !reader.boolean(state.crouched) ||
        !reader.boolean(state.grounded) || !reader.boolean(state.exhausted) ||
        !reader.boolean(state.invulnerable) || !reader.i16(state.yaw_centidegrees) ||
        !reader.i16(state.pitch_centidegrees) || !reader.i32(state.health_milli) ||
        !reader.i32(state.stamina_milli) || !reader.u8(state.dash_charges) ||
        !reader.boolean(state.air_dash_available) || !reader.u16(state.mode_ticks) ||
        !reader.u16(state.mode_duration_ticks) || !reader.u16(state.jump_buffer_ticks) ||
        !reader.u16(state.coyote_ticks) || !reader.u16(state.roll_buffer_ticks) ||
        !reader.u16(state.stamina_regen_delay_ticks) ||
        !reader.u16(state.landing_roll_window_ticks) || !reader.u8(state.stamina_drain_remainder) ||
        !reader.u8(state.stamina_regen_remainder) || !reader.boolean(has_fall_distance) ||
        !reader.f64(fall_distance) ||
        (has_fall_distance && !read_binary_position(reader, state.fall_origin)) ||
        (scripted_kind != static_cast<std::uint8_t>(ScriptedMovementKind::none) &&
         (!reader.f64(state.scripted_direction.x) || !reader.f64(state.scripted_direction.y) ||
          !reader.f64(state.scripted_direction.z) ||
          !read_binary_position(reader, state.scripted_start) ||
          !read_binary_position(reader, state.scripted_target))) ||
        !reader.u8(locomotion_kind) || !reader.u16(state.locomotion_animation.phase) ||
        !reader.u8(locomotion_from) ||
        !reader.u16(state.locomotion_animation.transition_from_phase) ||
        !reader.u64(state.locomotion_animation.transition_tick) ||
        !reader.u64(state.simulation_tick) || !reader.finished()) {
        return core::Result<PlayerControllerSnapshot>::failure(
            "movement_snapshot.invalid_binary",
            "movement snapshot binary payload is malformed or truncated");
    }
    snapshot.player_net_id = core::NetId::from_value(player_net_id);
    snapshot.player_save_id = core::SaveId::from_value(player_save_id);
    state.mode = static_cast<PlayerControllerMode>(mode);
    state.scripted_kind = static_cast<ScriptedMovementKind>(scripted_kind);
    state.encumbrance = static_cast<EncumbranceTier>(encumbrance);
    state.pending_fall_distance =
        has_fall_distance ? std::optional<double>{fall_distance} : std::nullopt;
    state.locomotion_animation.kind =
        static_cast<animation::LocomotionAnimationKind>(locomotion_kind);
    state.locomotion_animation.transition_from =
        static_cast<animation::LocomotionAnimationKind>(locomotion_from);
    state.last_input_sequence = snapshot.last_processed_input_sequence;
    auto status = snapshot.validate(config);
    if (!status) {
        return core::Result<PlayerControllerSnapshot>::failure(status.error().code,
                                                               status.error().message);
    }
    return core::Result<PlayerControllerSnapshot>::success(std::move(snapshot));
}

MovementPredictionBuffer::MovementPredictionBuffer(std::size_t capacity) : capacity_(capacity) {}

core::Status MovementPredictionBuffer::record(PlayerInputFrame input) {
    auto status = input.validate();
    if (!status || capacity_ == 0 ||
        (!inputs_.empty() &&
         (input.sequence <= inputs_.back().sequence || input.tick <= inputs_.back().tick))) {
        return core::Status::failure(
            !status ? status.error().code : "movement_prediction.invalid_input_order",
            !status ? status.error().message
                    : "predicted movement inputs must be strictly increasing");
    }
    if (inputs_.size() == capacity_) {
        inputs_.pop_front();
    }
    inputs_.push_back(input);
    return core::Status::ok();
}

void MovementPredictionBuffer::set_collision_world_revision(std::uint64_t revision) noexcept {
    collision_world_revision_ = revision;
}

core::Result<ReconciliationResult> MovementPredictionBuffer::reconcile(
    const PlayerControllerState& predicted, const PlayerControllerSnapshot& authoritative,
    const PlayerController& controller, const PlayerMovementModifiers& modifiers,
    ICharacterCollisionWorld& collision) {
    auto status = authoritative.validate(controller.config());
    if (!status) {
        return core::Result<ReconciliationResult>::failure(status.error().code,
                                                           status.error().message);
    }
    ReconciliationResult result;
    result.correction_distance =
        position_distance(predicted.position, authoritative.state.position);
    result.hard_correction = result.correction_distance > 2.0;
    result.state = authoritative.state;
    while (!inputs_.empty() &&
           inputs_.front().sequence <= authoritative.last_processed_input_sequence) {
        inputs_.pop_front();
        ++result.acknowledged_input_count;
    }
    if (collision_world_revision_ != 0 && authoritative.collision_world_revision != 0 &&
        collision_world_revision_ != authoritative.collision_world_revision) {
        result.acknowledged_input_count += inputs_.size();
        inputs_.clear();
        result.hard_correction = true;
        return core::Result<ReconciliationResult>::success(std::move(result));
    }
    for (const auto& input : inputs_) {
        if (result.state.simulation_tick == std::numeric_limits<std::uint64_t>::max()) {
            return core::Result<ReconciliationResult>::failure(
                "movement_prediction.tick_exhausted",
                "movement reconciliation exhausted its simulation tick space");
        }
        auto replay_input = input;
        // Input sequence is the acknowledgement identity. The server intentionally rebases a
        // delayed input onto its current authoritative simulation tick, so replay must do the
        // same instead of treating the client's original predicted tick as immutable.
        replay_input.tick = result.state.simulation_tick + 1;
        auto replayed = controller.tick(result.state, replay_input, modifiers, collision);
        if (!replayed) {
            return core::Result<ReconciliationResult>::failure(replayed.error().code,
                                                               replayed.error().message);
        }
        result.state = std::move(replayed).value().state;
        ++result.replayed_input_count;
    }
    return core::Result<ReconciliationResult>::success(std::move(result));
}

void MovementPredictionBuffer::clear() noexcept {
    inputs_.clear();
}

std::size_t MovementPredictionBuffer::size() const noexcept {
    return inputs_.size();
}

std::vector<PlayerInputFrame> MovementPredictionBuffer::unacknowledged() const {
    return {inputs_.begin(), inputs_.end()};
}

ServerMovementInputQueue::ServerMovementInputQueue(std::size_t capacity) : capacity_(capacity) {}

core::Status ServerMovementInputQueue::push(PlayerInputFrame input) {
    auto status = input.validate();
    if (!status) {
        return status;
    }
    if (capacity_ == 0 || inputs_.size() >= capacity_) {
        return core::Status::failure("movement_input_queue.full",
                                     "server movement input queue is full");
    }
    if (input.sequence <= last_sequence_ || input.tick <= last_tick_) {
        return core::Status::failure("movement_input_queue.out_of_order",
                                     "server movement inputs must be strictly increasing");
    }
    last_sequence_ = input.sequence;
    last_tick_ = input.tick;
    inputs_.push_back(input);
    return core::Status::ok();
}

core::Result<std::size_t> ServerMovementInputQueue::push_bundle(const PlayerInputBundle& bundle) {
    auto status = bundle.validate();
    if (!status) {
        return core::Result<std::size_t>::failure(status.error().code, status.error().message);
    }
    std::size_t accepted = 0;
    for (const auto& frame : bundle.frames) {
        if (frame.sequence <= last_sequence_ || frame.tick <= last_tick_) {
            continue;
        }
        status = push(frame);
        if (!status) {
            return core::Result<std::size_t>::failure(status.error().code, status.error().message);
        }
        ++accepted;
    }
    return core::Result<std::size_t>::success(accepted);
}

std::vector<PlayerInputFrame> ServerMovementInputQueue::drain(std::size_t maximum_count) {
    std::vector<PlayerInputFrame> result;
    result.reserve(std::min(maximum_count, inputs_.size()));
    while (!inputs_.empty() && result.size() < maximum_count) {
        result.push_back(inputs_.front());
        inputs_.pop_front();
    }
    return result;
}

void ServerMovementInputQueue::clear() noexcept {
    inputs_.clear();
    last_sequence_ = 0;
    last_tick_ = 0;
}

std::size_t ServerMovementInputQueue::size() const noexcept {
    return inputs_.size();
}

net::TransportMessage make_movement_input_message(const PlayerInputFrame& input,
                                                  std::int64_t timestamp_ms) {
    return {net::TransportMessageKind::control,
            net::TransportChannel::unreliable,
            input.sequence,
            std::string(movement_input_payload_type),
            PlayerInputTextCodec::encode(input),
            timestamp_ms};
}

core::Result<PlayerInputFrame>
movement_input_from_transport(const net::TransportEnvelope& envelope) {
    if (envelope.message.kind != net::TransportMessageKind::control ||
        envelope.message.channel != net::TransportChannel::unreliable ||
        envelope.message.payload_type != movement_input_payload_type) {
        return core::Result<PlayerInputFrame>::failure("movement_input.invalid_transport",
                                                       "transport envelope is not movement input");
    }
    auto input = PlayerInputTextCodec::decode(envelope.message.payload);
    if (!input || input.value().sequence != envelope.message.sequence) {
        return core::Result<PlayerInputFrame>::failure(
            !input ? input.error().code : "movement_input.sequence_mismatch",
            !input ? input.error().message
                   : "movement input transport sequence does not match its payload");
    }
    return input;
}

net::TransportMessage make_movement_input_bundle_message(const PlayerInputBundle& bundle,
                                                         std::int64_t timestamp_ms) {
    const auto sequence = bundle.frames.empty() ? 0 : bundle.frames.back().sequence;
    return {net::TransportMessageKind::control,
            net::TransportChannel::unreliable,
            sequence,
            std::string(movement_input_bundle_payload_type),
            PlayerInputBundleBinaryCodec::encode(bundle),
            timestamp_ms};
}

core::Result<PlayerInputBundle>
movement_input_bundle_from_transport(const net::TransportEnvelope& envelope) {
    if (envelope.message.kind != net::TransportMessageKind::control ||
        envelope.message.channel != net::TransportChannel::unreliable ||
        (envelope.message.payload_type != movement_input_bundle_payload_type &&
         envelope.message.payload_type != legacy_movement_input_bundle_payload_type)) {
        return core::Result<PlayerInputBundle>::failure(
            "movement_input_bundle.invalid_transport",
            "transport envelope is not a movement input bundle");
    }
    auto bundle = envelope.message.payload_type == movement_input_bundle_payload_type
                      ? PlayerInputBundleBinaryCodec::decode(envelope.message.payload)
                      : PlayerInputBundleTextCodec::decode(envelope.message.payload);
    if (!bundle || bundle.value().frames.back().sequence != envelope.message.sequence) {
        return core::Result<PlayerInputBundle>::failure(
            !bundle ? bundle.error().code : "movement_input_bundle.sequence_mismatch",
            !bundle ? bundle.error().message
                    : "movement input bundle transport sequence does not match its payload");
    }
    return bundle;
}

net::TransportMessage make_movement_snapshot_message(const PlayerControllerSnapshot& snapshot,
                                                     std::int64_t timestamp_ms,
                                                     std::uint64_t transport_sequence) {
    const auto sequence =
        transport_sequence == 0 ? snapshot.state.simulation_tick : transport_sequence;
    return {net::TransportMessageKind::replication,
            net::TransportChannel::unreliable,
            sequence,
            std::string(movement_snapshot_payload_type),
            PlayerControllerSnapshotBinaryCodec::encode(snapshot),
            timestamp_ms};
}

core::Result<PlayerControllerSnapshot>
movement_snapshot_from_transport(const net::TransportEnvelope& envelope,
                                 const PlayerMovementConfig& config) {
    if (envelope.message.kind != net::TransportMessageKind::replication ||
        (envelope.message.channel != net::TransportChannel::unreliable &&
         envelope.message.channel != net::TransportChannel::reliable) ||
        (envelope.message.payload_type != movement_snapshot_payload_type &&
         envelope.message.payload_type != legacy_movement_snapshot_payload_type)) {
        return core::Result<PlayerControllerSnapshot>::failure(
            "movement_snapshot.invalid_transport", "transport envelope is not a movement snapshot");
    }
    return envelope.message.payload_type == movement_snapshot_payload_type
               ? PlayerControllerSnapshotBinaryCodec::decode(envelope.message.payload, config)
               : PlayerControllerSnapshotTextCodec::decode(envelope.message.payload, config);
}

net::TransportMessage make_player_assignment_message(core::NetId player_net_id,
                                                     std::uint64_t sequence,
                                                     std::int64_t timestamp_ms) {
    return {net::TransportMessageKind::replication,
            net::TransportChannel::reliable,
            sequence,
            std::string(player_assignment_payload_type),
            std::to_string(player_net_id.value()),
            timestamp_ms};
}

core::Result<core::NetId> player_assignment_from_transport(const net::TransportEnvelope& envelope) {
    if (envelope.message.kind != net::TransportMessageKind::replication ||
        envelope.message.channel != net::TransportChannel::reliable ||
        envelope.message.payload_type != player_assignment_payload_type) {
        return core::Result<core::NetId>::failure("player_assignment.invalid_transport",
                                                  "transport envelope is not a player assignment");
    }
    auto value = parse_number<std::uint64_t>(envelope.message.payload, "player_net_id");
    if (!value || value.value() == 0) {
        return core::Result<core::NetId>::failure(
            !value ? value.error().code : "player_assignment.invalid_player",
            !value ? value.error().message : "assigned player net id must be valid");
    }
    return core::Result<core::NetId>::success(core::NetId::from_value(value.value()));
}

net::TransportMessage make_player_removal_message(core::NetId player_net_id, std::uint64_t sequence,
                                                  std::int64_t timestamp_ms) {
    return {net::TransportMessageKind::replication,
            net::TransportChannel::reliable,
            sequence,
            std::string(player_removal_payload_type),
            std::to_string(player_net_id.value()),
            timestamp_ms};
}

core::Result<core::NetId> player_removal_from_transport(const net::TransportEnvelope& envelope) {
    if (envelope.message.kind != net::TransportMessageKind::replication ||
        envelope.message.channel != net::TransportChannel::reliable ||
        envelope.message.payload_type != player_removal_payload_type) {
        return core::Result<core::NetId>::failure(
            "player_removal.invalid_transport",
            "transport envelope is not a player removal tombstone");
    }
    auto value = parse_number<std::uint64_t>(envelope.message.payload, "player_net_id");
    if (!value || value.value() == 0) {
        return core::Result<core::NetId>::failure(
            !value ? value.error().code : "player_removal.invalid_player",
            !value ? value.error().message : "removed player net id must be valid");
    }
    return core::Result<core::NetId>::success(core::NetId::from_value(value.value()));
}

} // namespace heartstead::movement
