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
    stats_.last_solve_ms = 0.0;
    stats_.last_apply_ms = 0.0;
    changed_chunks_.clear();

    auto status = refresh_block_table(palette, chunks);
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
        if (source_revision_ == std::numeric_limits<std::uint64_t>::max()) {
            return core::Status::failure("chunk_light.source_revision_exhausted",
                                         "voxel light source revision range is exhausted");
        }
        sources_.assign(sources.begin(), sources.end());
        ++source_revision_;
        invalidate_field(chunks);
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

core::Status ChunkLightSystem::refresh_block_table(const VoxelPalette& palette,
                                                   ChunkDatabase& chunks) {
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
    invalidate_field(chunks);
    return core::Status::ok();
}

void ChunkLightSystem::collect_dirty(ChunkDatabase& chunks,
                                     dirty::DirtyRegionTracker& dirty_regions) {
    const auto regions = dirty_regions.consume_kind(dirty::DirtyRegionKind::chunk_lighting);
    bool field_changed = !regions.empty();
    if (!regions.empty()) {
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
            field_changed = true;
        }
        resident_observations.emplace(identity, chunk->content_revision());
    }
    const bool topology_changed =
        resident_observations.size() != observed_dirty_revisions_.size() ||
        !std::ranges::equal(
            resident_observations, observed_dirty_revisions_, {},
            [](const auto& entry) { return entry.first; },
            [](const auto& entry) { return entry.first; });
    observed_dirty_revisions_ = std::move(resident_observations);
    if (field_changed || topology_changed) {
        invalidate_field(chunks);
    }
}

void ChunkLightSystem::invalidate_field(ChunkDatabase& chunks) {
    if (snapshot_build_.has_value()) {
        for (const auto& captured : snapshot_build_->snapshot.chunks) {
            auto* chunk = chunks.find(captured.identity.coordinate);
            if (chunk != nullptr && chunk->identity() == captured.identity) {
                static_cast<void>(chunk->note_stage_stale(captured.stage_ticket));
            }
        }
        snapshot_build_.reset();
        ++stats_.stale_snapshots;
    }
    for (const auto identity : chunks.identities()) {
        auto* chunk = chunks.find(identity.coordinate);
        if (chunk != nullptr && chunk->identity() == identity) {
            static_cast<void>(chunk->request_stage(ChunkStage::lighting));
        }
    }
    if (scheduler_ != nullptr && scheduler_->has_in_flight()) {
        scheduler_->cancel();
    }
    relight_requested_ = true;
}

void ChunkLightSystem::begin_snapshot(ChunkDatabase& chunks) {
    SnapshotBuildState build;
    build.identities = chunks.identities();
    build.snapshot.sources = sources_;
    build.snapshot.chunks.reserve(build.identities.size());
    build.source_revision = source_revision_;
    for (const auto identity : build.identities) {
        auto* chunk = chunks.find(identity.coordinate);
        if (chunk == nullptr || chunk->identity() != identity) {
            relight_requested_ = true;
            return;
        }
        auto stage_ticket = chunk->stage_ticket(ChunkStage::lighting);
        if (chunk->stages().record(ChunkStage::lighting).state != ChunkStageState::requested) {
            stage_ticket = chunk->request_stage(ChunkStage::lighting);
        }
        ChunkLightSnapshot captured;
        captured.identity = identity;
        captured.stage_ticket = stage_ticket;
        captured.content_revision = chunk->content_revision();
        captured.cells.reserve(VoxelChunk::total_cells);
        build.snapshot.chunks.push_back(std::move(captured));
    }
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
            !chunk->stage_ticket_is_current(captured.stage_ticket) ||
            chunk->content_revision() != captured.content_revision) {
            return false;
        }
    }
    return true;
}

