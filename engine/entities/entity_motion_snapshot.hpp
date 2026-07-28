#pragma once

#include "engine/animation/locomotion_animation.hpp"
#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/net/transport.hpp"
#include "engine/world/coords/world_position.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace heartstead::entities {

inline constexpr std::uint16_t entity_motion_snapshot_version = 1;
inline constexpr std::string_view entity_motion_snapshot_payload_type = "entity.motion_snapshot.v1";
inline constexpr std::string_view entity_motion_removal_payload_type = "entity.motion_removal.v1";

struct EntityMotionSnapshot {
    std::uint16_t version = entity_motion_snapshot_version;
    core::NetId entity_net_id;
    core::PrototypeId prototype_id;
    world::WorldTransform previous_transform;
    world::WorldTransform current_transform;
    animation::ReplicatedLocomotionAnimation locomotion;
    std::uint64_t simulation_tick = 0;

    [[nodiscard]] core::Status validate() const;
    friend bool operator==(const EntityMotionSnapshot&, const EntityMotionSnapshot&) = default;
};

class EntityMotionSnapshotTextCodec {
  public:
    [[nodiscard]] static std::string encode(const EntityMotionSnapshot& snapshot);
    [[nodiscard]] static core::Result<EntityMotionSnapshot> decode(std::string_view payload);
};

[[nodiscard]] net::TransportMessage
make_entity_motion_snapshot_message(const EntityMotionSnapshot& snapshot,
                                    std::uint64_t transport_sequence, std::int64_t timestamp_ms);
[[nodiscard]] core::Result<EntityMotionSnapshot>
entity_motion_snapshot_from_transport(const net::TransportEnvelope& envelope);
[[nodiscard]] net::TransportMessage
make_entity_motion_removal_message(core::NetId entity_net_id, std::uint64_t transport_sequence,
                                   std::int64_t timestamp_ms);
[[nodiscard]] core::Result<core::NetId>
entity_motion_removal_from_transport(const net::TransportEnvelope& envelope);

} // namespace heartstead::entities
