#include "engine/world/streaming/chunk_streaming_policy.hpp"
#include "engine/world/streaming/predictive_chunk_streaming_controller.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace heartstead;

[[nodiscard]] world::PredictiveChunkStreamingPolicy compact_policy() {
    world::PredictiveChunkStreamingPolicy policy;
    policy.interest.load_horizontal_radius_chunks = 0;
    policy.interest.load_vertical_radius_chunks = 0;
    policy.interest.retain_horizontal_radius_chunks = 1;
    policy.interest.retain_vertical_radius_chunks = 0;
    policy.prediction_horizon_seconds = 1.0;
    policy.camera_lookahead_chunks = 0.0;
    policy.max_prediction_distance_chunks = 4;
    policy.predictive_horizontal_radius_chunks = 0;
    policy.predictive_vertical_radius_chunks = 0;
    policy.max_speculative_submissions_per_update = 4;
    policy.max_active_speculative_requests = 8;
    policy.max_evictions_per_update = 8;
    policy.speculative_ttl_ms = 1'000;
    policy.temporal_retention_ms = 1'000;
    policy.nominal_resident_chunk_budget = 10;
    policy.elevated_resident_chunk_budget = 3;
    policy.critical_resident_chunk_budget = 1;
    return policy;
}

[[nodiscard]] world::ChunkStreamViewerMotion moving_viewer(std::int64_t block_x,
                                                           double velocity_x) {
    world::ChunkStreamViewerMotion motion;
    motion.viewer.viewer_id = core::NetId::from_value(1);
    motion.viewer.coord = {block_x, 0, 0};
    motion.velocity_x_blocks_per_second = velocity_x;
    return motion;
}

[[nodiscard]] bool contains(std::span<const world::ChunkCoord> coords, world::ChunkCoord coord) {
    return std::ranges::find(coords, coord) != coords.end();
}

