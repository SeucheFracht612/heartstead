#include "engine/world/streaming/chunk_streamer.hpp"

#include "engine/profiling/profiler.hpp"
#include "engine/world/chunks/chunk_edit_delta_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

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

[[nodiscard]] std::int64_t saturated_subtract(std::int64_t value, std::int64_t amount) noexcept {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    if (value < min + amount) {
        return min;
    }
    return value - amount;
}

[[nodiscard]] std::int64_t saturated_add(std::int64_t value, std::int64_t amount) noexcept {
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if (value > max - amount) {
        return max;
    }
    return value + amount;
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

[[nodiscard]] bool within_any_cylinder(ChunkCoord coord, const std::vector<ChunkCoord>& centers,
                                       std::uint16_t horizontal_radius,
                                       std::uint16_t vertical_radius) noexcept {
    return std::ranges::any_of(centers, [=](ChunkCoord center) {
        return within_cylinder(coord, center, horizontal_radius, vertical_radius);
    });
}

[[nodiscard]] bool is_dirty_for_persistence_or_replication(const VoxelChunk& chunk) noexcept {
    return chunk.dirty().contains(ChunkDirtyFlag::save) ||
           chunk.dirty().contains(ChunkDirtyFlag::replication);
}

[[nodiscard]] std::map<ChunkCoord, std::vector<const VoxelEditRecord*>>
edits_grouped_by_chunk(const ChunkDatabase& chunks) {
    std::map<ChunkCoord, std::vector<const VoxelEditRecord*>> edits_by_chunk;
    for (const auto& edit : chunks.edit_log()) {
        edits_by_chunk[edit.chunk_coord].push_back(&edit);
    }
    return edits_by_chunk;
}

} // namespace

FileSaveChunkEditDeltaSource::FileSaveChunkEditDeltaSource(
    const save::FileSaveDatabase& database) noexcept
    : database_(&database) {}

core::Result<std::optional<save::ChunkEditSaveRecord>>
FileSaveChunkEditDeltaSource::read_chunk_delta(ChunkCoord coord) const {
    auto delta = database_->read_chunk_delta(coord);
    if (!delta) {
        if (delta.error().code == "save_database.missing_chunk_delta") {
            return core::Result<std::optional<save::ChunkEditSaveRecord>>::success(std::nullopt);
        }
        return core::Result<std::optional<save::ChunkEditSaveRecord>>::failure(
            delta.error().code, delta.error().message);
    }
    return core::Result<std::optional<save::ChunkEditSaveRecord>>::success(
        std::move(delta).value());
}

FileSaveChunkEditDeltaSink::FileSaveChunkEditDeltaSink(
    const save::FileSaveDatabase& database) noexcept
    : database_(&database) {}

core::Status
FileSaveChunkEditDeltaSink::write_chunk_delta(const save::ChunkEditSaveRecord& chunk_delta) const {
    return database_->write_chunk_delta(chunk_delta);
}

core::Result<ChunkStreamLoadReport>
ChunkStreamer::load_chunk(WorldState& state, ChunkCoord coord,
                          const TerrainGenerationConfig& generation, const RegionGraph& regions,
                          const VoxelPalette& palette, const IChunkEditDeltaSource* saved_deltas) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("streaming.load_chunk");
    ChunkStreamLoadReport report;
    report.coord = coord;

    if (state.chunks().contains(coord)) {
        report.identity = state.chunks().find(coord)->identity();
        report.chunk_was_already_loaded = true;
        report.source = ChunkStreamLoadSource::already_loaded;
        return core::Result<ChunkStreamLoadReport>::success(report);
    }

    auto generated =
        DeterministicTerrainGenerator::generate_chunk(coord, generation, regions, palette);
    if (!generated) {
        return core::Result<ChunkStreamLoadReport>::failure(generated.error().code,
                                                            generated.error().message);
    }

    if (saved_deltas == nullptr) {
        auto status =
            state.chunks().insert_generated(std::move(generated).value(), state.dirty_regions());
        if (!status) {
            return core::Result<ChunkStreamLoadReport>::failure(status.error().code,
                                                                status.error().message);
        }
        report.generated_chunk_inserted = true;
        report.identity = state.chunks().find(coord)->identity();
        report.source = ChunkStreamLoadSource::generated;
        return core::Result<ChunkStreamLoadReport>::success(report);
    }

    // Read and decode persistence before publishing the generated chunk. Failure must leave the
    // live database exactly as it was before this load request.
    auto delta = saved_deltas->read_chunk_delta(coord);
    if (!delta) {
        return core::Result<ChunkStreamLoadReport>::failure(delta.error().code,
                                                            delta.error().message);
    }
    if (!delta.value().has_value()) {
        auto status =
            state.chunks().insert_generated(std::move(generated).value(), state.dirty_regions());
        if (!status) {
            return core::Result<ChunkStreamLoadReport>::failure(status.error().code,
                                                                status.error().message);
        }
        report.generated_chunk_inserted = true;
        report.identity = state.chunks().find(coord)->identity();
        report.source = ChunkStreamLoadSource::generated;
        return core::Result<ChunkStreamLoadReport>::success(report);
    }

    auto edits = ChunkEditDeltaTextCodec::decode(coord, delta.value()->encoded_edit_delta);
    if (!edits) {
        return core::Result<ChunkStreamLoadReport>::failure(edits.error().code,
                                                            edits.error().message);
    }

    auto status = state.chunks().insert_generated_with_saved_edits(
        std::move(generated).value(), edits.value(), state.dirty_regions());
    if (!status) {
        return core::Result<ChunkStreamLoadReport>::failure(status.error().code,
                                                            status.error().message);
    }

    report.generated_chunk_inserted = true;
    report.identity = state.chunks().find(coord)->identity();
    report.source = ChunkStreamLoadSource::generated_with_saved_delta;
    report.saved_delta_applied = true;
    report.saved_edit_count = edits.value().size();
    return core::Result<ChunkStreamLoadReport>::success(report);
}

