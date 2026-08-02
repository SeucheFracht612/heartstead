#include "engine/world/streaming/chunk_streaming_policy.hpp"

#include "engine/profiling/profiler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
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
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (value != 0 && value > maximum / value) {
        return maximum;
    }
    return value * value;
}

[[nodiscard]] std::uint64_t saturated_add(std::uint64_t left, std::uint64_t right) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (left > maximum - right) {
        return maximum;
    }
    return left + right;
}

[[nodiscard]] std::uint64_t squared_distance(ChunkCoord left, ChunkCoord right) noexcept {
    const auto dx = saturated_square(axis_distance(left.x, right.x));
    const auto dy = saturated_square(axis_distance(left.y, right.y));
    const auto dz = saturated_square(axis_distance(left.z, right.z));
    return saturated_add(saturated_add(dx, dy), dz);
}

[[nodiscard]] std::int64_t saturated_axis_offset(std::int64_t value, std::int64_t offset) noexcept {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (offset > 0 && value > maximum - offset) {
        return maximum;
    }
    if (offset < 0 && value < minimum - offset) {
        return minimum;
    }
    return value + offset;
}

[[nodiscard]] bool finite_non_negative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] bool motion_is_finite(const ChunkStreamViewerMotion& motion) noexcept {
    return std::isfinite(motion.velocity_x_blocks_per_second) &&
           std::isfinite(motion.velocity_y_blocks_per_second) &&
           std::isfinite(motion.velocity_z_blocks_per_second) &&
           std::isfinite(motion.view_direction_x) && std::isfinite(motion.view_direction_y) &&
           std::isfinite(motion.view_direction_z);
}

struct PredictedCenter {
    ChunkCoord current;
    ChunkCoord predicted;
};

[[nodiscard]] PredictedCenter predicted_center(const ChunkStreamViewerMotion& motion,
                                               const PredictiveChunkStreamingPolicy& policy) {
    const auto current = chunk_coord_for_simulation_coord(motion.viewer.coord);

    const auto velocity_length =
        std::hypot(motion.velocity_x_blocks_per_second, motion.velocity_y_blocks_per_second,
                   motion.velocity_z_blocks_per_second);
    const auto velocity_enabled = velocity_length >= policy.minimum_velocity_blocks_per_second;
    const auto edge = static_cast<double>(chunk_edge_length);
    auto delta_x = velocity_enabled ? motion.velocity_x_blocks_per_second *
                                          policy.prediction_horizon_seconds / edge
                                    : 0.0;
    auto delta_y = velocity_enabled ? motion.velocity_y_blocks_per_second *
                                          policy.prediction_horizon_seconds / edge
                                    : 0.0;
    auto delta_z = velocity_enabled ? motion.velocity_z_blocks_per_second *
                                          policy.prediction_horizon_seconds / edge
                                    : 0.0;

    const auto view_length =
        std::hypot(motion.view_direction_x, motion.view_direction_y, motion.view_direction_z);
    if (view_length > 0.0) {
        const auto lookahead = policy.camera_lookahead_chunks / view_length;
        delta_x += motion.view_direction_x * lookahead;
        delta_y += motion.view_direction_y * lookahead;
        delta_z += motion.view_direction_z * lookahead;
    }

    const auto prediction_length = std::hypot(delta_x, delta_y, delta_z);
    const auto maximum = static_cast<double>(policy.max_prediction_distance_chunks);
    if (prediction_length > maximum && prediction_length > 0.0) {
        const auto scale = maximum / prediction_length;
        delta_x *= scale;
        delta_y *= scale;
        delta_z *= scale;
    }

    const auto offset_x = static_cast<std::int64_t>(std::llround(delta_x));
    const auto offset_y = static_cast<std::int64_t>(std::llround(delta_y));
    const auto offset_z = static_cast<std::int64_t>(std::llround(delta_z));
    return {current,
            {saturated_axis_offset(current.x, offset_x), saturated_axis_offset(current.y, offset_y),
             saturated_axis_offset(current.z, offset_z)}};
}