class AirChunkGenerator final : public world::IChunkLoadGenerator {
  public:
    [[nodiscard]] core::Result<world::VoxelChunk> generate(world::ChunkCoord coord) const override {
        if (coord != (world::ChunkCoord{0, 0, 0})) {
            while (!release_non_origin.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        return core::Result<world::VoxelChunk>::success(world::VoxelChunk(coord));
    }

    std::atomic_bool release_non_origin = false;
};

void test_directional_prediction_and_timely_hit_metrics() {
    world::WorldState state;
    world::PredictiveChunkStreamingPlanner planner;
    const auto policy = compact_policy();
    const std::vector<world::ChunkStreamViewerMotion> first_viewers{moving_viewer(0, 64.0)};

    auto first = planner.plan(state, first_viewers, {}, policy,
                              world::ChunkStreamMemoryPressure::nominal, 0);
    assert(first);
    assert((first.value().immediate.desired_chunks == std::vector<world::ChunkCoord>{{0, 0, 0}}));
    assert(first.value().speculative_loads.size() == 2);
    assert(first.value().speculative_loads[0].coord == (world::ChunkCoord{1, 0, 0}));
    assert(first.value().speculative_loads[1].coord == (world::ChunkCoord{2, 0, 0}));
    assert(contains(first.value().scheduler_interest, {0, 0, 0}));
    assert(contains(first.value().scheduler_interest, {1, 0, 0}));
    assert(contains(first.value().scheduler_interest, {2, 0, 0}));

    assert(planner.note_speculative_submitted({1, 0, 0}, 0));
    assert(planner.note_speculative_outcome({1, 0, 0},
                                            world::ChunkSpeculativeLoadOutcome::published, 10));

    const std::vector<world::ChunkStreamViewerMotion> advanced_viewers{moving_viewer(32, 64.0)};
    auto advanced = planner.plan(state, advanced_viewers, {}, policy,
                                 world::ChunkStreamMemoryPressure::nominal, 100);
    assert(advanced);
    const auto stats = planner.stats();
    assert(stats.planning_updates == 2);
    assert(stats.demand_transitions == 1);
    assert(stats.speculative_submissions == 1);
    assert(stats.speculative_publications == 1);
    assert(stats.useful_predictions == 1);
    assert(stats.timely_prefetch_hits == 1);
    assert(stats.late_prefetch_hits == 0);
    assert(stats.wasted_predictions == 0);
    assert(stats.active_speculative_requests == 0);
    assert(stats.prediction_accuracy == 1.0);
    assert(stats.timely_coverage == 1.0);
    assert(stats.mean_timeliness_ms == 90.0);
    assert(stats.maximum_timeliness_ms == 90);
}

void test_camera_prediction_and_multi_viewer_deduplication() {
    world::WorldState state;
    world::PredictiveChunkStreamingPlanner planner;
    auto policy = compact_policy();
    policy.camera_lookahead_chunks = 2.0;

    auto first = moving_viewer(0, 0.0);
    first.view_direction_z = 1.0;
    auto second = first;
    second.viewer.viewer_id = core::NetId::from_value(2);
    const std::vector<world::ChunkStreamViewerMotion> viewers{first, second};
    auto plan =
        planner.plan(state, viewers, {}, policy, world::ChunkStreamMemoryPressure::nominal, 0);
    assert(plan);
    assert(plan.value().speculative_loads.size() == 2);
    assert(plan.value().speculative_loads[0].coord == (world::ChunkCoord{0, 0, 1}));
    assert(plan.value().speculative_loads[1].coord == (world::ChunkCoord{0, 0, 2}));
}

void test_prediction_extends_the_future_demand_footprint() {
    world::WorldState state;
    world::PredictiveChunkStreamingPlanner planner;
    auto policy = compact_policy();
    policy.interest.load_horizontal_radius_chunks = 1;
    policy.interest.retain_horizontal_radius_chunks = 2;
    policy.predictive_horizontal_radius_chunks = 0;
    policy.camera_lookahead_chunks = 0.0;
    const std::vector<world::ChunkStreamViewerMotion> viewers{moving_viewer(0, 64.0)};

    auto plan =
        planner.plan(state, viewers, {}, policy, world::ChunkStreamMemoryPressure::nominal, 0);
    assert(plan);
    assert(contains(plan.value().immediate.desired_chunks, {1, 0, 0}));
    assert(contains(plan.value().predicted_chunks, {2, 0, 0}));
    assert(!contains(plan.value().immediate.desired_chunks, {2, 0, 0}));
    assert(std::ranges::any_of(plan.value().speculative_loads, [](const auto& candidate) {
        return candidate.coord == (world::ChunkCoord{2, 0, 0});
    }));
}

void test_reversal_teleport_and_actual_cancellation_metrics() {
    world::WorldState state;
    world::PredictiveChunkStreamingPlanner planner;
    const auto policy = compact_policy();
    const std::vector<world::ChunkStreamViewerMotion> forward{moving_viewer(0, 64.0)};
    auto first =
        planner.plan(state, forward, {}, policy, world::ChunkStreamMemoryPressure::nominal, 0);
    assert(first);
    assert(planner.note_speculative_submitted({1, 0, 0}, 0));

    const std::vector<world::ChunkStreamViewerMotion> reverse{moving_viewer(0, -64.0)};
    auto reversed =
        planner.plan(state, reverse, {}, policy, world::ChunkStreamMemoryPressure::nominal, 10);
    assert(reversed);
    assert(
        (reversed.value().speculative_cancellations == std::vector<world::ChunkCoord>{{1, 0, 0}}));
    auto stats = planner.stats();
    assert(stats.wasted_predictions == 1);
    assert(stats.cancellation_requests == 1);
    assert(stats.cancelled_requests == 0);
    assert(planner.note_speculative_outcome({1, 0, 0},
                                            world::ChunkSpeculativeLoadOutcome::cancelled, 11));
    stats = planner.stats();
    assert(stats.cancelled_requests == 1);
    assert(stats.active_speculative_requests == 0);

    assert(!reversed.value().speculative_loads.empty());
    const auto second_coord = reversed.value().speculative_loads.front().coord;
    assert(planner.note_speculative_submitted(second_coord, 12));
    auto teleported_motion = moving_viewer(3'200, 64.0);
    teleported_motion.teleport = true;
    const std::vector<world::ChunkStreamViewerMotion> teleported{teleported_motion};
    auto teleport =
        planner.plan(state, teleported, {}, policy, world::ChunkStreamMemoryPressure::nominal, 20);
    assert(teleport);
    assert(teleport.value().teleport_mode);
    assert(teleport.value().predicted_chunks.empty());
    assert(teleport.value().speculative_loads.empty());
    assert(teleport.value().speculative_cancellations ==
           std::vector<world::ChunkCoord>{second_coord});
    stats = planner.stats();
    assert(stats.teleport_updates == 1);
    assert(stats.wasted_predictions == 2);
    assert(stats.cancellation_requests == 2);

    assert(planner.note_speculative_outcome(second_coord,
                                            world::ChunkSpeculativeLoadOutcome::published, 21));
    state.chunks().get_or_create(second_coord).clear_all_dirty();
    stats = planner.stats();
    assert(stats.cancellation_misses == 1);
    auto cleanup =
        planner.plan(state, teleported, {}, policy, world::ChunkStreamMemoryPressure::nominal, 22);
    assert(cleanup);
    assert(contains(cleanup.value().eviction_requests, second_coord));
    assert(planner.stats().active_speculative_requests == 1);
    auto evicted = world::ChunkStreamer::evict_chunks(state, cleanup.value().eviction_requests);
    assert(evicted.evicted_count() == 1);
    auto reconciled = planner.plan(state, teleported, {}, policy,
                                   world::ChunkStreamMemoryPressure::nominal, 23);
    assert(reconciled);
    assert(planner.stats().active_speculative_requests == 0);
}

void test_pressure_aware_temporal_retention_and_eviction_value() {
    world::WorldState state;
    for (const auto coord :
         std::vector<world::ChunkCoord>{{0, 0, 0}, {1, 0, 0}, {4, 0, 0}, {5, 0, 0}, {8, 0, 0}}) {
        state.chunks().get_or_create(coord).clear_all_dirty();
    }
    state.chunks().find({8, 0, 0})->mark_dirty(world::ChunkDirtyFlag::save);

    auto policy = compact_policy();
    policy.prediction_horizon_seconds = 0.0;
    world::PredictiveChunkStreamingPlanner planner;
    const std::vector<world::ChunkStreamViewerMotion> at_five{moving_viewer(160, 0.0)};
    auto history =
        planner.plan(state, at_five, {}, policy, world::ChunkStreamMemoryPressure::nominal, 0);
    assert(history);

    const std::vector<world::ChunkStreamViewerMotion> at_zero{moving_viewer(0, 0.0)};
    auto nominal =
        planner.plan(state, at_zero, {}, policy, world::ChunkStreamMemoryPressure::nominal, 100);
    assert(nominal);
    assert(contains(nominal.value().eviction_requests, {4, 0, 0}));
    assert(!contains(nominal.value().eviction_requests, {5, 0, 0}));
    assert(!contains(nominal.value().eviction_requests, {8, 0, 0}));
    const auto recent =
        std::ranges::find_if(nominal.value().ranked_eviction_candidates, [](const auto& candidate) {
            return candidate.coord == (world::ChunkCoord{5, 0, 0});
        });
    assert(recent != nominal.value().ranked_eviction_candidates.end());
    assert(recent->temporally_retained);
    assert(recent->temporal_retention_bonus > 0.0);
    assert(!recent->selected);

    auto elevated =
        planner.plan(state, at_zero, {}, policy, world::ChunkStreamMemoryPressure::elevated, 101);
    assert(elevated);
    assert(elevated.value().target_resident_chunk_count == 3);
    assert(elevated.value().eviction_requests.size() == 2);
    assert(contains(elevated.value().eviction_requests, {4, 0, 0}));
    assert(contains(elevated.value().eviction_requests, {1, 0, 0}));
    assert(!contains(elevated.value().eviction_requests, {5, 0, 0}));
    const auto pressure_override = std::ranges::find_if(
        elevated.value().ranked_eviction_candidates,
        [](const auto& candidate) { return candidate.coord == (world::ChunkCoord{1, 0, 0}); });
    assert(pressure_override != elevated.value().ranked_eviction_candidates.end());
    assert(pressure_override->spatially_retained);
    assert(pressure_override->selected);
    assert(pressure_override->pressure_override);

    auto critical =
        planner.plan(state, at_zero, {}, policy, world::ChunkStreamMemoryPressure::critical, 102);
    assert(critical);
    assert(critical.value().target_resident_chunk_count == 1);
    assert(critical.value().predicted_chunks.empty());
    assert(critical.value().eviction_requests.size() == 3);
    assert(critical.value().deferred_eviction_count == 0);
    assert(critical.value().projected_resident_overage == 1);
    assert(critical.value().unresolved_resident_overage == 1);
    assert(!contains(critical.value().eviction_requests, {0, 0, 0}));
    assert(!contains(critical.value().eviction_requests, {8, 0, 0}));
}

void test_eviction_waves_are_bounded_and_report_deferred_work() {
    world::WorldState state;
    for (std::int64_t x = 0; x < 7; ++x) {
        state.chunks().get_or_create({x, 0, 0}).clear_all_dirty();
    }

    auto policy = compact_policy();
    policy.interest.retain_horizontal_radius_chunks = 0;
    policy.temporal_retention_ms = 1;
    policy.nominal_resident_chunk_budget = 1;
    policy.elevated_resident_chunk_budget = 1;
    policy.critical_resident_chunk_budget = 1;
    policy.max_evictions_per_update = 2;
    policy.prediction_horizon_seconds = 0.0;
    world::PredictiveChunkStreamingPlanner planner;
    const std::vector<world::ChunkStreamViewerMotion> viewers{moving_viewer(0, 0.0)};

    auto planned =
        planner.plan(state, viewers, {}, policy, world::ChunkStreamMemoryPressure::critical, 0);
    assert(planned);
    assert(planned.value().eviction_requests.size() == 2);
    assert(planned.value().deferred_eviction_count == 4);
    assert(planned.value().projected_resident_overage == 4);
    assert(planned.value().unresolved_resident_overage == 0);
    assert(std::ranges::count_if(planned.value().ranked_eviction_candidates,
                                 [](const auto& candidate) { return candidate.selected; }) == 2);

    auto evicted = world::ChunkStreamer::evict_chunks(state, planned.value().eviction_requests);
    assert(evicted.evicted_count() == 2);
    auto next =
        planner.plan(state, viewers, {}, policy, world::ChunkStreamMemoryPressure::critical, 1);
    assert(next);
    assert(next.value().eviction_requests.size() == 2);
    assert(next.value().deferred_eviction_count == 2);
    assert(next.value().projected_resident_overage == 2);
    assert(next.value().unresolved_resident_overage == 0);
}

void test_required_chunk_salvages_a_late_cancellation_publication() {
    world::WorldState state;
    world::PredictiveChunkStreamingPlanner planner;
    const auto policy = compact_policy();
    const std::vector<world::ChunkStreamViewerMotion> forward{moving_viewer(0, 64.0)};
    auto first =
        planner.plan(state, forward, {}, policy, world::ChunkStreamMemoryPressure::nominal, 0);
    assert(first);
    assert(planner.note_speculative_submitted({1, 0, 0}, 0));

    const std::vector<world::ChunkStreamViewerMotion> reverse{moving_viewer(0, -64.0)};
    auto reversed =
        planner.plan(state, reverse, {}, policy, world::ChunkStreamMemoryPressure::nominal, 10);
    assert(reversed);
    assert(planner.note_speculative_outcome({1, 0, 0},
                                            world::ChunkSpeculativeLoadOutcome::published, 11));
    state.chunks().get_or_create({1, 0, 0}).clear_all_dirty();

    const std::vector<world::ChunkStreamViewerMotion> arrived{moving_viewer(32, 0.0)};
    auto required =
        planner.plan(state, arrived, {}, policy, world::ChunkStreamMemoryPressure::nominal, 12);
    assert(required);
    assert(!contains(required.value().eviction_requests, {1, 0, 0}));
    const auto stats = planner.stats();
    assert(stats.wasted_predictions == 1);
    assert(stats.useful_predictions == 0);
    assert(stats.cancellation_misses == 1);
    assert(stats.active_speculative_requests == 0);
}

void test_policy_and_motion_validation() {
    auto policy = compact_policy();
    policy.elevated_resident_chunk_budget = policy.nominal_resident_chunk_budget + 1;
    auto status = policy.validate();
    assert(!status);
    assert(status.error().code == "chunk_stream_policy.invalid_residency_budget");

    policy = compact_policy();
    policy.max_evictions_per_update = 0;
    status = policy.validate();
    assert(!status);
    assert(status.error().code == "chunk_stream_policy.invalid_speculation_budget");

    policy = compact_policy();
    policy.interest.load_horizontal_radius_chunks =
        world::ChunkStreamInterestPolicy::max_load_radius_chunks;
    policy.interest.retain_horizontal_radius_chunks =
        world::ChunkStreamInterestPolicy::max_load_radius_chunks;
    policy.predictive_horizontal_radius_chunks = 1;
    status = policy.validate();
    assert(!status);
    assert(status.error().code == "chunk_stream_policy.prediction_radius_too_large");

    policy = compact_policy();
    auto invalid_motion = moving_viewer(0, 0.0);
    invalid_motion.view_direction_x = std::numeric_limits<double>::quiet_NaN();
    world::PredictiveChunkStreamingPlanner planner;
    const std::vector<world::ChunkStreamViewerMotion> viewers{invalid_motion};
    auto plan = planner.plan(world::WorldState{}, viewers, {}, policy,
                             world::ChunkStreamMemoryPressure::nominal, 0);
    assert(!plan);
    assert(plan.error().code == "chunk_stream_policy.invalid_viewer_motion");

    world::PredictiveChunkStreamingPlanner valid_planner;
    const std::vector<world::ChunkStreamViewerMotion> valid_viewers{moving_viewer(0, 64.0)};
    auto valid_plan = valid_planner.plan(world::WorldState{}, valid_viewers, {}, policy,
                                         world::ChunkStreamMemoryPressure::nominal, 0);
    assert(valid_plan);
    status = valid_planner.note_speculative_submitted({99, 0, 0}, 0);
    assert(!status);
    assert(status.error().code == "chunk_stream_policy.unoffered_speculation");
    const auto offered_coord = valid_plan.value().speculative_loads.front().coord;
    assert(valid_planner.note_speculative_submitted(offered_coord, 2));
    auto reversed_time = valid_planner.plan(world::WorldState{}, valid_viewers, {}, policy,
                                            world::ChunkStreamMemoryPressure::nominal, 1);
    assert(!reversed_time);
    assert(reversed_time.error().code == "chunk_stream_policy.time_reversed");
}

void test_controller_prioritizes_required_work_and_reconciles_cancellation() {
    world::ChunkLoadSchedulerContext context;
    auto generator = std::make_shared<AirChunkGenerator>();
    context.generator = generator;
    world::ChunkLoadSchedulerConfig scheduler_config;
    scheduler_config.worker_count = 1;
    scheduler_config.max_concurrent_requests = 2;
    scheduler_config.max_completed_results = 2;
    scheduler_config.reservation_bytes_per_request = 1;
    scheduler_config.max_reserved_working_bytes = 2;
    scheduler_config.max_publications_per_update = 2;
    scheduler_config.max_publication_time_us = 10'000;
    auto created = world::ChunkLoadScheduler::create(std::move(context), scheduler_config);
    assert(created);
    auto scheduler = std::move(created).value();

    auto policy = compact_policy();
    policy.max_speculative_submissions_per_update = 2;
    policy.max_active_speculative_requests = 2;
    policy.reserved_required_request_slots = 1;
    world::WorldState state;
    world::PredictiveChunkStreamingController controller;
    const std::vector<world::ChunkStreamViewerMotion> forward{moving_viewer(0, 64.0)};

    auto first = controller.update(state, *scheduler, forward, policy,
                                   world::ChunkStreamMemoryPressure::nominal, 0);
    assert(first);
    assert((first.value().submitted_required == std::vector<world::ChunkCoord>{{0, 0, 0}}));
    assert(first.value().submitted_speculative.empty());
    assert(scheduler->available_submission_slots() == 1);

    bool published_required = false;
    for (simulation::WorldTick now = 1; now < 1'000 && !published_required; ++now) {
        std::this_thread::yield();
        auto update = controller.update(state, *scheduler, forward, policy,
                                        world::ChunkStreamMemoryPressure::nominal, now);
        assert(update);
        published_required = state.chunks().contains({0, 0, 0});
    }
    assert(published_required);
    assert(controller.stats().speculative_submissions == 1);
    assert(scheduler->available_submission_slots() == 1);

    const std::vector<world::ChunkStreamViewerMotion> reverse{moving_viewer(0, -64.0)};
    auto reversed = controller.update(state, *scheduler, reverse, policy,
                                      world::ChunkStreamMemoryPressure::nominal, 1'000);
    assert(reversed);
    assert(reversed.value().explicit_speculative_cancellations == 1);
    generator->release_non_origin.store(true, std::memory_order_release);

    bool drained = false;
    for (simulation::WorldTick now = 1'001; now < 2'000 && !drained; ++now) {
        std::this_thread::yield();
        auto update = controller.update(state, *scheduler, reverse, policy,
                                        world::ChunkStreamMemoryPressure::critical, now);
        assert(update);
        drained = !scheduler->has_in_flight() && !controller.has_pending_loads();
    }
    assert(drained);
    const auto stats = controller.stats();
    assert(stats.cancellation_requests == 1);
    assert(stats.cancelled_requests == 1);
    assert(stats.active_speculative_requests == 0);
    assert(scheduler->stats().reserved_working_bytes == 0);
}

} // namespace

int main() {
    test_directional_prediction_and_timely_hit_metrics();
    test_camera_prediction_and_multi_viewer_deduplication();
    test_prediction_extends_the_future_demand_footprint();
    test_reversal_teleport_and_actual_cancellation_metrics();
    test_pressure_aware_temporal_retention_and_eviction_value();
    test_eviction_waves_are_bounded_and_report_deferred_work();
    test_required_chunk_salvages_a_late_cancellation_publication();
    test_policy_and_motion_validation();
    test_controller_prioritizes_required_work_and_reconciles_cancellation();
    return 0;
}
