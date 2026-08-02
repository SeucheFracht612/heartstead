#include "engine/world/voxels/voxel_chunk.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace heartstead::world {

namespace {

[[nodiscard]] constexpr std::uint8_t bit(ChunkDirtyFlag flag) noexcept {
    return static_cast<std::uint8_t>(flag);
}

[[nodiscard]] constexpr ChunkStage stage_for_dirty_flag(ChunkDirtyFlag flag) noexcept {
    switch (flag) {
    case ChunkDirtyFlag::mesh:
        return ChunkStage::mesh;
    case ChunkDirtyFlag::collision:
        return ChunkStage::collision;
    case ChunkDirtyFlag::lighting:
        return ChunkStage::lighting;
    case ChunkDirtyFlag::save:
        return ChunkStage::persistence;
    case ChunkDirtyFlag::replication:
        return ChunkStage::replication;
    }
    return ChunkStage::count;
}

[[nodiscard]] core::Status validate_ticket_identity(ChunkIdentity current,
                                                    ChunkStageTicket ticket) {
    if (!ticket.is_valid()) {
        return core::Status::failure(
            "chunk_stage.invalid_ticket",
            "chunk stage ticket must contain a valid identity and revision");
    }
    if (ticket.identity != current) {
        return core::Status::failure("chunk_stage.stale_identity",
                                     "chunk stage ticket names a stale load generation");
    }
    return core::Status::ok();
}

} // namespace

void ChunkDirtyState::mark(ChunkDirtyFlag flag) noexcept {
    bits_ = static_cast<std::uint8_t>(bits_ | bit(flag));
}

void ChunkDirtyState::clear(ChunkDirtyFlag flag) noexcept {
    bits_ = static_cast<std::uint8_t>(bits_ & ~bit(flag));
}

void ChunkDirtyState::clear_all() noexcept {
    bits_ = 0;
}

bool ChunkDirtyState::contains(ChunkDirtyFlag flag) const noexcept {
    return (bits_ & bit(flag)) != 0;
}

std::uint8_t ChunkDirtyState::bits() const noexcept {
    return bits_;
}

VoxelChunk::VoxelChunk(ChunkCoord coord)
    : coord_(coord),
      cells_(std::make_shared<std::vector<VoxelCell>>(total_cells, VoxelCell::air())) {}

ChunkCoord VoxelChunk::coord() const noexcept {
    return coord_;
}

ChunkIdentity VoxelChunk::identity() const noexcept {
    return {coord_, load_generation_};
}

std::uint64_t VoxelChunk::content_revision() const noexcept {
    return content_revision_;
}

const ChunkDirtyState& VoxelChunk::dirty() const noexcept {
    return dirty_;
}

const ChunkStageLedger& VoxelChunk::stages() const noexcept {
    return stages_;
}

const VoxelOccupancyMask& VoxelChunk::occupancy() const noexcept {
    return occupancy_;
}

ChunkStageTicket VoxelChunk::stage_ticket(ChunkStage stage) const noexcept {
    if (stage == ChunkStage::count) {
        return {};
    }
    return {identity(), stage, stages_.requested_revision(stage)};
}

bool VoxelChunk::stage_ticket_is_current(ChunkStageTicket ticket) const noexcept {
    return ticket.is_valid() && ticket.identity == identity() &&
           stages_.is_current(ticket.stage, ticket.revision);
}

std::span<const VoxelCell> VoxelChunk::cells() const noexcept {
    return *cells_;
}

core::Result<VoxelCell> VoxelChunk::get(VoxelCoord coord) const {
    if (!contains(coord)) {
        return core::Result<VoxelCell>::failure("chunk.coord_out_of_bounds",
                                                "voxel coordinate is outside the chunk");
    }

    return core::Result<VoxelCell>::success((*cells_)[index_of(coord)]);
}

core::Status VoxelChunk::set(VoxelCoord coord, VoxelCell cell) {
    return set(coord, cell, {});
}