struct CandidateRank {
    std::uint32_t trajectory_step = 0;
    std::uint64_t distance_to_prediction_squared = 0;
};

[[nodiscard]] bool better_rank(const CandidateRank& left, const CandidateRank& right) noexcept {
    return left.trajectory_step != right.trajectory_step
               ? left.trajectory_step < right.trajectory_step
               : left.distance_to_prediction_squared < right.distance_to_prediction_squared;
}

void add_prediction_cylinder(std::map<ChunkCoord, CandidateRank>& candidates, ChunkCoord center,
                             ChunkCoord prediction, std::uint32_t trajectory_step,
                             const PredictiveChunkStreamingPolicy& policy) {
    const auto horizontal = static_cast<std::int64_t>(policy.predictive_horizontal_radius_chunks);
    const auto vertical = static_cast<std::int64_t>(policy.predictive_vertical_radius_chunks);
    for (auto offset_z = -horizontal; offset_z <= horizontal; ++offset_z) {
        for (auto offset_x = -horizontal; offset_x <= horizontal; ++offset_x) {
            if (offset_x * offset_x + offset_z * offset_z > horizontal * horizontal) {
                continue;
            }
            for (auto offset_y = -vertical; offset_y <= vertical; ++offset_y) {
                const ChunkCoord coord{
                    saturated_axis_offset(center.x, offset_x),
                    saturated_axis_offset(center.y, offset_y),
                    saturated_axis_offset(center.z, offset_z),
                };
                const CandidateRank rank{trajectory_step, squared_distance(coord, prediction)};
                const auto found = candidates.find(coord);
                if (found == candidates.end()) {
                    candidates.emplace(coord, rank);
                } else if (better_rank(rank, found->second)) {
                    found->second = rank;
                }
            }
        }
    }
}

void add_prediction_trajectory(std::map<ChunkCoord, CandidateRank>& candidates,
                               const PredictedCenter& centers,
                               const PredictiveChunkStreamingPolicy& policy) {
    const auto delta_x = axis_distance(centers.current.x, centers.predicted.x);
    const auto delta_y = axis_distance(centers.current.y, centers.predicted.y);
    const auto delta_z = axis_distance(centers.current.z, centers.predicted.z);
    const auto steps = std::max({delta_x, delta_y, delta_z});
    if (steps == 0) {
        return;
    }

    const auto signed_delta_x = centers.predicted.x - centers.current.x;
    const auto signed_delta_y = centers.predicted.y - centers.current.y;
    const auto signed_delta_z = centers.predicted.z - centers.current.z;
    for (std::uint64_t step = 1; step <= steps; ++step) {
        const auto fraction = static_cast<double>(step) / static_cast<double>(steps);
        const ChunkCoord center{
            saturated_axis_offset(centers.current.x,
                                  static_cast<std::int64_t>(std::llround(
                                      static_cast<double>(signed_delta_x) * fraction))),
            saturated_axis_offset(centers.current.y,
                                  static_cast<std::int64_t>(std::llround(
                                      static_cast<double>(signed_delta_y) * fraction))),
            saturated_axis_offset(centers.current.z,
                                  static_cast<std::int64_t>(std::llround(
                                      static_cast<double>(signed_delta_z) * fraction))),
        };
        add_prediction_cylinder(candidates, center, centers.predicted,
                                static_cast<std::uint32_t>(step), policy);
    }
}

[[nodiscard]] std::uint64_t
nearest_viewer_distance_squared(ChunkCoord coord,
                                std::span<const ChunkCoord> viewer_chunks) noexcept {
    auto nearest = std::numeric_limits<std::uint64_t>::max();
    for (const auto viewer : viewer_chunks) {
        nearest = std::min(nearest, squared_distance(coord, viewer));
    }
    return nearest;
}

[[nodiscard]] bool dirty_for_eviction(const VoxelChunk& chunk) noexcept {
    return chunk.dirty().contains(ChunkDirtyFlag::save) ||
           chunk.dirty().contains(ChunkDirtyFlag::replication);
}

