#include "engine/entities/entity_motion_snapshot.hpp"

#include <cassert>
#include <string>

int main() {
    using namespace heartstead;
    entities::EntityMotionSnapshot snapshot;
    snapshot.entity_net_id = core::NetId::from_value(1'000'042);
    snapshot.prototype_id = *core::PrototypeId::parse("base:entities/test_animal");
    auto previous_position =
        world::WorldPosition::from_anchor({9'000'000'000'000LL, 1, -7}, {0.25, 0.0, 0.5});
    assert(previous_position);
    snapshot.previous_transform.position = previous_position.value();
    snapshot.current_transform = snapshot.previous_transform;
    auto current_position =
        world::WorldPosition::from_anchor({9'000'000'000'000LL, 1, -7}, {0.5, 0.0, 0.75});
    assert(current_position);
    snapshot.current_transform.position = current_position.value();
    snapshot.current_transform.rotation_degrees.y = 45.0;
    snapshot.current_transform.scale = {0.72, 0.72, 0.72};
    snapshot.previous_transform.scale = snapshot.current_transform.scale;
    snapshot.locomotion.kind = animation::LocomotionAnimationKind::walk;
    snapshot.locomotion.phase = 32'768;
    snapshot.locomotion.transition_from = animation::LocomotionAnimationKind::idle;
    snapshot.locomotion.transition_tick = 40;
    snapshot.simulation_tick = 42;
    assert(snapshot.validate());

    const auto encoded = entities::EntityMotionSnapshotTextCodec::encode(snapshot);
    auto decoded = entities::EntityMotionSnapshotTextCodec::decode(encoded);
    assert(decoded);
    assert(decoded.value() == snapshot);
    assert(!entities::EntityMotionSnapshotTextCodec::decode(encoded + "|trailing"));
    assert(!entities::EntityMotionSnapshotTextCodec::decode(std::string(4097, 'x')));

    auto message = entities::make_entity_motion_snapshot_message(snapshot, 77, 1234);
    net::TransportEnvelope envelope{core::NetId::from_value(1), core::NetId::from_value(2),
                                    std::move(message)};
    auto transported = entities::entity_motion_snapshot_from_transport(envelope);
    assert(transported);
    assert(transported.value() == snapshot);
    envelope.message.channel = net::TransportChannel::reliable;
    assert(!entities::entity_motion_snapshot_from_transport(envelope));

    envelope.message =
        entities::make_entity_motion_removal_message(snapshot.entity_net_id, 78, 1235);
    auto removed = entities::entity_motion_removal_from_transport(envelope);
    assert(removed && removed.value() == snapshot.entity_net_id);
    envelope.message.payload = "0";
    assert(!entities::entity_motion_removal_from_transport(envelope));
    return 0;
}
