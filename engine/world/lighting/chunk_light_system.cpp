#include "engine/world/lighting/chunk_light_system.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace heartstead::world {

core::Status ChunkLightSystemConfig::validate() const {
    auto status = scheduler.validate();
    if (!status) {
        return status;
    }
    if (max_snapshot_cells_per_update == 0 || !std::isfinite(apply_time_budget_ms) ||
        apply_time_budget_ms <= 0.0) {
        return core::Status::failure(
            "chunk_light.invalid_system_config",
            "chunk light snapshot and apply budgets must be finite and positive");
    }
    return core::Status::ok();
}

core::Result<std::unique_ptr<ChunkLightSystem>>
ChunkLightSystem::create(const VoxelPalette& palette, ChunkLightSystemConfig config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<std::unique_ptr<ChunkLightSystem>>::failure(status.error().code,
                                                                        status.error().message);
    }
    auto table =
        std::make_shared<const VoxelLightBlockTable>(build_voxel_light_block_table(palette));
    status = table->validate();
    if (!status) {
        return core::Result<std::unique_ptr<ChunkLightSystem>>::failure(status.error().code,
                                                                        status.error().message);
    }
    auto scheduler = ChunkLightScheduler::create(config.scheduler);
    if (!scheduler) {
        return core::Result<std::unique_ptr<ChunkLightSystem>>::failure(scheduler.error().code,
                                                                        scheduler.error().message);
    }
    return core::Result<std::unique_ptr<ChunkLightSystem>>::success(
        std::unique_ptr<ChunkLightSystem>(
            new ChunkLightSystem(config, std::move(scheduler).value(), std::move(table))));
}

ChunkLightSystem::ChunkLightSystem(ChunkLightSystemConfig config,
                                   std::unique_ptr<ChunkLightScheduler> scheduler,
                                   std::shared_ptr<const VoxelLightBlockTable> block_table)
    : config_(config), scheduler_(std::move(scheduler)), block_table_(std::move(block_table)) {}

ChunkLightSystem::~ChunkLightSystem() {
    shutdown();
}

core::Status ChunkLightSystem::update(ChunkDatabase& chunks,
                                      dirty::DirtyRegionTracker& dirty_regions,
                                      const VoxelPalette& palette,
                                      std::span<const VoxelLightSource> sources) {
    stats_.snapshot_cells_copied_this_update = 0;
    stats_.changed_chunks_this_update = 0;
    stats_.changed_cells_this_update = 0;
    stats_.last_sunlight_queue_visits = 0;
    stats_.last_block_light_queue_visits = 0;
    stats_.last_apply_ms = 0.0;
    changed_chunks_.clear();

    auto status = refresh_block_table(palette);
    if (!status) {
        return status;
    }
    if (!std::ranges::is_sorted(sources, {}, &VoxelLightSource::position) ||
        std::ranges::adjacent_find(sources, {}, &VoxelLightSource::position) != sources.end() ||
        std::ranges::any_of(sources, [](const auto& source) { return source.light == 0; })) {
        return core::Status::failure(
            "chunk_light.invalid_sources",
            "external voxel light sources must be nonzero and strictly sorted by block position");
    }
    if (!std::ranges::equal(sources, sources_)) {
        sources_.assign(sources.begin(), sources.end());
        source_revision_ = source_revision_ == std::numeric_limits<std::uint64_t>::max()
                               ? 1
                               : source_revision_ + 1;
        relight_requested_ = true;
    }
    collect_dirty(chunks, dirty_regions);
    status = apply_completed(chunks, dirty_regions);
    if (!status) {
        return status;
    }
    if (!snapshot_build_.has_value() && !scheduler_->has_in_flight() && relight_requested_) {
        begin_snapshot(chunks);
    }
    status = advance_snapshot(chunks);
    refresh_stats();
    return status;
}

void ChunkLightSystem::shutdown() noexcept {
    if (scheduler_ != nullptr) {
        scheduler_->shutdown();
        scheduler_.reset();
    }
    snapshot_build_.reset();
    observed_dirty_revisions_.clear();
    changed_chunks_.clear();
    relight_requested_ = false;
    refresh_stats();
}

std::span<const ChunkCoord> ChunkLightSystem::changed_chunks() const noexcept {
    return changed_chunks_;
}

const ChunkLightSystemStats& ChunkLightSystem::stats() noexcept {
    refresh_stats();
    return stats_;
}

const ChunkLightSystemStats& ChunkLightSystem::stats() const noexcept {
    return stats_;
}

