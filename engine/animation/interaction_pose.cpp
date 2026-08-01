#include "engine/animation/interaction_pose.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace heartstead::animation {

core::Status AnimationInteractionRequest::validate() const {
    std::unordered_set<std::uint8_t> semantics;
    const auto valid_effectors =
        effectors.size() <= 16U &&
        std::ranges::all_of(effectors, [&](const AnimationEffectorTarget& target) {
            const auto semantic = static_cast<std::uint8_t>(target.semantic);
            const auto normal_length = math::length_squared(target.surface_normal);
            return !target.node.empty() && target.node.size() <= 128U &&
                   target.rotation_degrees.is_finite() && target.surface_normal.is_finite() &&
                   std::isfinite(normal_length) && normal_length > 0.0001F &&
                   std::isfinite(target.position_weight) && target.position_weight >= 0.0F &&
                   target.position_weight <= 1.0F && std::isfinite(target.rotation_weight) &&
                   target.rotation_weight >= 0.0F && target.rotation_weight <= 1.0F &&
                   semantics.insert(semantic).second;
        });
    if (!entity.is_valid() || revision == 0U || !std::isfinite(normalized_phase) ||
        normalized_phase < 0.0F || normalized_phase > 1.0F || !valid_effectors) {
        return core::Status::failure(
            "animation_interaction.invalid_request",
            "animation interaction requires valid identity, phase, and unique finite effectors");
    }
    return core::Status::ok();
}

} // namespace heartstead::animation
