#include "engine/world/fluids/chunk_fluid_system.hpp"

#include "engine/profiling/profiler.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace heartstead::world {

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double elapsed_milliseconds(Clock::time_point begin) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

[[nodiscard]] bool chunk_in_bounds(ChunkCoord coordinate, ChunkCoord minimum,
                                   ChunkCoord maximum) noexcept {
    return coordinate.x >= minimum.x && coordinate.x <= maximum.x &&
           coordinate.y >= minimum.y && coordinate.y <= maximum.y &&
           coordinate.z >= minimum.z && coordinate.z <= maximum.z;
}

} // namespace

core::Status ChunkFluidSystemConfig::validate() const {
    if (simulation_tick_interval == 0 || maximum_active_cells_per_step == 0 ||
        !std::isfinite(apply_time_budget_ms) || apply_time_budget_ms <= 0.0) {
        return core::Status::failure(
            "chunk_fluid_system.invalid_config",
            "fluid system requires nonzero tick/cell budgets and a finite positive apply budget");
    }
    return core::Status::ok();
}

core::Result<std::unique_ptr<ChunkFluidSystem>>
ChunkFluidSystem::create(const VoxelPalette& palette, ChunkFluidSystemConfig config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<std::unique_ptr<ChunkFluidSystem>>::failure(status.error().code,
                                                                       status.error().message);
    }
    auto table = build_fluid_block_table(palette);
    status = table.validate();
    if (!status) {
        return core::Result<std::unique_ptr<ChunkFluidSystem>>::failure(status.error().code,
                                                                       status.error().message);
    }
    return core::Result<std::unique_ptr<ChunkFluidSystem>>::success(
        std::unique_ptr<ChunkFluidSystem>(
            new ChunkFluidSystem(config, std::move(table))));
}

ChunkFluidSystem::ChunkFluidSystem(ChunkFluidSystemConfig config, FluidBlockTable block_table)
    : config_(config), block_table_(std::move(block_table)) {}

core::Status ChunkFluidSystem::update(ChunkDatabase& chunks,
                                      dirty::DirtyRegionTracker& dirty_regions,
                                      const VoxelPalette& palette, std::uint64_t tick) {
    stats_.step_due = tick % config_.simulation_tick_interval == 0;
    stats_.budget_exhausted = false;
    stats_.processed_cells_this_update = 0;
    stats_.deferred_cells_this_update = 0;
    stats_.proposals_this_update = 0;
    stats_.changed_cells_this_update = 0;
    stats_.changed_chunks_this_update = 0;
    stats_.last_topology_reconciliation_ms = 0.0;
    stats_.last_dirty_collection_ms = 0.0;
    stats_.last_snapshot_ms = 0.0;
    stats_.last_simulation_ms = 0.0;
    stats_.last_apply_ms = 0.0;
    changed_chunks_.clear();

    auto status = refresh_block_table(palette);
    if (!status) {
        return status;
    }
    const auto topology_begin = Clock::now();
    reconcile_topology(chunks);
    stats_.last_topology_reconciliation_ms = elapsed_milliseconds(topology_begin);
    const auto dirty_begin = Clock::now();
    collect_dirty(chunks, dirty_regions);
    stats_.last_dirty_collection_ms = elapsed_milliseconds(dirty_begin);
    HEARTSTEAD_PROFILE_PLOT("voxel_fluid.topology_ms", stats_.last_topology_reconciliation_ms);
    HEARTSTEAD_PROFILE_PLOT("voxel_fluid.dirty_collection_ms", stats_.last_dirty_collection_ms);
    stats_.active_cell_count = active_.size();
    if (!stats_.step_due || active_.empty()) {
        return core::Status::ok();
    }

    const auto snapshot_begin = Clock::now();
    auto snapshot = build_fluid_simulation_snapshot(chunks);
    stats_.last_snapshot_ms = elapsed_milliseconds(snapshot_begin);
    std::vector<ChunkLocalCoord> active(active_.begin(), active_.end());

    const auto simulation_begin = Clock::now();
    auto step = simulate_fluid_step(snapshot, block_table_, active,
                                    config_.maximum_active_cells_per_step, tick);
    stats_.last_simulation_ms = elapsed_milliseconds(simulation_begin);
    if (!step) {
        return core::Status::failure(step.error().code, step.error().message);
    }

    const auto apply_begin = Clock::now();
    auto applied = apply_fluid_step(chunks, dirty_regions, step.value());
    stats_.last_apply_ms = elapsed_milliseconds(apply_begin);
    if (!applied) {
        return core::Status::failure(applied.error().code, applied.error().message);
    }
    if (stats_.last_apply_ms > config_.apply_time_budget_ms) {
        ++stats_.apply_budget_overruns;
    }

    active_.clear();
    active_.insert(step.value().next_active.begin(), step.value().next_active.end());
    changed_chunks_ = std::move(applied.value().changed_chunks);
    stats_.budget_exhausted = step.value().stats.budget_exhausted;
    stats_.processed_cells_this_update = step.value().stats.processed_active_cell_count;
    stats_.deferred_cells_this_update = step.value().stats.deferred_active_cell_count;
    stats_.proposals_this_update = step.value().stats.proposal_count;
    stats_.changed_cells_this_update = step.value().stats.changed_cell_count;
    stats_.changed_chunks_this_update = changed_chunks_.size();
    stats_.active_cell_count = active_.size();
    ++stats_.steps;
    stats_.total_processed_cells += step.value().stats.processed_active_cell_count;
    stats_.total_changed_cells += step.value().stats.changed_cell_count;
    if (step.value().stats.budget_exhausted) {
        ++stats_.budget_exhaustions;
    }
    if (active_.empty()) {
        ++stats_.settled_steps;
    }
    return core::Status::ok();
}

