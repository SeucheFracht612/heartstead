#pragma once

#include "engine/core/result.hpp"
#include "engine/world/streaming/chunk_load_scheduler.hpp"
#include "engine/world/streaming/chunk_streaming_policy.hpp"

#include <cstddef>
#include <map>
#include <span>
#include <vector>

namespace heartstead::world {

struct PredictiveChunkStreamingControllerReport {
    PredictiveChunkStreamPlan policy;
    ChunkLoadPublicationReport publication;
    ChunkStreamEvictionReport eviction;
    std::vector<ChunkCoord> submitted_required;
    std::vector<ChunkCoord> submitted_speculative;
    std::size_t explicit_speculative_cancellations = 0;
    std::size_t obsolete_cancellation_signals = 0;
    std::size_t deferred_required_loads = 0;
    std::size_t pending_loads = 0;
};

// Owns one scheduler's request bookkeeping on its caller/owner thread. No other producer may submit
// to the scheduler while it is attached to this controller.
class PredictiveChunkStreamingController {
  public:
    [[nodiscard]] core::Result<PredictiveChunkStreamingControllerReport>
    update(WorldState& state, ChunkLoadScheduler& scheduler,
           std::span<const ChunkStreamViewerMotion> viewers,
           const PredictiveChunkStreamingPolicy& policy, ChunkStreamMemoryPressure pressure,
           simulation::WorldTick now_ms);

    [[nodiscard]] PredictiveChunkStreamingStats stats() const noexcept;
    [[nodiscard]] std::size_t pending_load_count() const noexcept;
    [[nodiscard]] bool has_pending_loads() const noexcept;

  private:
    enum class PendingKind : std::uint8_t {
        required,
        speculative,
    };

    [[nodiscard]] std::vector<ChunkCoord> pending_coords() const;

    PredictiveChunkStreamingPlanner planner_;
    std::map<ChunkCoord, PendingKind> pending_;
};

} // namespace heartstead::world
