#include "engine/entities/entity_motion_snapshot.hpp"

#include <charconv>
#include <cmath>
#include <sstream>
#include <vector>

namespace heartstead::entities {

namespace {

template <typename T>
[[nodiscard]] core::Result<T> parse_number(std::string_view text, std::string_view field) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return core::Result<T>::failure("entity_motion_snapshot.invalid_number",
                                        "entity motion field is invalid: " + std::string(field));
    }
    return core::Result<T>::success(value);
}

template <>
core::Result<double> parse_number<double>(std::string_view text, std::string_view field) {
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        return core::Result<double>::failure("entity_motion_snapshot.invalid_number",
                                             "entity motion field is invalid: " +
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

void encode_transform(std::ostringstream& output, const world::WorldTransform& transform) {
    output << transform.position.anchor.x << '|' << transform.position.anchor.y << '|'
           << transform.position.anchor.z << '|' << transform.position.local_offset.x << '|'
           << transform.position.local_offset.y << '|' << transform.position.local_offset.z << '|'
           << transform.rotation_degrees.x << '|' << transform.rotation_degrees.y << '|'
           << transform.rotation_degrees.z << '|' << transform.scale.x << '|' << transform.scale.y
           << '|' << transform.scale.z;
}

struct ParsedTransform {
    core::Result<std::int64_t> anchor_x;
    core::Result<std::int64_t> anchor_y;
    core::Result<std::int64_t> anchor_z;
    core::Result<double> local_x;
    core::Result<double> local_y;
    core::Result<double> local_z;
    core::Result<double> rotation_x;
    core::Result<double> rotation_y;
    core::Result<double> rotation_z;
    core::Result<double> scale_x;
    core::Result<double> scale_y;
    core::Result<double> scale_z;

    [[nodiscard]] bool parsed() const noexcept {
        return anchor_x && anchor_y && anchor_z && local_x && local_y && local_z && rotation_x &&
               rotation_y && rotation_z && scale_x && scale_y && scale_z;
    }

    [[nodiscard]] core::Result<world::WorldTransform> materialize() const {
        auto position = world::WorldPosition::from_anchor(
            {anchor_x.value(), anchor_y.value(), anchor_z.value()},
            {local_x.value(), local_y.value(), local_z.value()});
        if (!position) {
            return core::Result<world::WorldTransform>::failure(position.error().code,
                                                                position.error().message);
        }
        world::WorldTransform transform;
        transform.position = position.value();
        transform.rotation_degrees = {rotation_x.value(), rotation_y.value(), rotation_z.value()};
        transform.scale = {scale_x.value(), scale_y.value(), scale_z.value()};
        return core::Result<world::WorldTransform>::success(transform);
    }
};

} // namespace

core::Status EntityMotionSnapshot::validate() const {
    if (version != entity_motion_snapshot_version || !entity_net_id.is_valid() ||
        !prototype_id.is_valid() || !previous_transform.is_finite() ||
        !previous_transform.has_non_zero_scale() || !current_transform.is_finite() ||
        !current_transform.has_non_zero_scale()) {
        return core::Status::failure(
            "entity_motion_snapshot.invalid_state",
            "entity motion snapshot identity and transforms must be valid");
    }
    return locomotion.validate(simulation_tick);
}

std::string EntityMotionSnapshotTextCodec::encode(const EntityMotionSnapshot& snapshot) {
    std::ostringstream output;
    output.precision(17);
    output << snapshot.version << '|' << snapshot.entity_net_id.value() << '|'
           << snapshot.prototype_id.value() << '|';
    encode_transform(output, snapshot.previous_transform);
    output << '|';
    encode_transform(output, snapshot.current_transform);
    output << '|' << static_cast<unsigned>(snapshot.locomotion.kind) << '|'
           << snapshot.locomotion.phase << '|'
           << static_cast<unsigned>(snapshot.locomotion.transition_from) << '|'
           << snapshot.locomotion.transition_from_phase << '|'
           << snapshot.locomotion.transition_tick << '|' << snapshot.simulation_tick;
    return output.str();
}

core::Result<EntityMotionSnapshot> EntityMotionSnapshotTextCodec::decode(std::string_view payload) {
    if (payload.empty() || payload.size() > 4096) {
        return core::Result<EntityMotionSnapshot>::failure(
            "entity_motion_snapshot.invalid_payload_size",
            "entity motion snapshot payload size is invalid");
    }
    const auto fields = split(payload);
    if (fields.size() != 33) {
        return core::Result<EntityMotionSnapshot>::failure(
            "entity_motion_snapshot.invalid_payload",
            "entity motion snapshot must contain 33 fields");
    }
    std::size_t index = 0;
    const auto next_u64 = [&](std::string_view name) {
        return parse_number<std::uint64_t>(fields[index++], name);
    };
    const auto next_i64 = [&](std::string_view name) {
        return parse_number<std::int64_t>(fields[index++], name);
    };
    const auto next_u32 = [&](std::string_view name) {
        return parse_number<std::uint32_t>(fields[index++], name);
    };
    const auto next_u16 = [&](std::string_view name) {
        return parse_number<std::uint16_t>(fields[index++], name);
    };
    const auto next_double = [&](std::string_view name) {
        return parse_number<double>(fields[index++], name);
    };
    const auto next_transform = [&] {
        return ParsedTransform{
            next_i64("anchor_x"),      next_i64("anchor_y"),      next_i64("anchor_z"),
            next_double("local_x"),    next_double("local_y"),    next_double("local_z"),
            next_double("rotation_x"), next_double("rotation_y"), next_double("rotation_z"),
            next_double("scale_x"),    next_double("scale_y"),    next_double("scale_z")};
    };

    auto version = next_u16("version");
    auto net_id = next_u64("net_id");
    const auto prototype_text = fields[index++];
    auto previous = next_transform();
    auto current = next_transform();
    auto kind = next_u32("locomotion_kind");
    auto phase = next_u16("locomotion_phase");
    auto transition_from = next_u32("transition_from");
    auto transition_phase = next_u16("transition_phase");
    auto transition_tick = next_u64("transition_tick");
    auto simulation_tick = next_u64("simulation_tick");
    const auto prototype = core::PrototypeId::parse(prototype_text);
    if (!version || !net_id || !prototype || !previous.parsed() || !current.parsed() || !kind ||
        !phase || !transition_from || !transition_phase || !transition_tick || !simulation_tick ||
        kind.value() > static_cast<std::uint32_t>(animation::LocomotionAnimationKind::fall) ||
        transition_from.value() >
            static_cast<std::uint32_t>(animation::LocomotionAnimationKind::fall)) {
        return core::Result<EntityMotionSnapshot>::failure(
            "entity_motion_snapshot.invalid_payload", "entity motion snapshot fields are invalid");
    }
    auto previous_transform = previous.materialize();
    auto current_transform = current.materialize();
    if (!previous_transform || !current_transform) {
        const auto& error =
            !previous_transform ? previous_transform.error() : current_transform.error();
        return core::Result<EntityMotionSnapshot>::failure(error.code, error.message);
    }

    EntityMotionSnapshot snapshot;
    snapshot.version = version.value();
    snapshot.entity_net_id = core::NetId::from_value(net_id.value());
    snapshot.prototype_id = *prototype;
    snapshot.previous_transform = previous_transform.value();
    snapshot.current_transform = current_transform.value();
    snapshot.locomotion.kind = static_cast<animation::LocomotionAnimationKind>(kind.value());
    snapshot.locomotion.phase = phase.value();
    snapshot.locomotion.transition_from =
        static_cast<animation::LocomotionAnimationKind>(transition_from.value());
    snapshot.locomotion.transition_from_phase = transition_phase.value();
    snapshot.locomotion.transition_tick = transition_tick.value();
    snapshot.simulation_tick = simulation_tick.value();
    auto status = snapshot.validate();
    if (!status) {
        return core::Result<EntityMotionSnapshot>::failure(status.error().code,
                                                           status.error().message);
    }
    return core::Result<EntityMotionSnapshot>::success(std::move(snapshot));
}

net::TransportMessage make_entity_motion_snapshot_message(const EntityMotionSnapshot& snapshot,
                                                          std::uint64_t transport_sequence,
                                                          std::int64_t timestamp_ms) {
    return {net::TransportMessageKind::replication,
            net::TransportChannel::unreliable,
            transport_sequence,
            std::string(entity_motion_snapshot_payload_type),
            EntityMotionSnapshotTextCodec::encode(snapshot),
            timestamp_ms};
}

core::Result<EntityMotionSnapshot>
entity_motion_snapshot_from_transport(const net::TransportEnvelope& envelope) {
    if (envelope.message.kind != net::TransportMessageKind::replication ||
        envelope.message.channel != net::TransportChannel::unreliable ||
        envelope.message.payload_type != entity_motion_snapshot_payload_type) {
        return core::Result<EntityMotionSnapshot>::failure(
            "entity_motion_snapshot.invalid_transport",
            "transport envelope is not an entity motion snapshot");
    }
    return EntityMotionSnapshotTextCodec::decode(envelope.message.payload);
}

net::TransportMessage make_entity_motion_removal_message(core::NetId entity_net_id,
                                                         std::uint64_t transport_sequence,
                                                         std::int64_t timestamp_ms) {
    return {net::TransportMessageKind::replication,
            net::TransportChannel::reliable,
            transport_sequence,
            std::string(entity_motion_removal_payload_type),
            std::to_string(entity_net_id.value()),
            timestamp_ms};
}

core::Result<core::NetId>
entity_motion_removal_from_transport(const net::TransportEnvelope& envelope) {
    if (envelope.message.kind != net::TransportMessageKind::replication ||
        envelope.message.channel != net::TransportChannel::reliable ||
        envelope.message.payload_type != entity_motion_removal_payload_type) {
        return core::Result<core::NetId>::failure(
            "entity_motion_removal.invalid_transport",
            "transport envelope is not an entity motion removal");
    }
    auto value = parse_number<std::uint64_t>(envelope.message.payload, "entity_net_id");
    if (!value || value.value() == 0) {
        return core::Result<core::NetId>::failure(
            !value ? value.error().code : "entity_motion_removal.invalid_entity",
            !value ? value.error().message : "removed entity net id must be valid");
    }
    return core::Result<core::NetId>::success(core::NetId::from_value(value.value()));
}

} // namespace heartstead::entities