[[nodiscard]] simulation::WorldTick elapsed_ms(simulation::WorldTick now,
                                               simulation::WorldTick then) noexcept {
    return now >= then ? now - then : 0;
}

[[nodiscard]] std::size_t target_budget(const PredictiveChunkStreamingPolicy& policy,
                                        ChunkStreamMemoryPressure pressure) noexcept {
    switch (pressure) {
    case ChunkStreamMemoryPressure::nominal:
        return policy.nominal_resident_chunk_budget;
    case ChunkStreamMemoryPressure::elevated:
        return policy.elevated_resident_chunk_budget;
    case ChunkStreamMemoryPressure::critical:
        return policy.critical_resident_chunk_budget;
    }
    return policy.critical_resident_chunk_budget;
}

[[nodiscard]] double spatial_pressure_scale(ChunkStreamMemoryPressure pressure) noexcept {
    switch (pressure) {
    case ChunkStreamMemoryPressure::nominal:
        return 1.0;
    case ChunkStreamMemoryPressure::elevated:
        return 0.25;
    case ChunkStreamMemoryPressure::critical:
        return 0.0;
    }
    return 0.0;
}

[[nodiscard]] simulation::WorldTick
effective_temporal_retention(const PredictiveChunkStreamingPolicy& policy,
                             ChunkStreamMemoryPressure pressure) noexcept {
    switch (pressure) {
    case ChunkStreamMemoryPressure::nominal:
        return policy.temporal_retention_ms;
    case ChunkStreamMemoryPressure::elevated:
        return policy.temporal_retention_ms / 2;
    case ChunkStreamMemoryPressure::critical:
        return 0;
    }
    return 0;
}

} // namespace

core::Status PredictiveChunkStreamingPolicy::validate() const {
    auto status = interest.validate();
    if (!status) {
        return status;
    }
    if (!finite_non_negative(prediction_horizon_seconds) ||
        !finite_non_negative(camera_lookahead_chunks) ||
        !finite_non_negative(minimum_velocity_blocks_per_second)) {
        return core::Status::failure("chunk_stream_policy.invalid_prediction",
                                     "prediction horizon, camera lookahead, and minimum velocity "
                                     "must be finite and non-negative");
    }
    if (max_prediction_distance_chunks > ChunkStreamInterestPolicy::max_load_radius_chunks ||
        predictive_horizontal_radius_chunks > ChunkStreamInterestPolicy::max_load_radius_chunks ||
        predictive_vertical_radius_chunks > ChunkStreamInterestPolicy::max_load_radius_chunks) {
        return core::Status::failure("chunk_stream_policy.prediction_radius_too_large",
                                     "predictive streaming radii exceed the bounded planner limit");
    }
    if (max_speculative_submissions_per_update == 0 || max_active_speculative_requests == 0 ||
        max_speculative_submissions_per_update > max_active_speculative_requests ||
        speculative_ttl_ms == 0) {
        return core::Status::failure("chunk_stream_policy.invalid_speculation_budget",
                                     "speculative submission, active-request, and lifetime budgets "
                                     "must be positive and ordered");
    }
    if (nominal_resident_chunk_budget == 0 || elevated_resident_chunk_budget == 0 ||
        critical_resident_chunk_budget == 0 ||
        nominal_resident_chunk_budget < elevated_resident_chunk_budget ||
        elevated_resident_chunk_budget < critical_resident_chunk_budget) {
        return core::Status::failure(
            "chunk_stream_policy.invalid_residency_budget",
            "resident chunk budgets must be positive and non-increasing with memory pressure");
    }
    if (!finite_non_negative(estimated_reload_cost_ms) ||
        !finite_non_negative(spatial_retention_value) ||
        !finite_non_negative(temporal_retention_value) ||
        !finite_non_negative(speculative_value_penalty) ||
        !finite_non_negative(distance_value_penalty_per_chunk)) {
        return core::Status::failure("chunk_stream_policy.invalid_eviction_value",
                                     "eviction value inputs must be finite and non-negative");
    }
    return core::Status::ok();
}