core::Status ChunkStreamInterestPolicy::validate() const {
    if (load_horizontal_radius_chunks > retain_horizontal_radius_chunks ||
        load_vertical_radius_chunks > retain_vertical_radius_chunks) {
        return core::Status::failure(
            "chunk_stream.invalid_interest_radius",
            "chunk stream load radii must be less than or equal to retain radii");
    }
    if (load_horizontal_radius_chunks > max_load_radius_chunks ||
        load_vertical_radius_chunks > max_load_radius_chunks) {
        return core::Status::failure(
            "chunk_stream.load_radius_too_large",
            "chunk stream load radius exceeds the bounded planning budget");
    }
    return core::Status::ok();
}

core::Result<ChunkStreamInterestPlan>
ChunkStreamer::plan_interest(const WorldState& state,
                             const std::vector<simulation::SimulationViewer>& viewers,
                             const ChunkStreamInterestPolicy& policy) {
    auto status = policy.validate();
    if (!status) {
        return core::Result<ChunkStreamInterestPlan>::failure(status.error().code,
                                                              status.error().message);
    }

    std::vector<ChunkCoord> viewer_chunks;
    viewer_chunks.reserve(viewers.size());
    for (const auto& viewer : viewers) {
        viewer_chunks.push_back(chunk_coord_for_simulation_coord(viewer.coord));
    }

    std::set<ChunkCoord> desired;
    const auto horizontal_radius = static_cast<std::int64_t>(policy.load_horizontal_radius_chunks);
    const auto vertical_radius = static_cast<std::int64_t>(policy.load_vertical_radius_chunks);
    for (const auto center : viewer_chunks) {
        const auto min_z = saturated_subtract(center.z, horizontal_radius);
        const auto max_z = saturated_add(center.z, horizontal_radius);
        const auto min_y = saturated_subtract(center.y, vertical_radius);
        const auto max_y = saturated_add(center.y, vertical_radius);
        const auto min_x = saturated_subtract(center.x, horizontal_radius);
        const auto max_x = saturated_add(center.x, horizontal_radius);
        for (auto z = min_z;; ++z) {
            for (auto x = min_x;; ++x) {
                if (within_cylinder({x, center.y, z}, center, policy.load_horizontal_radius_chunks,
                                    0)) {
                    for (auto y = min_y;; ++y) {
                        desired.insert({x, y, z});
                        if (y == max_y) {
                            break;
                        }
                    }
                }
                if (x == max_x) {
                    break;
                }
            }
            if (z == max_z) {
                break;
            }
        }
    }

    std::set<ChunkCoord> loaded;
    for (const auto* chunk : state.chunks().records()) {
        loaded.insert(chunk->coord());
    }

    ChunkStreamInterestPlan plan;
    plan.viewer_count = viewers.size();
    plan.loaded_chunk_count = loaded.size();
    plan.desired_chunk_count = desired.size();

    for (const auto coord : desired) {
        if (!loaded.contains(coord)) {
            plan.load_requests.push_back(coord);
        }
    }

    for (const auto* chunk : state.chunks().records()) {
        const auto coord = chunk->coord();
        if (within_any_cylinder(coord, viewer_chunks, policy.retain_horizontal_radius_chunks,
                                policy.retain_vertical_radius_chunks)) {
            plan.retained_chunks.push_back(coord);
            continue;
        }
        if (is_dirty_for_persistence_or_replication(*chunk)) {
            plan.pinned_dirty_chunks.push_back(coord);
        } else {
            plan.evictable_chunks.push_back(coord);
        }
    }

    std::ranges::sort(plan.load_requests);
    std::ranges::sort(plan.evictable_chunks);
    std::ranges::sort(plan.retained_chunks);
    std::ranges::sort(plan.pinned_dirty_chunks);
    return core::Result<ChunkStreamInterestPlan>::success(std::move(plan));
}