core::Status ChunkLightSystem::refresh_block_table(const VoxelPalette& palette) {
    if (block_table_ != nullptr && block_table_->revision == palette.render_revision()) {
        return core::Status::ok();
    }
    auto rebuilt =
        std::make_shared<const VoxelLightBlockTable>(build_voxel_light_block_table(palette));
    auto status = rebuilt->validate();
    if (!status) {
        return status;
    }
    block_table_ = std::move(rebuilt);
    relight_requested_ = true;
    snapshot_build_.reset();
    if (scheduler_ != nullptr) {
        scheduler_->cancel();
    }
    return core::Status::ok();
}

void ChunkLightSystem::collect_dirty(const ChunkDatabase& chunks,
                                     dirty::DirtyRegionTracker& dirty_regions) {
    const auto regions = dirty_regions.consume_kind(dirty::DirtyRegionKind::chunk_lighting);
    if (!regions.empty()) {
        relight_requested_ = true;
        stats_.dirty_regions_consumed += regions.size();
    }

    std::map<ChunkIdentity, std::uint64_t> resident_observations;
    for (const auto identity : chunks.identities()) {
        const auto* chunk = chunks.find(identity.coordinate);
        if (chunk == nullptr || chunk->identity() != identity) {
            continue;
        }
        const auto observed = observed_dirty_revisions_.find(identity);
        if (chunk->dirty().contains(ChunkDirtyFlag::lighting) &&
            (observed == observed_dirty_revisions_.end() ||
             observed->second != chunk->content_revision())) {
            relight_requested_ = true;
        }
        resident_observations.emplace(identity, chunk->content_revision());
    }
    observed_dirty_revisions_ = std::move(resident_observations);
}

void ChunkLightSystem::begin_snapshot(const ChunkDatabase& chunks) {
    SnapshotBuildState build;
    build.identities = chunks.identities();
    build.snapshot.sources = sources_;
    build.snapshot.chunks.reserve(build.identities.size());
    build.source_revision = source_revision_;
    snapshot_build_ = std::move(build);
    relight_requested_ = false;
}

bool ChunkLightSystem::snapshot_still_current(const ChunkDatabase& chunks) const {
    if (!snapshot_build_.has_value() || chunks.identities() != snapshot_build_->identities) {
        return false;
    }
    if (snapshot_build_->source_revision != source_revision_) {
        return false;
    }
    for (const auto& captured : snapshot_build_->snapshot.chunks) {
        const auto* chunk = chunks.find(captured.identity.coordinate);
        if (chunk == nullptr || chunk->identity() != captured.identity ||
            chunk->content_revision() != captured.content_revision) {
            return false;
        }
    }
    return true;
}

core::Status ChunkLightSystem::advance_snapshot(const ChunkDatabase& chunks) {
    if (!snapshot_build_.has_value()) {
        return core::Status::ok();
    }
    if (!snapshot_still_current(chunks)) {
        snapshot_build_.reset();
        relight_requested_ = true;
        ++stats_.stale_snapshots;
        return core::Status::ok();
    }

    auto& build = *snapshot_build_;
    std::size_t copied = 0;
    while (build.chunk_index < build.identities.size() &&
           copied < config_.max_snapshot_cells_per_update) {
        const auto identity = build.identities[build.chunk_index];
        const auto* chunk = chunks.find(identity.coordinate);
        if (chunk == nullptr || chunk->identity() != identity) {
            snapshot_build_.reset();
            relight_requested_ = true;
            ++stats_.stale_snapshots;
            return core::Status::ok();
        }
        if (build.cell_index == 0) {
            ChunkLightSnapshot captured;
            captured.identity = identity;
            captured.content_revision = chunk->content_revision();
            captured.cells.reserve(VoxelChunk::total_cells);
            build.snapshot.chunks.push_back(std::move(captured));
        }
        auto& captured = build.snapshot.chunks.back();
        if (chunk->content_revision() != captured.content_revision) {
            snapshot_build_.reset();
            relight_requested_ = true;
            ++stats_.stale_snapshots;
            return core::Status::ok();
        }
        const auto remaining_budget = config_.max_snapshot_cells_per_update - copied;
        const auto remaining_chunk = VoxelChunk::total_cells - build.cell_index;
        const auto count = std::min(remaining_budget, remaining_chunk);
        const auto cells = chunk->cells();
        const auto begin_offset = static_cast<std::ptrdiff_t>(build.cell_index);
        const auto end_offset = static_cast<std::ptrdiff_t>(build.cell_index + count);
        captured.cells.insert(captured.cells.end(), cells.begin() + begin_offset,
                              cells.begin() + end_offset);
        build.cell_index += count;
        copied += count;
        if (build.cell_index == VoxelChunk::total_cells) {
            build.cell_index = 0;
            ++build.chunk_index;
        }
    }
    stats_.snapshot_cells_copied_this_update = copied;
    stats_.total_snapshot_cells_copied += copied;
    if (build.chunk_index != build.identities.size()) {
        return core::Status::ok();
    }
    if (!snapshot_still_current(chunks)) {
        snapshot_build_.reset();
        relight_requested_ = true;
        ++stats_.stale_snapshots;
        return core::Status::ok();
    }
    return submit_snapshot();
}