void ChunkFluidSystem::activate(BlockCoord position) {
    activate(block_to_chunk_local(position));
}

void ChunkFluidSystem::activate(ChunkLocalCoord address) {
    active_.insert(address);
}

void ChunkFluidSystem::clear() noexcept {
    active_.clear();
    known_identities_.clear();
    changed_chunks_.clear();
    stats_ = {};
}

std::span<const ChunkCoord> ChunkFluidSystem::changed_chunks() const noexcept {
    return changed_chunks_;
}

const ChunkFluidSystemStats& ChunkFluidSystem::stats() const noexcept {
    return stats_;
}

core::Status ChunkFluidSystem::refresh_block_table(const VoxelPalette& palette) {
    if (block_table_.revision == palette.render_revision()) {
        return core::Status::ok();
    }
    auto table = build_fluid_block_table(palette);
    auto status = table.validate();
    if (!status) {
        return status;
    }
    block_table_ = std::move(table);
    known_identities_.clear();
    return core::Status::ok();
}

void ChunkFluidSystem::reconcile_topology(const ChunkDatabase& chunks) {
    const auto identities = chunks.identities();
    if (identities == known_identities_) {
        return;
    }
    known_identities_ = identities;
    rebuild_frontier(chunks);
    ++stats_.topology_rebuilds;
}

void ChunkFluidSystem::collect_dirty(const ChunkDatabase& chunks,
                                     dirty::DirtyRegionTracker& dirty_regions) {
    auto regions = dirty_regions.consume_kind(dirty::DirtyRegionKind::water_network);
    stats_.dirty_regions_consumed += regions.size();
    for (const auto& region : regions) {
        activate_region(chunks, region.bounds);
    }
}

void ChunkFluidSystem::activate_region(const ChunkDatabase& chunks,
                                       const dirty::DirtyRegionBounds& bounds) {
    const auto minimum_chunk = chunk_coord_for_block(bounds.min);
    const auto maximum_chunk = chunk_coord_for_block(bounds.max);
    const auto minimum_local = local_coord_for_block(bounds.min);
    const auto maximum_local = local_coord_for_block(bounds.max);
    for (const auto identity : chunks.identities()) {
        if (!chunk_in_bounds(identity.coordinate, minimum_chunk, maximum_chunk)) {
            continue;
        }
        const VoxelCoord local_min{
            identity.coordinate.x == minimum_chunk.x ? minimum_local.x : std::uint16_t{0},
            identity.coordinate.y == minimum_chunk.y ? minimum_local.y : std::uint16_t{0},
            identity.coordinate.z == minimum_chunk.z ? minimum_local.z : std::uint16_t{0},
        };
        const VoxelCoord local_max{
            identity.coordinate.x == maximum_chunk.x
                ? maximum_local.x
                : static_cast<std::uint16_t>(VoxelChunk::edge_length - 1U),
            identity.coordinate.y == maximum_chunk.y
                ? maximum_local.y
                : static_cast<std::uint16_t>(VoxelChunk::edge_length - 1U),
            identity.coordinate.z == maximum_chunk.z
                ? maximum_local.z
                : static_cast<std::uint16_t>(VoxelChunk::edge_length - 1U),
        };
        for (std::uint16_t z = local_min.z; z <= local_max.z; ++z) {
            for (std::uint16_t y = local_min.y; y <= local_max.y; ++y) {
                for (std::uint16_t x = local_min.x; x <= local_max.x; ++x) {
                    active_.insert({identity.coordinate, {x, y, z}});
                }
            }
        }
    }
}

void ChunkFluidSystem::rebuild_frontier(const ChunkDatabase& chunks) {
    auto snapshot = build_fluid_simulation_snapshot(chunks);
    auto reconstructed = fluid_cells_and_neighbors(snapshot, block_table_);
    active_.clear();
    active_.insert(reconstructed.begin(), reconstructed.end());
}

} // namespace heartstead::world