std::size_t ChunkStreamEvictionReport::evicted_count() const noexcept {
    return evicted_chunks.size();
}

ChunkStreamEvictionReport
ChunkStreamer::evict_chunks(WorldState& state, std::span<const ChunkCoord> requested_chunks) {
    ChunkStreamEvictionReport report;
    report.requested_count = requested_chunks.size();

    std::set<ChunkCoord> unique_requests(requested_chunks.begin(), requested_chunks.end());
    for (const auto coord : unique_requests) {
        const auto* chunk = state.chunks().find(coord);
        if (chunk == nullptr) {
            report.missing_chunks.push_back(coord);
            continue;
        }
        if (is_dirty_for_persistence_or_replication(*chunk)) {
            report.retained_dirty_chunks.push_back(coord);
            continue;
        }
        const auto identity = chunk->identity();
        if (state.chunks().erase(coord)) {
            report.evicted_chunks.push_back(coord);
            report.evicted_identities.push_back(identity);
        } else {
            report.missing_chunks.push_back(coord);
        }
    }

    return report;
}

std::size_t ChunkStreamSaveFlushReport::written_count() const noexcept {
    return written_chunks.size();
}

core::Result<ChunkStreamSaveFlushReport>
ChunkStreamer::flush_save_deltas(WorldState& state, std::span<const ChunkCoord> requested_chunks,
                                 const IChunkEditDeltaSink& sink) {
    ChunkStreamSaveFlushReport report;
    report.requested_count = requested_chunks.size();

    const auto edits_by_chunk = edits_grouped_by_chunk(state.chunks());

    std::set<ChunkCoord> unique_requests(requested_chunks.begin(), requested_chunks.end());
    for (const auto coord : unique_requests) {
        auto* chunk = state.chunks().find(coord);
        if (chunk == nullptr) {
            report.missing_chunks.push_back(coord);
            continue;
        }
        if (!chunk->dirty().contains(ChunkDirtyFlag::save)) {
            report.clean_chunks.push_back(coord);
            continue;
        }

        const auto found_edits = edits_by_chunk.find(coord);
        if (found_edits == edits_by_chunk.end() || found_edits->second.empty()) {
            report.dirty_without_delta_chunks.push_back(coord);
            continue;
        }

        save::ChunkEditSaveRecord record;
        record.coord = coord;
        record.encoded_edit_delta = ChunkEditDeltaTextCodec::encode(coord, found_edits->second);

        const auto ticket = chunk->ensure_stage_requested(ChunkStage::persistence);
        auto stage_status = chunk->mark_stage_running(ticket);
        if (!stage_status) {
            return core::Result<ChunkStreamSaveFlushReport>::failure(stage_status.error().code,
                                                                     stage_status.error().message);
        }

        auto status = sink.write_chunk_delta(record);
        if (!status) {
            auto* current = state.chunks().find(coord);
            if (current != nullptr && current->identity() == ticket.identity &&
                current->stage_ticket_is_current(ticket)) {
                static_cast<void>(current->retry_stage(ticket));
            }
            return core::Result<ChunkStreamSaveFlushReport>::failure(status.error().code,
                                                                     status.error().message);
        }

        auto* current = state.chunks().find(coord);
        if (current == nullptr || current->identity() != ticket.identity ||
            !current->stage_ticket_is_current(ticket)) {
            if (current != nullptr && current->identity() == ticket.identity) {
                static_cast<void>(current->note_stage_stale(ticket));
            }
            report.stale_chunks.push_back(coord);
            continue;
        }
        stage_status = current->mark_stage_ready(ticket);
        if (!stage_status) {
            return core::Result<ChunkStreamSaveFlushReport>::failure(stage_status.error().code,
                                                                     stage_status.error().message);
        }
        stage_status = current->publish_stage(ticket);
        if (!stage_status) {
            return core::Result<ChunkStreamSaveFlushReport>::failure(stage_status.error().code,
                                                                     stage_status.error().message);
        }
        current->clear_dirty(ChunkDirtyFlag::save);
        report.written_chunks.push_back(coord);
    }

    return core::Result<ChunkStreamSaveFlushReport>::success(std::move(report));
}

