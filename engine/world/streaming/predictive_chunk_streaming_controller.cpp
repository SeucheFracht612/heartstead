#include "engine/world/streaming/predictive_chunk_streaming_controller.hpp"

#include "engine/profiling/profiler.hpp"

#include <algorithm>

namespace heartstead::world {

core::Result<PredictiveChunkStreamingControllerReport> PredictiveChunkStreamingController::update(
    WorldState& state, ChunkLoadScheduler& scheduler,
    std::span<const ChunkStreamViewerMotion> viewers, const PredictiveChunkStreamingPolicy& policy,
    ChunkStreamMemoryPressure pressure, simulation::WorldTick now_ms) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("streaming.predictive_controller");
    PredictiveChunkStreamingControllerReport report;
    report.publication = scheduler.update(state);

    for (const auto& timing : report.publication.timings) {
        const auto pending = pending_.find(timing.coord);
        if (pending == pending_.end()) {
            return core::Result<PredictiveChunkStreamingControllerReport>::failure(
                "predictive_chunk_streaming.untracked_result",
                "chunk scheduler published a result not owned by the predictive controller");
        }
        if (planner_.tracks_speculation(timing.coord)) {
            ChunkSpeculativeLoadOutcome outcome = ChunkSpeculativeLoadOutcome::failed;
            switch (timing.state) {
            case ChunkLoadResultState::succeeded:
                outcome = ChunkSpeculativeLoadOutcome::published;
                break;
            case ChunkLoadResultState::failed:
                outcome = ChunkSpeculativeLoadOutcome::failed;
                break;
            case ChunkLoadResultState::cancelled:
                outcome = ChunkSpeculativeLoadOutcome::cancelled;
                break;
            case ChunkLoadResultState::stale:
                outcome = ChunkSpeculativeLoadOutcome::stale;
                break;
            }
            auto status = planner_.note_speculative_outcome(timing.coord, outcome, now_ms);
            if (!status) {
                return core::Result<PredictiveChunkStreamingControllerReport>::failure(
                    status.error().code, status.error().message);
            }
        }
        pending_.erase(pending);
    }
    if (!report.publication.failures.empty()) {
        const auto& failure = report.publication.failures.front();
        return core::Result<PredictiveChunkStreamingControllerReport>::failure(
            failure.error.code, failure.error.message);
    }
    if (!report.publication.stale.empty()) {
        return core::Result<PredictiveChunkStreamingControllerReport>::failure(
            "predictive_chunk_streaming.stale_result",
            "exclusive predictive controller received a stale chunk result");
    }

    const auto pending = pending_coords();
    auto planned = planner_.plan(state, viewers, pending, policy, pressure, now_ms);
    if (!planned) {
        return core::Result<PredictiveChunkStreamingControllerReport>::failure(
            planned.error().code, planned.error().message);
    }
    report.policy = std::move(planned).value();

    for (const auto coord : report.policy.speculative_cancellations) {
        const auto pending_request = pending_.find(coord);
        if (pending_request == pending_.end() ||
            pending_request->second != PendingKind::speculative) {
            return core::Result<PredictiveChunkStreamingControllerReport>::failure(
                "predictive_chunk_streaming.cancellation_mismatch",
                "policy selected an untracked or required request for speculative cancellation");
        }
        auto status = scheduler.cancel(coord);
        if (!status) {
            return core::Result<PredictiveChunkStreamingControllerReport>::failure(
                status.error().code, status.error().message);
        }
        ++report.explicit_speculative_cancellations;
    }
    report.obsolete_cancellation_signals =
        scheduler.cancel_all_except(report.policy.scheduler_interest);

    report.eviction = ChunkStreamer::evict_chunks(state, report.policy.eviction_requests);
    if (!report.eviction.missing_chunks.empty() || !report.eviction.retained_dirty_chunks.empty()) {
        return core::Result<PredictiveChunkStreamingControllerReport>::failure(
            "predictive_chunk_streaming.eviction_mismatch",
            "pressure policy selected a missing or persistence-dirty chunk for eviction");
    }

    for (const auto coord : report.policy.immediate.load_requests) {
        if (state.chunks().contains(coord) || pending_.contains(coord)) {
            continue;
        }
        if (!scheduler.has_capacity()) {
            ++report.deferred_required_loads;
            continue;
        }
        auto submitted = scheduler.submit(coord, jobs::JobPriority::high);
        if (!submitted) {
            return core::Result<PredictiveChunkStreamingControllerReport>::failure(
                submitted.error().code, submitted.error().message);
        }
        pending_.emplace(coord, PendingKind::required);
        report.submitted_required.push_back(coord);
    }

    if (report.deferred_required_loads == 0) {
        for (const auto& candidate : report.policy.speculative_loads) {
            if (state.chunks().contains(candidate.coord) || pending_.contains(candidate.coord)) {
                continue;
            }
            if (scheduler.available_submission_slots() <= policy.reserved_required_request_slots) {
                break;
            }
            auto submitted = scheduler.submit(candidate.coord, jobs::JobPriority::low);
            if (!submitted) {
                return core::Result<PredictiveChunkStreamingControllerReport>::failure(
                    submitted.error().code, submitted.error().message);
            }
            auto status = planner_.note_speculative_submitted(candidate.coord, now_ms);
            if (!status) {
                static_cast<void>(scheduler.cancel(candidate.coord));
                return core::Result<PredictiveChunkStreamingControllerReport>::failure(
                    status.error().code, status.error().message);
            }
            pending_.emplace(candidate.coord, PendingKind::speculative);
            report.submitted_speculative.push_back(candidate.coord);
        }
    }

    report.pending_loads = pending_.size();
    return core::Result<PredictiveChunkStreamingControllerReport>::success(std::move(report));
}

PredictiveChunkStreamingStats PredictiveChunkStreamingController::stats() const noexcept {
    return planner_.stats();
}

std::size_t PredictiveChunkStreamingController::pending_load_count() const noexcept {
    return pending_.size();
}

bool PredictiveChunkStreamingController::has_pending_loads() const noexcept {
    return !pending_.empty();
}

std::vector<ChunkCoord> PredictiveChunkStreamingController::pending_coords() const {
    std::vector<ChunkCoord> result;
    result.reserve(pending_.size());
    for (const auto& [coord, _] : pending_) {
        result.push_back(coord);
    }
    return result;
}

} // namespace heartstead::world