core::Result<PredictiveChunkStreamPlan> PredictiveChunkStreamingPlanner::plan(
    const WorldState& state, std::span<const ChunkStreamViewerMotion> viewers,
    std::span<const ChunkCoord> pending_loads, const PredictiveChunkStreamingPolicy& policy,
    ChunkStreamMemoryPressure pressure, simulation::WorldTick now_ms) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("streaming.predictive_policy");
    auto status = policy.validate();
    if (!status) {
        return core::Result<PredictiveChunkStreamPlan>::failure(status.error().code,
                                                                status.error().message);
    }
    if (initialized_ && now_ms < last_observed_time_ms_) {
        return core::Result<PredictiveChunkStreamPlan>::failure(
            "chunk_stream_policy.time_reversed", "predictive streaming time cannot move backward");
    }

    std::vector<simulation::SimulationViewer> immediate_viewers;
    std::vector<ChunkCoord> viewer_chunks;
    immediate_viewers.reserve(viewers.size());
    viewer_chunks.reserve(viewers.size());
    for (const auto& motion : viewers) {
        if (!motion_is_finite(motion)) {
            return core::Result<PredictiveChunkStreamPlan>::failure(
                "chunk_stream_policy.invalid_viewer_motion",
                "viewer velocity and view direction must be finite");
        }
        immediate_viewers.push_back(motion.viewer);
        viewer_chunks.push_back(chunk_coord_for_simulation_coord(motion.viewer.coord));
    }

    auto immediate = ChunkStreamer::plan_interest(state, immediate_viewers, policy.interest);
    if (!immediate) {
        return core::Result<PredictiveChunkStreamPlan>::failure(immediate.error().code,
                                                                immediate.error().message);
    }

    PredictiveChunkStreamPlan plan;
    plan.immediate = std::move(immediate).value();
    plan.memory_pressure = pressure;
    plan.target_resident_chunk_count = target_budget(policy, pressure);
    plan.teleport_mode = std::ranges::any_of(viewers, &ChunkStreamViewerMotion::teleport);
    ++stats_.planning_updates;
    if (plan.teleport_mode) {
        ++stats_.teleport_updates;
    }

    const std::set<ChunkCoord> required(plan.immediate.desired_chunks.begin(),
                                        plan.immediate.desired_chunks.end());
    for (const auto coord : required) {
        last_required_at_ms_[coord] = now_ms;
    }

    if (initialized_) {
        for (const auto coord : required) {
            if (std::ranges::binary_search(previous_required_, coord)) {
                continue;
            }
            ++stats_.demand_transitions;
            const auto found = speculative_.find(coord);
            if (found == speculative_.end()) {
                continue;
            }
            if (found->second.waste_counted ||
                elapsed_ms(now_ms, found->second.submitted_at_ms) > policy.speculative_ttl_ms) {
                if (!found->second.waste_counted) {
                    ++stats_.wasted_predictions;
                }
                speculative_.erase(found);
                continue;
            }
            ++stats_.useful_predictions;
            if (found->second.state == SpeculativeState::published) {
                ++stats_.timely_prefetch_hits;
                const auto timeliness = elapsed_ms(now_ms, found->second.published_at_ms);
                stats_.cumulative_timeliness_ms += timeliness;
                stats_.maximum_timeliness_ms = std::max(stats_.maximum_timeliness_ms, timeliness);
            } else {
                ++stats_.late_prefetch_hits;
            }
            speculative_.erase(found);
        }
    }
    previous_required_.assign(required.begin(), required.end());

    std::map<ChunkCoord, CandidateRank> predicted;
    if (pressure != ChunkStreamMemoryPressure::critical) {
        for (const auto& motion : viewers) {
            if (motion.teleport) {
                continue;
            }
            add_prediction_trajectory(predicted, predicted_center(motion, policy), policy);
        }
    }
    for (const auto coord : required) {
        predicted.erase(coord);
    }
    plan.predicted_chunks.reserve(predicted.size());
    for (const auto& [coord, _] : predicted) {
        plan.predicted_chunks.push_back(coord);
    }
    const std::set<ChunkCoord> predicted_set(plan.predicted_chunks.begin(),
                                             plan.predicted_chunks.end());

    std::set<ChunkCoord> invalidated_resident;
    for (auto iterator = speculative_.begin(); iterator != speculative_.end();) {
        auto& record = iterator->second;
        if (record.submitted_at_ms > now_ms) {
            return core::Result<PredictiveChunkStreamPlan>::failure(
                "chunk_stream_policy.time_reversed",
                "predictive streaming time precedes a speculative submission");
        }
        const auto expired = elapsed_ms(now_ms, record.submitted_at_ms) > policy.speculative_ttl_ms;
        const auto still_predicted = predicted_set.contains(iterator->first);
        if (!record.invalidated && (expired || !still_predicted)) {
            record.invalidated = true;
            if (!record.waste_counted) {
                record.waste_counted = true;
                ++stats_.wasted_predictions;
            }
            if (record.state == SpeculativeState::published) {
                invalidated_resident.insert(iterator->first);
                iterator = speculative_.erase(iterator);
                continue;
            }
            record.state = SpeculativeState::cancellation_requested;
            ++stats_.cancellation_requests;
            plan.speculative_cancellations.push_back(iterator->first);
        } else if (record.invalidated && record.state == SpeculativeState::published) {
            invalidated_resident.insert(iterator->first);
            iterator = speculative_.erase(iterator);
            continue;
        }
        ++iterator;
    }

    std::set<ChunkCoord> loaded;
    for (const auto* chunk : state.chunks().records()) {
        loaded.insert(chunk->coord());
    }
    const std::set<ChunkCoord> pending(pending_loads.begin(), pending_loads.end());

    std::vector<ChunkSpeculativeLoadCandidate> candidates;
    candidates.reserve(predicted.size());
    for (const auto& [coord, rank] : predicted) {
        if (loaded.contains(coord) || pending.contains(coord) || speculative_.contains(coord)) {
            continue;
        }
        candidates.push_back({coord, rank.trajectory_step, rank.distance_to_prediction_squared});
    }
    std::ranges::sort(candidates, [](const auto& left, const auto& right) {
        if (left.trajectory_step != right.trajectory_step) {
            return left.trajectory_step < right.trajectory_step;
        }
        if (left.distance_to_prediction_squared != right.distance_to_prediction_squared) {
            return left.distance_to_prediction_squared < right.distance_to_prediction_squared;
        }
        return left.coord < right.coord;
    });
    auto submission_budget = policy.max_speculative_submissions_per_update;
    if (pressure == ChunkStreamMemoryPressure::elevated) {
        submission_budget = (submission_budget + 1) / 2;
    } else if (pressure == ChunkStreamMemoryPressure::critical) {
        submission_budget = 0;
    }
    const auto active_capacity = speculative_.size() < policy.max_active_speculative_requests
                                     ? policy.max_active_speculative_requests - speculative_.size()
                                     : 0;
    const auto candidate_count = std::min({candidates.size(), submission_budget, active_capacity});
    candidates.resize(candidate_count);
    plan.speculative_loads = std::move(candidates);
    offered_speculation_.clear();
    offered_speculation_.reserve(plan.speculative_loads.size());
    for (const auto& candidate : plan.speculative_loads) {
        offered_speculation_.push_back(candidate.coord);
    }

    std::set<ChunkCoord> scheduler_interest(required.begin(), required.end());
    scheduler_interest.insert(predicted_set.begin(), predicted_set.end());
    plan.scheduler_interest.assign(scheduler_interest.begin(), scheduler_interest.end());

    const std::set<ChunkCoord> spatially_retained(plan.immediate.retained_chunks.begin(),
                                                  plan.immediate.retained_chunks.end());
    const auto temporal_window = effective_temporal_retention(policy, pressure);
    const auto spatial_scale = spatial_pressure_scale(pressure);
    std::set<ChunkCoord> mandatory_evictions;

    for (const auto* chunk : state.chunks().records()) {
        const auto coord = chunk->coord();
        if (required.contains(coord) || dirty_for_eviction(*chunk)) {
            continue;
        }

        ChunkStreamEvictionValue value;
        value.coord = coord;
        value.reload_cost_value = policy.estimated_reload_cost_ms;
        value.spatially_retained = spatially_retained.contains(coord);
        value.spatial_retention_bonus =
            value.spatially_retained ? policy.spatial_retention_value * spatial_scale : 0.0;
        const auto recent = last_required_at_ms_.find(coord);
        if (recent != last_required_at_ms_.end()) {
            value.age_since_required_ms = elapsed_ms(now_ms, recent->second);
            value.temporally_retained =
                temporal_window != 0 && value.age_since_required_ms <= temporal_window;
            if (value.temporally_retained) {
                const auto remaining = 1.0 - static_cast<double>(value.age_since_required_ms) /
                                                 static_cast<double>(temporal_window);
                value.temporal_retention_bonus = policy.temporal_retention_value * remaining;
            }
        } else {
            value.age_since_required_ms = std::numeric_limits<simulation::WorldTick>::max();
        }
        value.speculative = predicted_set.contains(coord) || speculative_.contains(coord);
        value.speculative_penalty = value.speculative ? policy.speculative_value_penalty : 0.0;
        value.nearest_viewer_distance_squared =
            nearest_viewer_distance_squared(coord, viewer_chunks);
        value.distance_penalty =
            policy.distance_value_penalty_per_chunk *
            std::sqrt(static_cast<double>(value.nearest_viewer_distance_squared));
        value.value = value.reload_cost_value + value.spatial_retention_bonus +
                      value.temporal_retention_bonus - value.speculative_penalty -
                      value.distance_penalty;

        if (invalidated_resident.contains(coord) ||
            (!value.spatially_retained && !value.temporally_retained && !value.speculative)) {
            mandatory_evictions.insert(coord);
        }
        plan.ranked_eviction_candidates.push_back(value);
    }

    std::ranges::sort(plan.ranked_eviction_candidates, [](const auto& left, const auto& right) {
        return left.value != right.value ? left.value < right.value : left.coord < right.coord;
    });

    std::set<ChunkCoord> selected = mandatory_evictions;
    auto projected_resident_count =
        loaded.size() > selected.size() ? loaded.size() - selected.size() : 0;
    for (const auto& candidate : plan.ranked_eviction_candidates) {
        if (projected_resident_count <= plan.target_resident_chunk_count) {
            break;
        }
        if (selected.insert(candidate.coord).second) {
            --projected_resident_count;
        }
    }
    for (auto& candidate : plan.ranked_eviction_candidates) {
        candidate.selected = selected.contains(candidate.coord);
        candidate.pressure_override =
            candidate.selected && !mandatory_evictions.contains(candidate.coord);
        if (candidate.selected) {
            plan.eviction_requests.push_back(candidate.coord);
        }
    }
    plan.unresolved_resident_overage =
        projected_resident_count > plan.target_resident_chunk_count
            ? projected_resident_count - plan.target_resident_chunk_count
            : 0;

    const auto history_window = std::max(policy.temporal_retention_ms, policy.speculative_ttl_ms);
    for (auto iterator = last_required_at_ms_.begin(); iterator != last_required_at_ms_.end();) {
        if (!required.contains(iterator->first) &&
            elapsed_ms(now_ms, iterator->second) > history_window) {
            iterator = last_required_at_ms_.erase(iterator);
        } else {
            ++iterator;
        }
    }

    last_observed_time_ms_ = now_ms;
    initialized_ = true;
    return core::Result<PredictiveChunkStreamPlan>::success(std::move(plan));
}