std::size_t ChunkStreamReplicationFlushReport::replicated_count() const noexcept {
    return replicated_chunks.size();
}

core::Result<ChunkStreamReplicationFlushReport>
ChunkStreamer::flush_replication_deltas(WorldState& state,
                                        std::span<const ChunkCoord> requested_chunks,
                                        const IChunkReplicationDeltaSink& sink) {
    ChunkStreamReplicationFlushReport report;
    report.requested_count = requested_chunks.size();

    const auto edits_by_chunk = edits_grouped_by_chunk(state.chunks());
    std::set<ChunkCoord> unique_requests(requested_chunks.begin(), requested_chunks.end());
    for (const auto coord : unique_requests) {
        auto* chunk = state.chunks().find(coord);
        if (chunk == nullptr) {
            report.missing_chunks.push_back(coord);
            continue;
        }
        if (!chunk->dirty().contains(ChunkDirtyFlag::replication)) {
            report.clean_chunks.push_back(coord);
            continue;
        }

        const auto found_edits = edits_by_chunk.find(coord);
        if (found_edits == edits_by_chunk.end() || found_edits->second.empty()) {
            report.dirty_without_delta_chunks.push_back(coord);
            continue;
        }

        save::ChunkEditSaveRecord record;
        record.coord = coord;
        record.encoded_edit_delta = ChunkEditDeltaTextCodec::encode(coord, found_edits->second);

        const auto ticket = chunk->ensure_stage_requested(ChunkStage::replication);
        auto stage_status = chunk->mark_stage_running(ticket);
        if (!stage_status) {
            return core::Result<ChunkStreamReplicationFlushReport>::failure(
                stage_status.error().code, stage_status.error().message);
        }

        auto status = sink.replicate_chunk_delta(record);
        if (!status) {
            auto* current = state.chunks().find(coord);
            if (current != nullptr && current->identity() == ticket.identity &&
                current->stage_ticket_is_current(ticket)) {
                static_cast<void>(current->retry_stage(ticket));
            }
            return core::Result<ChunkStreamReplicationFlushReport>::failure(status.error().code,
                                                                            status.error().message);
        }

        auto* current = state.chunks().find(coord);
        if (current == nullptr || current->identity() != ticket.identity ||
            !current->stage_ticket_is_current(ticket)) {
            if (current != nullptr && current->identity() == ticket.identity) {
                static_cast<void>(current->note_stage_stale(ticket));
            }
            report.stale_chunks.push_back(coord);
            continue;
        }
        stage_status = current->mark_stage_ready(ticket);
        if (!stage_status) {
            return core::Result<ChunkStreamReplicationFlushReport>::failure(
                stage_status.error().code, stage_status.error().message);
        }
        stage_status = current->publish_stage(ticket);
        if (!stage_status) {
            return core::Result<ChunkStreamReplicationFlushReport>::failure(
                stage_status.error().code, stage_status.error().message);
        }
        current->clear_dirty(ChunkDirtyFlag::replication);
        report.replicated_chunks.push_back(coord);
    }

    return core::Result<ChunkStreamReplicationFlushReport>::success(std::move(report));
}

std::size_t ChunkStreamMaintenanceReport::loaded_count() const noexcept {
    return loads.size();
}