core::Status VoxelChunk::set(VoxelCoord coord, VoxelCell cell,
                             VoxelDerivedInvalidation invalidation) {
    if (!contains(coord)) {
        return core::Status::failure("chunk.coord_out_of_bounds",
                                     "voxel coordinate is outside the chunk");
    }

    const auto index = index_of(coord);
    if ((*cells_)[index] == cell) {
        return core::Status::ok();
    }

    ensure_unique_cells();
    auto& current = (*cells_)[index];
    occupancy_.set_occupied(index, !cell.is_air());
    current = cell;
    advance_content_revision();
    if (invalidation.mesh) {
        invalidate(ChunkDirtyFlag::mesh);
    }
    if (invalidation.collision) {
        invalidate(ChunkDirtyFlag::collision);
    }
    if (invalidation.lighting) {
        invalidate(ChunkDirtyFlag::lighting);
    }
    invalidate(ChunkDirtyFlag::save);
    invalidate(ChunkDirtyFlag::replication);
    return core::Status::ok();
}

core::Status VoxelChunk::apply_saved_cell(VoxelCoord coord, VoxelCell cell) {
    if (!contains(coord)) {
        return core::Status::failure("chunk.coord_out_of_bounds",
                                     "voxel coordinate is outside the chunk");
    }

    const auto index = index_of(coord);
    if ((*cells_)[index] == cell) {
        return core::Status::ok();
    }

    ensure_unique_cells();
    auto& current = (*cells_)[index];
    occupancy_.set_occupied(index, !cell.is_air());
    current = cell;
    advance_content_revision();
    invalidate(ChunkDirtyFlag::mesh);
    invalidate(ChunkDirtyFlag::collision);
    invalidate(ChunkDirtyFlag::lighting);
    return core::Status::ok();
}

core::Result<std::size_t> VoxelChunk::apply_derived_light(std::span<const std::uint8_t> light) {
    if (light.size() != total_cells) {
        return core::Result<std::size_t>::failure(
            "chunk.invalid_derived_light_count",
            "derived voxel light field cell count does not match chunk size");
    }
    if (std::ranges::equal(*cells_, light, [](const VoxelCell& cell, std::uint8_t value) {
            return cell.light == value;
        })) {
        return core::Result<std::size_t>::success(0);
    }

    ensure_unique_cells();
    std::size_t changed = 0;
    for (std::size_t index = 0; index < cells_->size(); ++index) {
        if ((*cells_)[index].light == light[index]) {
            continue;
        }
        (*cells_)[index].light = light[index];
        ++changed;
    }
    if (changed > 0) {
        advance_content_revision();
        invalidate(ChunkDirtyFlag::mesh);
        invalidate(ChunkDirtyFlag::replication);
    }
    return core::Result<std::size_t>::success(changed);
}

core::Status VoxelChunk::load_generated_cells(std::vector<VoxelCell> cells) {
    if (cells.size() != total_cells) {
        return core::Status::failure("chunk.invalid_generated_cell_count",
                                     "generated chunk cell count does not match chunk size");
    }

    cells_ = std::make_shared<std::vector<VoxelCell>>(std::move(cells));
    occupancy_.rebuild(*cells_);
    advance_content_revision();
    dirty_.clear_all();
    invalidate(ChunkDirtyFlag::mesh);
    invalidate(ChunkDirtyFlag::collision);
    invalidate(ChunkDirtyFlag::lighting);
    return core::Status::ok();
}

void VoxelChunk::fill(VoxelCell cell) {
    if (std::ranges::all_of(*cells_,
                            [cell](const VoxelCell& current) { return current == cell; })) {
        return;
    }

    ensure_unique_cells();
    std::ranges::fill(*cells_, cell);
    occupancy_.rebuild(*cells_);
    advance_content_revision();
    invalidate(ChunkDirtyFlag::mesh);
    invalidate(ChunkDirtyFlag::collision);
    invalidate(ChunkDirtyFlag::lighting);
    invalidate(ChunkDirtyFlag::save);
    invalidate(ChunkDirtyFlag::replication);
}

