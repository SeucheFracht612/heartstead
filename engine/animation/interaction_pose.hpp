#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/world/coords/world_position.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace heartstead::animation {

enum class AnimationInteractionKind : std::uint8_t {
    grounded,
    tool_action,
    workstation,
    mantle,
    vault,
    climb,
    swim,
    cart,
    ship,
};

enum class AnimationEffectorSemantic : std::uint8_t {
    left_foot,
    right_foot,
    left_hand,
    right_hand,
    primary_tool,
    secondary_tool,
    hips,
    look,
};

struct AnimationEffectorTarget {
    AnimationEffectorSemantic semantic{AnimationEffectorSemantic::left_foot};
    std::string node;
    world::WorldPosition position;
    math::Vec3f rotation_degrees{};
    math::Vec3f surface_normal{0.0F, 1.0F, 0.0F};
    float position_weight{1.0F};
    float rotation_weight{1.0F};

    [[nodiscard]] bool operator==(const AnimationEffectorTarget&) const = default;
};

struct AnimationInteractionRequest {
    core::NetId entity;
    AnimationInteractionKind kind{AnimationInteractionKind::grounded};
    std::vector<AnimationEffectorTarget> effectors;
    float normalized_phase{0.0F};
    std::uint64_t revision{0};

    [[nodiscard]] core::Status validate() const;
};

} // namespace heartstead::animation
