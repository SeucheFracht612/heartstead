#include "engine/world/chunks/chunk_subscription.hpp"

#include "engine/profiling/profiler.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace heartstead::world {

namespace {

[[nodiscard]] std::uint64_t ordered_axis_bits(std::int64_t value) noexcept {
    return static_cast<std::uint64_t>(value) ^ (std::uint64_t{1} << 63U);
}

[[nodiscard]] std::uint64_t axis_distance(std::int64_t left, std::int64_t right) noexcept {
    const auto ordered_left = ordered_axis_bits(left);
    const auto ordered_right = ordered_axis_bits(right);
    return ordered_left >= ordered_right ? ordered_left - ordered_right
                                         : ordered_right - ordered_left;
}

[[nodiscard]] std::uint64_t saturated_square(std::uint64_t value) noexcept {
    if (value != 0 && value > std::numeric_limits<std::uint64_t>::max() / value) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return value * value;
}

[[nodiscard]] std::uint64_t saturated_add(std::uint64_t left, std::uint64_t right) noexcept {
    return left > std::numeric_limits<std::uint64_t>::max() - right
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

[[nodiscard]] std::uint64_t squared_distance(ChunkCoord left, ChunkCoord right) noexcept {
    const auto x = saturated_square(axis_distance(left.x, right.x));
    const auto y = saturated_square(axis_distance(left.y, right.y));
    const auto z = saturated_square(axis_distance(left.z, right.z));
    return saturated_add(saturated_add(x, y), z);
}

[[nodiscard]] std::int64_t saturated_offset(std::int64_t value, std::int64_t offset) noexcept {
    if (offset > 0 && value > std::numeric_limits<std::int64_t>::max() - offset) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (offset < 0 && value < std::numeric_limits<std::int64_t>::min() - offset) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return value + offset;
}

[[nodiscard]] bool within_cylinder(ChunkCoord coord, ChunkCoord center,
                                   std::uint16_t horizontal_radius,
                                   std::uint16_t vertical_radius) noexcept {
    const auto dx = axis_distance(coord.x, center.x);
    const auto dy = axis_distance(coord.y, center.y);
    const auto dz = axis_distance(coord.z, center.z);
    const auto horizontal = static_cast<std::uint64_t>(horizontal_radius);
    return dx <= horizontal && dz <= horizontal &&
           dy <= static_cast<std::uint64_t>(vertical_radius) &&
           dx * dx + dz * dz <= horizontal * horizontal;
}

[[nodiscard]] std::set<ChunkCoord> desired_chunks(ChunkCoord center,
                                                  const ChunkSubscriptionPolicy& policy) {
    std::set<ChunkCoord> desired;
    const auto horizontal = static_cast<std::int64_t>(policy.subscribe_horizontal_radius_chunks);
    const auto vertical = static_cast<std::int64_t>(policy.subscribe_vertical_radius_chunks);
    for (std::int64_t z = -horizontal; z <= horizontal; ++z) {
        for (std::int64_t x = -horizontal; x <= horizontal; ++x) {
            if (x * x + z * z > horizontal * horizontal) {
                continue;
            }
            for (std::int64_t y = -vertical; y <= vertical; ++y) {
                desired.insert({saturated_offset(center.x, x), saturated_offset(center.y, y),
                                saturated_offset(center.z, z)});
            }
        }
    }
    return desired;
}

void sort_nearest_first(std::vector<ChunkCoord>& chunks, ChunkCoord center) {
    std::ranges::sort(chunks, [center](ChunkCoord left, ChunkCoord right) {
        const auto left_distance = squared_distance(left, center);
        const auto right_distance = squared_distance(right, center);
        return left_distance != right_distance ? left_distance < right_distance : left < right;
    });
}

void sort_farthest_first(std::vector<ChunkCoord>& chunks, ChunkCoord center) {
    std::ranges::sort(chunks, [center](ChunkCoord left, ChunkCoord right) {
        const auto left_distance = squared_distance(left, center);
        const auto right_distance = squared_distance(right, center);
        return left_distance != right_distance ? left_distance > right_distance : left < right;
    });
}

} // namespace

core::Status ChunkSubscriptionPolicy::validate() const noexcept {
    if (subscribe_horizontal_radius_chunks > retain_horizontal_radius_chunks ||
        subscribe_vertical_radius_chunks > retain_vertical_radius_chunks) {
        return core::Status::failure("chunk_subscription.invalid_hysteresis",
                                     "chunk subscription radii must not exceed their retain radii");
    }
    if (retain_horizontal_radius_chunks > max_radius_chunks ||
        retain_vertical_radius_chunks > max_radius_chunks) {
        return core::Status::failure("chunk_subscription.radius_too_large",
                                     "chunk subscription radius exceeds its planning bound");
    }
    if (max_chunks_per_client == 0 || max_chunks_per_client > hard_max_chunks_per_client ||
        desired_chunk_count() > max_chunks_per_client) {
        return core::Status::failure(
            "chunk_subscription.invalid_client_cap",
            "chunk subscription cap must contain the desired volume and stay within its hard "
            "bound");
    }
    if (max_additions_per_update == 0 || max_removals_per_update == 0 ||
        max_additions_per_update > max_chunks_per_client ||
        max_removals_per_update > max_chunks_per_client) {
        return core::Status::failure(
            "chunk_subscription.invalid_transition_budget",
            "chunk subscription transition budgets must be non-zero and fit the client cap");
    }
    return core::Status::ok();
}

std::size_t ChunkSubscriptionPolicy::desired_chunk_count() const noexcept {
    const auto horizontal = static_cast<std::int64_t>(subscribe_horizontal_radius_chunks);
    std::size_t horizontal_count = 0;
    for (std::int64_t z = -horizontal; z <= horizontal; ++z) {
        for (std::int64_t x = -horizontal; x <= horizontal; ++x) {
            horizontal_count += x * x + z * z <= horizontal * horizontal ? 1U : 0U;
        }
    }
    return horizontal_count *
           (static_cast<std::size_t>(subscribe_vertical_radius_chunks) * 2U + 1U);
}

bool ChunkSubscriptionPlan::converged() const noexcept {
    return deferred_addition_count == 0 && deferred_removal_count == 0;
}

core::Result<ChunkSubscriptionPlan>
plan_chunk_subscriptions(std::span<const ChunkCoord> existing_subscriptions, ChunkCoord center,
                         const ChunkSubscriptionPolicy& policy,
                         ChunkSubscriptionTransitionBudget budget) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("network.chunk_subscription_plan");
    auto status = policy.validate();
    if (!status) {
        return core::Result<ChunkSubscriptionPlan>::failure(status.error().code,
                                                            status.error().message);
    }
    if (existing_subscriptions.size() > policy.max_chunks_per_client ||
        budget.additions > policy.max_chunks_per_client ||
        budget.removals > policy.max_chunks_per_client) {
        return core::Result<ChunkSubscriptionPlan>::failure(
            "chunk_subscription.input_exceeds_cap",
            "existing subscriptions and transition budgets must fit the per-client cap");
    }