core::Status ChunkLightSystem::advance_snapshot(ChunkDatabase& chunks) {
    if (!snapshot_build_.has_value()) {
        return core::Status::ok();
    }
    if (!snapshot_still_current(chunks)) {
        invalidate_field(chunks);
        return core::Status::ok();
    }

    auto& build = *snapshot_build_;
    std::size_t copied = 0;
    while (build.chunk_index < build.identities.size() &&
           copied < config_.max_snapshot_cells_per_update) {
        const auto identity = build.identities[build.chunk_index];
        const auto* chunk = chunks.find(identity.coordinate);
        if (chunk == nullptr || chunk->identity() != identity) {
            invalidate_field(chunks);
            return core::Status::ok();
        }
        auto& captured = build.snapshot.chunks[build.chunk_index];
        if (chunk->content_revision() != captured.content_revision ||
            !chunk->stage_ticket_is_current(captured.stage_ticket)) {
            invalidate_field(chunks);
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
        invalidate_field(chunks);
        return core::Status::ok();
    }
    return submit_snapshot(chunks);
}

core::Status ChunkLightSystem::submit_snapshot(ChunkDatabase& chunks) {
    if (!snapshot_build_.has_value()) {
        return core::Status::ok();
    }
    if (next_request_id_ == 0) {
        return core::Status::failure("chunk_light.request_id_exhausted",
                                     "chunk light request id range is exhausted");
    }
    auto snapshot_status = snapshot_build_->snapshot.validate();
    if (!snapshot_status) {
        return snapshot_status;
    }
    std::vector<ChunkStageTicket> stage_tickets;
    stage_tickets.reserve(snapshot_build_->snapshot.chunks.size());
    for (const auto& captured : snapshot_build_->snapshot.chunks) {
        auto* chunk = chunks.find(captured.identity.coordinate);
        if (chunk == nullptr || chunk->identity() != captured.identity ||
            chunk->content_revision() != captured.content_revision ||
            !chunk->stage_ticket_is_current(captured.stage_ticket) ||
            chunk->stages().record(ChunkStage::lighting).state != ChunkStageState::requested) {
            invalidate_field(chunks);
            return core::Status::ok();
        }
        stage_tickets.push_back(captured.stage_ticket);
    }

    ChunkLightRequest request;
    request.request_id = next_request_id_;
    request.source_revision = snapshot_build_->source_revision;
    request.snapshot = std::move(snapshot_build_->snapshot);
    request.block_table = block_table_;
    auto status = scheduler_->submit(std::move(request));
    if (!status) {
        snapshot_build_.reset();
        relight_requested_ = true;
        return status;
    }
    std::size_t marked_running = 0;
    for (const auto ticket : stage_tickets) {
        auto* chunk = chunks.find(ticket.identity.coordinate);
        status =
            chunk == nullptr
                ? core::Status::failure("chunk_light.stale_submit_identity",
                                        "voxel light chunk changed while its field was submitted")
                : chunk->mark_stage_running(ticket);
        if (!status) {
            scheduler_->cancel();
            for (std::size_t index = 0; index < marked_running; ++index) {
                auto* marked = chunks.find(stage_tickets[index].identity.coordinate);
                if (marked != nullptr && marked->identity() == stage_tickets[index].identity &&
                    marked->stage_ticket_is_current(stage_tickets[index])) {
                    static_cast<void>(marked->retry_stage(stage_tickets[index]));
                }
            }
            snapshot_build_.reset();
            relight_requested_ = true;
            return status;
        }
        ++marked_running;
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

    const auto note_tickets = [&chunks, &result](bool cancelled) {
        for (const auto ticket : result.stage_tickets) {
            auto* chunk = chunks.find(ticket.identity.coordinate);
            if (chunk == nullptr || chunk->identity() != ticket.identity) {
                continue;
            }
            if (cancelled) {
                static_cast<void>(chunk->note_stage_cancelled(ticket));
            } else {
                static_cast<void>(chunk->note_stage_stale(ticket));
            }
        }
    };
    const auto retry_current_tickets = [this, &chunks, &result]() {
        for (const auto ticket : result.stage_tickets) {
            auto* chunk = chunks.find(ticket.identity.coordinate);
            if (chunk == nullptr || chunk->identity() != ticket.identity ||
                !chunk->stage_ticket_is_current(ticket)) {
                continue;
            }
            const auto state = chunk->stages().record(ChunkStage::lighting).state;
            if (state != ChunkStageState::running && state != ChunkStageState::ready) {
                continue;
            }
            auto status = chunk->retry_stage(ticket);
            if (!status) {
                return status;
            }
        }
        relight_requested_ = true;
        return core::Status::ok();
    };
    const auto tickets_are_current = [&chunks, &result]() {
        const auto identities = chunks.identities();
        if (identities.size() != result.stage_tickets.size()) {
            return false;
        }
        for (std::size_t index = 0; index < identities.size(); ++index) {
            const auto ticket = result.stage_tickets[index];
            const auto* chunk = chunks.find(identities[index].coordinate);
            if (ticket.identity != identities[index] || ticket.stage != ChunkStage::lighting ||
                chunk == nullptr || chunk->identity() != ticket.identity ||
                !chunk->stage_ticket_is_current(ticket) ||
                chunk->stages().record(ChunkStage::lighting).state != ChunkStageState::running) {
                return false;
            }
        }
        return true;
    };

    if (result.state == ChunkLightResultState::cancelled) {
        note_tickets(true);
        ++stats_.stale_results;
        relight_requested_ = true;
        return core::Status::ok();
    }
    if (block_table_ == nullptr || result.block_table_revision != block_table_->revision ||
        result.source_revision != source_revision_ || !tickets_are_current()) {
        note_tickets(false);
        ++stats_.stale_results;
        relight_requested_ = true;
        return core::Status::ok();
    }
    if (result.state == ChunkLightResultState::failed || !result.light.has_value()) {
        ++stats_.failed_results;
        auto retry = retry_current_tickets();
        if (!retry) {
            return retry;
        }
        return core::Status::failure(
            result.error_code.empty() ? "chunk_light.solve_failed" : result.error_code,
            result.error_message.empty() ? "voxel light solve failed" : result.error_message);
    }

    if (result.light->patches.size() != result.stage_tickets.size()) {
        ++stats_.failed_results;
        auto retry = retry_current_tickets();
        return retry ? core::Status::failure(
                           "chunk_light.patch_ticket_count_mismatch",
                           "voxel light result patch and stage-ticket counts differ")
                     : retry;
    }
    for (std::size_t index = 0; index < result.light->patches.size(); ++index) {
        const auto& patch = result.light->patches[index];
        const auto ticket = result.stage_tickets[index];
        const auto* chunk = chunks.find(patch.identity.coordinate);
        if (patch.stage_ticket != ticket || chunk == nullptr ||
            chunk->identity() != patch.identity ||
            chunk->content_revision() != patch.source_content_revision) {
            note_tickets(false);
            ++stats_.stale_results;
            relight_requested_ = true;
            return core::Status::ok();
        }
        auto patch_status = patch.validate();
        if (!patch_status) {
            ++stats_.failed_results;
            auto retry = retry_current_tickets();
            return retry ? patch_status : retry;
        }
    }

    std::size_t marked_ready = 0;
    for (const auto ticket : result.stage_tickets) {
        auto* chunk = chunks.find(ticket.identity.coordinate);
        auto status = chunk->mark_stage_ready(ticket);
        if (!status) {
            for (std::size_t index = 0; index < marked_ready; ++index) {
                auto* marked = chunks.find(result.stage_tickets[index].identity.coordinate);
                if (marked != nullptr &&
                    marked->identity() == result.stage_tickets[index].identity &&
                    marked->stage_ticket_is_current(result.stage_tickets[index])) {
                    static_cast<void>(marked->retry_stage(result.stage_tickets[index]));
                }
            }
            relight_requested_ = true;
            return status;
        }
        ++marked_ready;
    }

    const auto start = Clock::now();
    auto applied = apply_voxel_light(chunks, dirty_regions, *result.light);
    stats_.last_apply_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (!applied && applied.error().code == "voxel_light.stale_result") {
        note_tickets(false);
        ++stats_.stale_results;
        relight_requested_ = true;
        return core::Status::ok();
    }
    if (!applied) {
        ++stats_.failed_results;
        auto retry = retry_current_tickets();
        if (!retry) {
            return retry;
        }
        return core::Status::failure(applied.error().code, applied.error().message);
    }
    if (stats_.last_apply_ms > config_.apply_time_budget_ms) {
        ++stats_.apply_budget_overruns;
    }
    auto report = std::move(applied).value();
    for (const auto ticket : result.stage_tickets) {
        auto* chunk = chunks.find(ticket.identity.coordinate);
        const bool output_changed =
            std::ranges::binary_search(report.changed_chunks, ticket.identity.coordinate);
        auto status = chunk->publish_stage(ticket, output_changed);
        if (!status) {
            return status;
        }
    }
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
