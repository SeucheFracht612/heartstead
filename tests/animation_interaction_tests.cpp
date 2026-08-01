#include "engine/animation/interaction_pose.hpp"

#include <cassert>

int main() {
    using namespace heartstead;

    animation::AnimationInteractionRequest request;
    request.entity = core::NetId::from_value(81);
    request.kind = animation::AnimationInteractionKind::workstation;
    request.normalized_phase = 0.5F;
    request.revision = 4;
    request.effectors.push_back({
        .semantic = animation::AnimationEffectorSemantic::right_hand,
        .node = "HandR",
        .position = world::WorldPosition{8'000'000'000'000.0, 2.0, -8'000'000'000'000.0},
        .rotation_degrees = {0.0F, 45.0F, 0.0F},
        .surface_normal = {0.0F, 1.0F, 0.0F},
        .position_weight = 1.0F,
        .rotation_weight = 0.75F,
    });
    assert(request.validate());

    request.effectors.push_back(request.effectors.front());
    assert(!request.validate());
    request.effectors.pop_back();
    request.normalized_phase = 1.5F;
    assert(!request.validate());
}
