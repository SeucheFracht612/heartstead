#pragma once

#include "engine/animation/locomotion_animation.hpp"
#include "engine/core/ids.hpp"
#include "engine/entities/entity_visual.hpp"
#include "engine/math/vector.hpp"
#include "engine/world/coords/world_position.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace heartstead::game {

struct PresentationObjectIdTag;
using PresentationObjectId = core::GenerationalHandle<PresentationObjectIdTag>;

struct RenderObjectSnapshot {
    PresentationObjectId id;
    core::NetId source_net_id;
    core::PrototypeId visual_prototype;
    world::WorldTransform previous_transform;
    world::WorldTransform current_transform;
    animation::ReplicatedLocomotionAnimation previous_locomotion;
    animation::ReplicatedLocomotionAnimation current_locomotion;
    math::Bounds3f local_bounds{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::vector<entities::VisualStateValue> visual_states;
    std::uint64_t source_revision = 0;
    bool visible = true;
    bool teleported = false;
};

struct RenderSnapshot {
    std::uint64_t simulation_tick = 0;
    std::uint64_t presentation_revision = 0;
    std::vector<RenderObjectSnapshot> objects;
};

} // namespace heartstead::game