core::Status
PredictiveChunkStreamingPlanner::note_speculative_submitted(ChunkCoord coord,
                                                            simulation::WorldTick now_ms) {
    if (initialized_ && now_ms < last_observed_time_ms_) {
        return core::Status::failure("chunk_stream_policy.time_reversed",
                                     "speculative submission time cannot precede planning time");
    }
    if (speculative_.contains(coord)) {
        return core::Status::failure("chunk_stream_policy.duplicate_speculation",
                                     "chunk already has tracked speculative work");
    }
    const auto offered = std::ranges::find(offered_speculation_, coord);
    if (offered == offered_speculation_.end()) {
        return core::Status::failure("chunk_stream_policy.unoffered_speculation",
                                     "chunk was not offered by the latest predictive plan");
    }
    offered_speculation_.erase(offered);
    speculative_.emplace(coord, SpeculativeRecord{now_ms});
    ++stats_.speculative_submissions;
    last_observed_time_ms_ = now_ms;
    return core::Status::ok();
}

core::Status PredictiveChunkStreamingPlanner::note_speculative_outcome(
    ChunkCoord coord, ChunkSpeculativeLoadOutcome outcome, simulation::WorldTick now_ms) {
    const auto found = speculative_.find(coord);
    if (found == speculative_.end()) {
        return core::Status::failure("chunk_stream_policy.unknown_speculation",
                                     "chunk has no tracked speculative work");
    }
    auto& record = found->second;
    if (now_ms < record.submitted_at_ms || (initialized_ && now_ms < last_observed_time_ms_)) {
        return core::Status::failure("chunk_stream_policy.time_reversed",
                                     "speculative outcome time precedes an observed policy event");
    }

    switch (outcome) {
    case ChunkSpeculativeLoadOutcome::published:
        if (record.state == SpeculativeState::published) {
            return core::Status::failure("chunk_stream_policy.duplicate_publication",
                                         "speculative chunk was already published");
        }
        if (record.state == SpeculativeState::cancellation_requested) {
            ++stats_.cancellation_misses;
        }
        record.state = SpeculativeState::published;
        record.published_at_ms = now_ms;
        ++stats_.speculative_publications;
        break;
    case ChunkSpeculativeLoadOutcome::cancelled:
        if (!record.waste_counted) {
            record.waste_counted = true;
            ++stats_.wasted_predictions;
        }
        ++stats_.cancelled_requests;
        speculative_.erase(found);
        break;
    case ChunkSpeculativeLoadOutcome::failed:
        if (!record.waste_counted) {
            record.waste_counted = true;
            ++stats_.wasted_predictions;
        }
        ++stats_.failed_requests;
        speculative_.erase(found);
        break;
    case ChunkSpeculativeLoadOutcome::stale:
        if (!record.waste_counted) {
            record.waste_counted = true;
            ++stats_.wasted_predictions;
        }
        ++stats_.stale_results;
        speculative_.erase(found);
        break;
    }
    last_observed_time_ms_ = now_ms;
    return core::Status::ok();
}

