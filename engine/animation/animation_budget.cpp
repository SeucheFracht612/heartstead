#include "engine/animation/animation_budget.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace heartstead::animation {
namespace {

[[nodiscard]] std::uint32_t update_interval(
    const AnimationBudgetCandidate& candidate,
    const AnimationBudgetSettings& settings) {
    if (candidate.force_update || candidate.importance >= 224U) {
        return 1;
    }
    const auto distance = std::sqrt(std::max(candidate.distance_squared, 0.0F));
    if (distance <= settings.full_rate_distance) {
        return 1;
    }
    if (distance <= settings.half_rate_distance) {
        return 2;
    }
    if (distance <= settings.quarter_rate_distance) {
        return 4;
    }
    return settings.distant_update_interval;
}

} // namespace

std::vector<AnimationBudgetDecision> schedule_animation_updates(
    const std::span<const AnimationBudgetCandidate> candidates,
    const AnimationBudgetSettings& settings,
    const std::uint64_t frame_index) {
    if (!(settings.full_rate_distance >= 0.0F) ||
        !(settings.half_rate_distance >= settings.full_rate_distance) ||
        !(settings.quarter_rate_distance >= settings.half_rate_distance) ||
        settings.distant_update_interval == 0U) {
        throw std::invalid_argument("animation budget distances and update intervals must be ordered and non-zero");
    }

    struct RankedDecision {
        AnimationBudgetCandidate candidate;
        AnimationBudgetDecision decision;
        bool due{false};
    };
    std::vector<RankedDecision> ranked;
    ranked.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (!candidate.visible || !std::isfinite(candidate.distance_squared)) {
            continue;
        }
        const auto interval = update_interval(candidate, settings);
        ranked.push_back({
            .candidate = candidate,
            .decision = {
                .object_id = candidate.object_id,
                .update_interval = interval,
                .evaluate_pose = false,
                .interpolate = interval > 1U,
            },
            .due = candidate.force_update || frame_index % interval == candidate.object_id % interval,
        });
    }

    std::ranges::sort(ranked, [](const RankedDecision& left, const RankedDecision& right) {
        if (left.candidate.force_update != right.candidate.force_update) {
            return left.candidate.force_update;
        }
        if (left.candidate.importance != right.candidate.importance) {
            return left.candidate.importance > right.candidate.importance;
        }
        if (left.candidate.distance_squared != right.candidate.distance_squared) {
            return left.candidate.distance_squared < right.candidate.distance_squared;
        }
        return left.candidate.object_id < right.candidate.object_id;
    });

    std::size_t evaluation_count = 0;
    for (auto& entry : ranked) {
        if (entry.due && evaluation_count < settings.maximum_pose_evaluations) {
            entry.decision.evaluate_pose = true;
            ++evaluation_count;
        }
    }

    std::vector<AnimationBudgetDecision> result;
    result.reserve(ranked.size());
    for (const auto& entry : ranked) {
        result.push_back(entry.decision);
    }
    std::ranges::sort(result, {}, &AnimationBudgetDecision::object_id);
    return result;
}

} // namespace heartstead::animation
