#pragma once

#include "engine/core/result.hpp"
#include "engine/world/coords/world_coords.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace heartstead::world {

struct ChunkSubscriptionPolicy {
    static constexpr std::uint16_t max_radius_chunks = 32;
    static constexpr std::size_t hard_max_chunks_per_client = 4'096;

    std::uint16_t subscribe_horizontal_radius_chunks = 2;
    std::uint16_t subscribe_vertical_radius_chunks = 1;
    std::uint16_t retain_horizontal_radius_chunks = 3;
    std::uint16_t retain_vertical_radius_chunks = 2;
    std::size_t max_chunks_per_client = 128;
    std::size_t max_additions_per_update = 4;
    std::size_t max_removals_per_update = 16;

    [[nodiscard]] core::Status validate() const noexcept;
    [[nodiscard]] std::size_t desired_chunk_count() const noexcept;
};

struct ChunkSubscriptionTransitionBudget {
    std::size_t additions = 0;
    std::size_t removals = 0;
};

struct ChunkSubscriptionPlan {
    ChunkCoord center;
    std::size_t initial_subscription_count = 0;
    std::size_t desired_subscription_count = 0;
    std::size_t hysteresis_retained_count = 0;
    std::size_t deferred_addition_count = 0;
    std::size_t capacity_deferred_addition_count = 0;
    std::size_t deferred_removal_count = 0;
    std::vector<ChunkCoord> added_chunks;
    std::vector<ChunkCoord> removed_chunks;
    std::vector<ChunkCoord> subscriptions;

    [[nodiscard]] bool converged() const noexcept;
};

[[nodiscard]] core::Result<ChunkSubscriptionPlan>
plan_chunk_subscriptions(std::span<const ChunkCoord> existing_subscriptions, ChunkCoord center,
                         const ChunkSubscriptionPolicy& policy,
                         ChunkSubscriptionTransitionBudget budget);

} // namespace heartstead::world