PredictiveChunkStreamingStats PredictiveChunkStreamingPlanner::stats() const noexcept {
    auto result = stats_;
    result.active_speculative_requests = speculative_.size();
    const auto resolved = result.useful_predictions + result.wasted_predictions;
    result.prediction_accuracy = resolved == 0 ? 0.0
                                               : static_cast<double>(result.useful_predictions) /
                                                     static_cast<double>(resolved);
    result.timely_coverage = result.demand_transitions == 0
                                 ? 0.0
                                 : static_cast<double>(result.timely_prefetch_hits) /
                                       static_cast<double>(result.demand_transitions);
    result.mean_timeliness_ms = result.timely_prefetch_hits == 0
                                    ? 0.0
                                    : static_cast<double>(result.cumulative_timeliness_ms) /
                                          static_cast<double>(result.timely_prefetch_hits);
    return result;
}

void PredictiveChunkStreamingPlanner::reset() noexcept {
    speculative_.clear();
    last_required_at_ms_.clear();
    previous_required_.clear();
    offered_speculation_.clear();
    stats_ = {};
    last_observed_time_ms_ = 0;
    initialized_ = false;
}

const char* chunk_stream_memory_pressure_name(ChunkStreamMemoryPressure pressure) noexcept {
    switch (pressure) {
    case ChunkStreamMemoryPressure::nominal:
        return "nominal";
    case ChunkStreamMemoryPressure::elevated:
        return "elevated";
    case ChunkStreamMemoryPressure::critical:
        return "critical";
    }
    return "unknown";
}

} // namespace heartstead::world