core::Result<ChunkStreamMaintenanceReport> ChunkStreamer::maintain_interest(
    WorldState& state, const std::vector<simulation::SimulationViewer>& viewers,
    const ChunkStreamInterestPolicy& policy, const IChunkEditDeltaSink* save_sink,
    const IChunkReplicationDeltaSink* replication_sink) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("streaming.maintain_interest");
    auto interest = plan_interest(state, viewers, policy);
    if (!interest) {
        return core::Result<ChunkStreamMaintenanceReport>::failure(interest.error().code,
                                                                   interest.error().message);
    }

    ChunkStreamMaintenanceReport report;
    report.interest = std::move(interest).value();

    if (save_sink != nullptr) {
        auto save_flush = flush_save_deltas(state, report.interest.pinned_dirty_chunks, *save_sink);
        if (!save_flush) {
            return core::Result<ChunkStreamMaintenanceReport>::failure(save_flush.error().code,
                                                                       save_flush.error().message);
        }
        report.save_flush = std::move(save_flush).value();
    }

    if (replication_sink != nullptr) {
        auto replication_flush =
            flush_replication_deltas(state, report.interest.pinned_dirty_chunks, *replication_sink);
        if (!replication_flush) {
            return core::Result<ChunkStreamMaintenanceReport>::failure(
                replication_flush.error().code, replication_flush.error().message);
        }
        report.replication_flush = std::move(replication_flush).value();
    }

    std::set<ChunkCoord> eviction_requests(report.interest.evictable_chunks.begin(),
                                           report.interest.evictable_chunks.end());
    eviction_requests.insert(report.interest.pinned_dirty_chunks.begin(),
                             report.interest.pinned_dirty_chunks.end());
    const std::vector<ChunkCoord> ordered_eviction_requests(eviction_requests.begin(),
                                                            eviction_requests.end());
    report.eviction = evict_chunks(state, ordered_eviction_requests);

    return core::Result<ChunkStreamMaintenanceReport>::success(std::move(report));
}

core::Result<ChunkStreamMaintenanceReport> ChunkStreamer::maintain_loaded_interest(
    WorldState& state, const std::vector<simulation::SimulationViewer>& viewers,
    const ChunkStreamInterestPolicy& policy, const TerrainGenerationConfig& generation,
    const RegionGraph& regions, const VoxelPalette& palette,
    const IChunkEditDeltaSource* saved_deltas, const IChunkEditDeltaSink* save_sink,
    const IChunkReplicationDeltaSink* replication_sink) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("streaming.maintain_loaded_interest");
    auto interest = plan_interest(state, viewers, policy);
    if (!interest) {
        return core::Result<ChunkStreamMaintenanceReport>::failure(interest.error().code,
                                                                   interest.error().message);
    }

    ChunkStreamMaintenanceReport report;
    report.interest = std::move(interest).value();
    report.loads.reserve(report.interest.load_requests.size());

    for (const auto coord : report.interest.load_requests) {
        auto load = load_chunk(state, coord, generation, regions, palette, saved_deltas);
        if (!load) {
            return core::Result<ChunkStreamMaintenanceReport>::failure(load.error().code,
                                                                       load.error().message);
        }
        report.loads.push_back(std::move(load).value());
    }

    if (save_sink != nullptr) {
        auto save_flush = flush_save_deltas(state, report.interest.pinned_dirty_chunks, *save_sink);
        if (!save_flush) {
            return core::Result<ChunkStreamMaintenanceReport>::failure(save_flush.error().code,
                                                                       save_flush.error().message);
        }
        report.save_flush = std::move(save_flush).value();
    }

    if (replication_sink != nullptr) {
        auto replication_flush =
            flush_replication_deltas(state, report.interest.pinned_dirty_chunks, *replication_sink);
        if (!replication_flush) {
            return core::Result<ChunkStreamMaintenanceReport>::failure(
                replication_flush.error().code, replication_flush.error().message);
        }
        report.replication_flush = std::move(replication_flush).value();
    }

    std::set<ChunkCoord> eviction_requests(report.interest.evictable_chunks.begin(),
                                           report.interest.evictable_chunks.end());
    eviction_requests.insert(report.interest.pinned_dirty_chunks.begin(),
                             report.interest.pinned_dirty_chunks.end());
    const std::vector<ChunkCoord> ordered_eviction_requests(eviction_requests.begin(),
                                                            eviction_requests.end());
    report.eviction = evict_chunks(state, ordered_eviction_requests);

    return core::Result<ChunkStreamMaintenanceReport>::success(std::move(report));
}

const char* chunk_stream_load_source_name(ChunkStreamLoadSource source) noexcept {
    switch (source) {
    case ChunkStreamLoadSource::already_loaded:
        return "already_loaded";
    case ChunkStreamLoadSource::generated:
        return "generated";
    case ChunkStreamLoadSource::generated_with_saved_delta:
        return "generated_with_saved_delta";
    }
    return "unknown";
}

ChunkCoord chunk_coord_for_simulation_coord(simulation::SimulationCoord coord) noexcept {
    return chunk_coord_for_block(coord);
}

} // namespace heartstead::world