core::Status ChunkLightSystem::submit_snapshot() {
    if (!snapshot_build_.has_value()) {
        return core::Status::ok();
    }
    if (next_request_id_ == 0) {
        return core::Status::failure("chunk_light.request_id_exhausted",
                                     "chunk light request id range is exhausted");
    }
    ChunkLightRequest request;
    request.request_id = next_request_id_;
    request.snapshot = std::move(snapshot_build_->snapshot);
    request.block_table = block_table_;
    submitted_source_revision_ = snapshot_build_->source_revision;
    auto status = scheduler_->submit(std::move(request));
    if (!status) {
        return status;
    }
    next_request_id_ =
        next_request_id_ == std::numeric_limits<std::uint64_t>::max() ? 0 : next_request_id_ + 1;
    snapshot_build_.reset();
    ++stats_.submitted_fields;
    return core::Status::ok();
}

core::Status ChunkLightSystem::apply_completed(ChunkDatabase& chunks,
                                               dirty::DirtyRegionTracker& dirty_regions) {
    using Clock = std::chrono::steady_clock;
    auto completed = scheduler_->drain_completed(1);
    if (completed.empty()) {
        return core::Status::ok();
    }
    auto& result = completed.front();
    stats_.last_solve_ms = result.solve_ms;
    if (result.state == ChunkLightResultState::cancelled ||
        result.block_table_revision != block_table_->revision ||
        submitted_source_revision_ != source_revision_) {
        ++stats_.stale_results;
        relight_requested_ = true;
        return core::Status::ok();
    }
    if (result.state == ChunkLightResultState::failed || !result.light.has_value()) {
        ++stats_.failed_results;
        return core::Status::failure(
            result.error_code.empty() ? "chunk_light.solve_failed" : result.error_code,
            result.error_message.empty() ? "voxel light solve failed" : result.error_message);
    }

    const auto start = Clock::now();
    auto applied = apply_voxel_light(chunks, dirty_regions, *result.light);
    stats_.last_apply_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (!applied && applied.error().code == "voxel_light.stale_result") {
        ++stats_.stale_results;
        relight_requested_ = true;
        return core::Status::ok();
    }
    if (!applied) {
        ++stats_.failed_results;
        return core::Status::failure(applied.error().code, applied.error().message);
    }
    if (stats_.last_apply_ms > config_.apply_time_budget_ms) {
        ++stats_.apply_budget_overruns;
    }
    auto report = std::move(applied).value();
    changed_chunks_ = std::move(report.changed_chunks);
    stats_.changed_chunks_this_update = changed_chunks_.size();
    stats_.changed_cells_this_update = report.changed_cell_count;
    stats_.total_changed_chunks += report.changed_chunk_count;
    stats_.total_changed_cells += report.changed_cell_count;
    stats_.last_sunlight_queue_visits = result.light->stats.sunlight_queue_visits;
    stats_.last_block_light_queue_visits = result.light->stats.block_light_queue_visits;
    stats_.total_sunlight_queue_visits += result.light->stats.sunlight_queue_visits;
    stats_.total_block_light_queue_visits += result.light->stats.block_light_queue_visits;
    ++stats_.applied_fields;
    return core::Status::ok();
}

void ChunkLightSystem::refresh_stats() noexcept {
    stats_.relight_requested = relight_requested_;
    stats_.snapshot_in_progress = snapshot_build_.has_value();
    stats_.solve_in_flight = scheduler_ != nullptr && scheduler_->has_in_flight();
    stats_.snapshot_chunk_count =
        snapshot_build_.has_value() ? snapshot_build_->identities.size() : 0;
    if (snapshot_build_.has_value()) {
        const auto copied_chunks = snapshot_build_->chunk_index * VoxelChunk::total_cells;
        const auto copied_cells = copied_chunks + snapshot_build_->cell_index;
        const auto total_cells = snapshot_build_->identities.size() * VoxelChunk::total_cells;
        stats_.snapshot_pending_cell_count = total_cells - copied_cells;
    } else {
        stats_.snapshot_pending_cell_count = 0;
    }
    stats_.completed_mailbox_count =
        scheduler_ == nullptr ? 0 : scheduler_->stats().completed_mailbox_count;
}

} // namespace heartstead::world
