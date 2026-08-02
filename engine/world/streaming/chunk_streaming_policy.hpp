#pragma once

#include "engine/core/result.hpp"
#include "engine/simulation/simulation_lod.hpp"
#include "engine/world/streaming/chunk_streamer.hpp"
#include "engine/world/world_state.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace heartstead::world {

enum class ChunkStreamMemoryPressure : std::uint8_t {
    nominal,
    elevated,
    critical,
};

enum class ChunkSpeculativeLoadOutcome : std::uint8_t {
    published,
    cancelled,
    failed,
    stale,
};

struct ChunkStreamViewerMotion {
    simulation::SimulationViewer viewer;
    double velocity_x_blocks_per_second = 0.0;
    double velocity_y_blocks_per_second = 0.0;
    double velocity_z_blocks_per_second = 0.0;
    double view_direction_x = 0.0;
    double view_direction_y = 0.0;
    double view_direction_z = 0.0;
    bool teleport = false;
};

struct PredictiveChunkStreamingPolicy {
    ChunkStreamInterestPolicy interest;
    double prediction_horizon_seconds = 1.0;
    double camera_lookahead_chunks = 1.0;
    double minimum_velocity_blocks_per_second = 1.0;
    std::uint16_t max_prediction_distance_chunks = 4;
    std::uint16_t predictive_horizontal_radius_chunks = 1;
    std::uint16_t predictive_vertical_radius_chunks = 0;
    std::size_t max_speculative_submissions_per_update = 4;
    std::size_t max_active_speculative_requests = 16;
    simulation::WorldTick speculative_ttl_ms = 3'000;
    simulation::WorldTick temporal_retention_ms = 2'000;
    std::size_t nominal_resident_chunk_budget = 512;
    std::size_t elevated_resident_chunk_budget = 384;
    std::size_t critical_resident_chunk_budget = 256;
    double estimated_reload_cost_ms = 10.0;
    double spatial_retention_value = 100.0;
    double temporal_retention_value = 50.0;
    double speculative_value_penalty = 25.0;
    double distance_value_penalty_per_chunk = 1.0;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkSpeculativeLoadCandidate {
    ChunkCoord coord;
    std::uint32_t trajectory_step = 0;
    std::uint64_t distance_to_prediction_squared = 0;
};

struct ChunkStreamEvictionValue {
    ChunkCoord coord;
    double value = 0.0;
    double reload_cost_value = 0.0;
    double spatial_retention_bonus = 0.0;
    double temporal_retention_bonus = 0.0;
    double speculative_penalty = 0.0;
    double distance_penalty = 0.0;
    std::uint64_t nearest_viewer_distance_squared = 0;
    simulation::WorldTick age_since_required_ms = 0;
    bool spatially_retained = false;
    bool temporally_retained = false;
    bool speculative = false;
    bool selected = false;
    bool pressure_override = false;
};

struct PredictiveChunkStreamPlan {
    ChunkStreamInterestPlan immediate;
    std::vector<ChunkCoord> predicted_chunks;
    std::vector<ChunkSpeculativeLoadCandidate> speculative_loads;
    std::vector<ChunkCoord> scheduler_interest;
    std::vector<ChunkCoord> speculative_cancellations;
    std::vector<ChunkStreamEvictionValue> ranked_eviction_candidates;
    std::vector<ChunkCoord> eviction_requests;
    ChunkStreamMemoryPressure memory_pressure = ChunkStreamMemoryPressure::nominal;
    std::size_t target_resident_chunk_count = 0;
    std::size_t unresolved_resident_overage = 0;
    bool teleport_mode = false;
};

struct PredictiveChunkStreamingStats {
    std::uint64_t planning_updates = 0;
    std::uint64_t teleport_updates = 0;
    std::uint64_t demand_transitions = 0;
    std::uint64_t speculative_submissions = 0;
    std::uint64_t speculative_publications = 0;
    std::uint64_t useful_predictions = 0;
    std::uint64_t timely_prefetch_hits = 0;
    std::uint64_t late_prefetch_hits = 0;
    std::uint64_t wasted_predictions = 0;
    std::uint64_t cancellation_requests = 0;
    std::uint64_t cancelled_requests = 0;
    std::uint64_t cancellation_misses = 0;
    std::uint64_t failed_requests = 0;
    std::uint64_t stale_results = 0;
    std::uint64_t cumulative_timeliness_ms = 0;
    simulation::WorldTick maximum_timeliness_ms = 0;
    std::size_t active_speculative_requests = 0;
    double prediction_accuracy = 0.0;
    double timely_coverage = 0.0;
    double mean_timeliness_ms = 0.0;
};

class PredictiveChunkStreamingPlanner {
  public:
    // This is an owner-thread policy object. plan() offers bounded work and reports cancellation
    // and eviction actions; the caller remains responsible for applying those actions. Only work
    // accepted from the latest plan may be registered, and actual scheduler outcomes must be
    // reported after their publication/cancellation boundary.
    [[nodiscard]] core::Result<PredictiveChunkStreamPlan>
    plan(const WorldState& state, std::span<const ChunkStreamViewerMotion> viewers,
         std::span<const ChunkCoord> pending_loads, const PredictiveChunkStreamingPolicy& policy,
         ChunkStreamMemoryPressure pressure, simulation::WorldTick now_ms);

    [[nodiscard]] core::Status note_speculative_submitted(ChunkCoord coord,
                                                          simulation::WorldTick now_ms);
    [[nodiscard]] core::Status note_speculative_outcome(ChunkCoord coord,
                                                        ChunkSpeculativeLoadOutcome outcome,
                                                        simulation::WorldTick now_ms);

    [[nodiscard]] PredictiveChunkStreamingStats stats() const noexcept;
    void reset() noexcept;

  private:
    enum class SpeculativeState : std::uint8_t {
        submitted,
        published,
        cancellation_requested,
    };

    struct SpeculativeRecord {
        simulation::WorldTick submitted_at_ms = 0;
        simulation::WorldTick published_at_ms = 0;
        SpeculativeState state = SpeculativeState::submitted;
        bool invalidated = false;
        bool waste_counted = false;
    };

    std::map<ChunkCoord, SpeculativeRecord> speculative_;
    std::map<ChunkCoord, simulation::WorldTick> last_required_at_ms_;
    std::vector<ChunkCoord> previous_required_;
    std::vector<ChunkCoord> offered_speculation_;
    PredictiveChunkStreamingStats stats_;
    simulation::WorldTick last_observed_time_ms_ = 0;
    bool initialized_ = false;
};

[[nodiscard]] const char*
chunk_stream_memory_pressure_name(ChunkStreamMemoryPressure pressure) noexcept;

} // namespace heartstead::world
