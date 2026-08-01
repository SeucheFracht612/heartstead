#include "engine/animation/animation_budget.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <stdexcept>

int main() {
    using namespace heartstead::animation;

    constexpr AnimationBudgetSettings settings{
        .maximum_pose_evaluations = 2,
        .full_rate_distance = 10.0F,
        .half_rate_distance = 20.0F,
        .quarter_rate_distance = 40.0F,
        .distant_update_interval = 8,
    };
    constexpr std::array candidates{
        AnimationBudgetCandidate{.object_id = 4, .distance_squared = 25.0F, .importance = 10, .visible = true, .force_update = false},
        AnimationBudgetCandidate{.object_id = 2, .distance_squared = 25.0F, .importance = 20, .visible = true, .force_update = false},
        AnimationBudgetCandidate{.object_id = 8, .distance_squared = 10'000.0F, .importance = 255, .visible = true, .force_update = true},
        AnimationBudgetCandidate{.object_id = 9, .distance_squared = 10'000.0F, .importance = 255, .visible = false, .force_update = true},
    };

    const auto decisions = schedule_animation_updates(candidates, settings, 0);
    assert(decisions.size() == 3);
    assert(std::ranges::is_sorted(decisions, {}, &AnimationBudgetDecision::object_id));
    assert(decisions[0].object_id == 2 && decisions[0].evaluate_pose);
    assert(decisions[1].object_id == 4 && !decisions[1].evaluate_pose);
    assert(decisions[2].object_id == 8 && decisions[2].evaluate_pose);
    assert(decisions[2].update_interval == 1);

    constexpr std::array staggered_candidates{
        AnimationBudgetCandidate{.object_id = 1, .distance_squared = 225.0F, .importance = 0, .visible = true, .force_update = false},
        AnimationBudgetCandidate{.object_id = 2, .distance_squared = 225.0F, .importance = 0, .visible = true, .force_update = false},
    };
    const auto staggered = schedule_animation_updates(staggered_candidates, settings, 1);
    assert(staggered[0].update_interval == 2 && staggered[0].evaluate_pose);
    assert(staggered[1].update_interval == 2 && !staggered[1].evaluate_pose);
    assert(staggered[0].interpolate && staggered[1].interpolate);

    bool rejected = false;
    try {
        auto invalid = settings;
        invalid.half_rate_distance = 1.0F;
        static_cast<void>(schedule_animation_updates(candidates, invalid, 0));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