    std::set<ChunkCoord> subscriptions(existing_subscriptions.begin(),
                                       existing_subscriptions.end());
    if (subscriptions.size() != existing_subscriptions.size()) {
        return core::Result<ChunkSubscriptionPlan>::failure(
            "chunk_subscription.duplicate_existing", "existing chunk subscriptions must be unique");
    }

    const auto desired = desired_chunks(center, policy);
    ChunkSubscriptionPlan plan;
    plan.center = center;
    plan.initial_subscription_count = subscriptions.size();
    plan.desired_subscription_count = desired.size();

    std::vector<ChunkCoord> removal_candidates;
    removal_candidates.reserve(subscriptions.size());
    for (const auto coord : subscriptions) {
        if (!within_cylinder(coord, center, policy.retain_horizontal_radius_chunks,
                             policy.retain_vertical_radius_chunks)) {
            removal_candidates.push_back(coord);
        }
    }
    sort_farthest_first(removal_candidates, center);
    const auto removal_count = std::min(budget.removals, removal_candidates.size());
    plan.removed_chunks.assign(
        removal_candidates.begin(),
        removal_candidates.begin() +
            static_cast<std::vector<ChunkCoord>::difference_type>(removal_count));
    for (const auto coord : plan.removed_chunks) {
        subscriptions.erase(coord);
    }
    plan.deferred_removal_count = removal_candidates.size() - removal_count;
    const auto remaining_removal_budget = budget.removals - removal_count;

    std::vector<ChunkCoord> addition_candidates;
    addition_candidates.reserve(desired.size());
    for (const auto coord : desired) {
        if (!subscriptions.contains(coord)) {
            addition_candidates.push_back(coord);
        }
    }
    sort_nearest_first(addition_candidates, center);
    const auto budget_admitted = std::min(budget.additions, addition_candidates.size());
    auto available_capacity = policy.max_chunks_per_client - subscriptions.size();
    if (budget_admitted > available_capacity && remaining_removal_budget > 0) {
        std::vector<ChunkCoord> capacity_eviction_candidates;
        capacity_eviction_candidates.reserve(subscriptions.size());
        for (const auto coord : subscriptions) {
            if (!desired.contains(coord)) {
                capacity_eviction_candidates.push_back(coord);
            }
        }
        sort_farthest_first(capacity_eviction_candidates, center);
        const auto capacity_needed = budget_admitted - available_capacity;
        const auto eviction_count = std::min(
            {capacity_needed, remaining_removal_budget, capacity_eviction_candidates.size()});
        plan.removed_chunks.insert(
            plan.removed_chunks.end(), capacity_eviction_candidates.begin(),
            capacity_eviction_candidates.begin() +
                static_cast<std::vector<ChunkCoord>::difference_type>(eviction_count));
        for (std::size_t index = 0; index < eviction_count; ++index) {
            subscriptions.erase(capacity_eviction_candidates[index]);
        }
        available_capacity = policy.max_chunks_per_client - subscriptions.size();
    }
    const auto addition_count =
        std::min({budget.additions, available_capacity, addition_candidates.size()});
    plan.added_chunks.assign(
        addition_candidates.begin(),
        addition_candidates.begin() +
            static_cast<std::vector<ChunkCoord>::difference_type>(addition_count));
    for (const auto coord : plan.added_chunks) {
        subscriptions.insert(coord);
    }
    plan.deferred_addition_count = addition_candidates.size() - addition_count;
    plan.capacity_deferred_addition_count = budget_admitted - addition_count;
    plan.subscriptions.assign(subscriptions.begin(), subscriptions.end());
    plan.hysteresis_retained_count =
        static_cast<std::size_t>(std::ranges::count_if(plan.subscriptions, [&](ChunkCoord coord) {
            return !desired.contains(coord) &&
                   within_cylinder(coord, center, policy.retain_horizontal_radius_chunks,
                                   policy.retain_vertical_radius_chunks);
        }));
    HEARTSTEAD_PROFILE_PLOT("network.chunk_subscriptions", plan.subscriptions.size());
    HEARTSTEAD_PROFILE_PLOT("network.chunk_subscription_deferred_additions",
                            plan.deferred_addition_count);
    return core::Result<ChunkSubscriptionPlan>::success(std::move(plan));
}

} // namespace heartstead::world
