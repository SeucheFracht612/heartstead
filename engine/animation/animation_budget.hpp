#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace heartstead::animation {

struct AnimationBudgetCandidate {
    std::uint64_t object_id{0};
    float distance_squared{0.0F};
    std::uint8_t importance{128};
    bool visible{true};
    bool force_update{false};
};

struct AnimationBudgetSettings {
    std::size_t maximum_pose_evaluations{256};
    float full_rate_distance{24.0F};
    float half_rate_distance{48.0F};
    float quarter_rate_distance{96.0F};
    std::uint32_t distant_update_interval{8};
};

struct AnimationBudgetDecision {
    std::uint64_t object_id{0};
    std::uint32_t update_interval{1};
    bool evaluate_pose{false};
    bool interpolate{false};
};

[[nodiscard]] std::vector<AnimationBudgetDecision> schedule_animation_updates(
    std::span<const AnimationBudgetCandidate> candidates,
    const AnimationBudgetSettings& settings,
    std::uint64_t frame_index);

} // namespace heartstead::animation