void VoxelChunk::mark_dirty(ChunkDirtyFlag flag) noexcept {
    invalidate(flag);
}

void VoxelChunk::clear_dirty(ChunkDirtyFlag flag) noexcept {
    dirty_.clear(flag);
}

void VoxelChunk::clear_all_dirty() noexcept {
    dirty_.clear_all();
}

ChunkStageTicket VoxelChunk::request_stage(ChunkStage stage) noexcept {
    const auto revision = stages_.request(stage);
    return revision == 0 ? ChunkStageTicket{} : ChunkStageTicket{identity(), stage, revision};
}

ChunkStageTicket VoxelChunk::ensure_stage_requested(ChunkStage stage) noexcept {
    const auto revision = stages_.ensure_requested(stage);
    return revision == 0 ? ChunkStageTicket{} : ChunkStageTicket{identity(), stage, revision};
}

core::Status VoxelChunk::mark_stage_running(ChunkStageTicket ticket) {
    auto status = validate_ticket_identity(identity(), ticket);
    return status ? stages_.mark_running(ticket.stage, ticket.revision) : status;
}

core::Status VoxelChunk::mark_stage_ready(ChunkStageTicket ticket) {
    auto status = validate_ticket_identity(identity(), ticket);
    return status ? stages_.mark_ready(ticket.stage, ticket.revision) : status;
}

core::Status VoxelChunk::publish_stage(ChunkStageTicket ticket, bool output_changed) {
    auto status = validate_ticket_identity(identity(), ticket);
    return status ? stages_.publish(ticket.stage, ticket.revision, output_changed) : status;
}

core::Status VoxelChunk::retry_stage(ChunkStageTicket ticket) {
    auto status = validate_ticket_identity(identity(), ticket);
    return status ? stages_.retry(ticket.stage, ticket.revision) : status;
}

core::Status VoxelChunk::note_stage_stale(ChunkStageTicket ticket) {
    auto status = validate_ticket_identity(identity(), ticket);
    return status ? stages_.note_stale(ticket.stage, ticket.revision) : status;
}

core::Status VoxelChunk::note_stage_cancelled(ChunkStageTicket ticket) {
    auto status = validate_ticket_identity(identity(), ticket);
    return status ? stages_.note_cancelled(ticket.stage, ticket.revision) : status;
}

void VoxelChunk::ensure_unique_cells() {
    if (!cells_.unique()) {
        cells_ = std::make_shared<std::vector<VoxelCell>>(*cells_);
    }
}

void VoxelChunk::invalidate(ChunkDirtyFlag flag) noexcept {
    dirty_.mark(flag);
    const auto stage = stage_for_dirty_flag(flag);
    if (stage != ChunkStage::count) {
        static_cast<void>(stages_.request(stage));
    }
}

void VoxelChunk::assign_load_generation(std::uint64_t generation) noexcept {
    load_generation_ = generation;
}

void VoxelChunk::advance_content_revision() noexcept {
    if (content_revision_ == std::numeric_limits<std::uint64_t>::max()) {
        std::terminate();
    }
    ++content_revision_;
    occupancy_.retag(content_revision_);
    stages_.publish_content(content_revision_);
}

bool VoxelChunk::contains(VoxelCoord coord) noexcept {
    return coord.x < edge_length && coord.y < edge_length && coord.z < edge_length;
}

std::size_t VoxelChunk::index_of(VoxelCoord coord) noexcept {
    constexpr auto edge = static_cast<std::size_t>(edge_length);
    return static_cast<std::size_t>(coord.z) * edge * edge +
           static_cast<std::size_t>(coord.y) * edge + static_cast<std::size_t>(coord.x);
}

} // namespace heartstead::world
