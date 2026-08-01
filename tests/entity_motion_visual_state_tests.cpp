#include "engine/entities/entity_motion_snapshot.hpp"

#include <cassert>

int main() {
    using namespace heartstead;

    entities::EntityMotionSnapshot snapshot;
    snapshot.entity_net_id = core::NetId::from_value(42);
    snapshot.prototype_id = *core::PrototypeId::parse("base:entities/workshop_machine");
    snapshot.simulation_tick = 120;
    snapshot.visual_states = {
        {.channel = "activity", .value = "active"},
        {.channel = "heat", .value = "hot"},
        {.channel = "process", .value = "loaded"},
    };
    assert(snapshot.validate());
    const auto encoded = entities::EntityMotionSnapshotTextCodec::encode(snapshot);
    auto decoded = entities::EntityMotionSnapshotTextCodec::decode(encoded);
    assert(decoded);
    assert(decoded.value() == snapshot);

    auto legacy = snapshot;
    legacy.version = 1;
    legacy.visual_states.clear();
    auto decoded_legacy = entities::EntityMotionSnapshotTextCodec::decode(
        entities::EntityMotionSnapshotTextCodec::encode(legacy));
    assert(decoded_legacy && decoded_legacy.value() == legacy);

    auto duplicate = snapshot;
    duplicate.visual_states.push_back({.channel = "heat", .value = "cold"});
    assert(!duplicate.validate());

    auto invalid_delimiter = snapshot;
    invalid_delimiter.visual_states[0].value = "active|broken";
    assert(!invalid_delimiter.validate());
}
