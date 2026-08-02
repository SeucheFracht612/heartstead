#include "engine/world/fluids/fluid_simulation.hpp"

#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/fluids/fluid_state.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace heartstead::world {

namespace {

constexpr auto edge = static_cast<std::size_t>(VoxelChunk::edge_length);

[[nodiscard]] constexpr std::size_t cell_index(VoxelCoord coordinate) noexcept {
    return static_cast<std::size_t>(coordinate.z) * edge * edge +
           static_cast<std::size_t>(coordinate.y) * edge + static_cast<std::size_t>(coordinate.x);
}

[[nodiscard]] std::optional<std::int64_t> checked_axis_offset(std::int64_t value,
                                                              int offset) noexcept {
    if (offset < 0 && value == std::numeric_limits<std::int64_t>::min()) {
        return std::nullopt;
    }
    if (offset > 0 && value == std::numeric_limits<std::int64_t>::max()) {
        return std::nullopt;
    }
    return value + offset;
}

[[nodiscard]] std::optional<ChunkLocalCoord> neighbor_address(ChunkLocalCoord address, int dx,
                                                              int dy, int dz) noexcept {
    auto result = address;
    const auto offset_axis = [](std::int64_t& chunk_axis, std::uint16_t& local_axis,
                                int offset) -> bool {
        if (offset < 0) {
            if (local_axis > 0) {
                --local_axis;
                return true;
            }
            const auto previous = checked_axis_offset(chunk_axis, -1);
            if (!previous) {
                return false;
            }
            chunk_axis = *previous;
            local_axis = VoxelChunk::edge_length - 1;
            return true;
        }
        if (offset > 0) {
            if (local_axis + 1 < VoxelChunk::edge_length) {
                ++local_axis;
                return true;
            }
            const auto next = checked_axis_offset(chunk_axis, 1);
            if (!next) {
                return false;
            }
            chunk_axis = *next;
            local_axis = 0;
        }
        return true;
    };
    if (!offset_axis(result.chunk.x, result.local.x, dx) ||
        !offset_axis(result.chunk.y, result.local.y, dy) ||
        !offset_axis(result.chunk.z, result.local.z, dz)) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] const FluidChunkSnapshot*
find_chunk(const FluidSimulationSnapshot& snapshot, ChunkCoord coordinate) noexcept {
    const auto found = std::ranges::lower_bound(
        snapshot.chunks, coordinate, {}, [](const FluidChunkSnapshot& chunk) {
            return chunk.identity.coordinate;
        });
    return found != snapshot.chunks.end() && found->identity.coordinate == coordinate ? &*found
                                                                                       : nullptr;
}

[[nodiscard]] const VoxelCell* find_cell(const FluidSimulationSnapshot& snapshot,
                                         ChunkLocalCoord address) noexcept {
    const auto* chunk = find_chunk(snapshot, address.chunk);
    return chunk == nullptr ? nullptr : &chunk->cells[cell_index(address.local)];
}

struct CellFluidView {
    std::uint16_t type = 0;
    std::uint8_t amount = 0;
    bool fluid = false;
    bool source = false;
    bool falling = false;
};

[[nodiscard]] core::Result<CellFluidView>
cell_view(const FluidSimulationSnapshot& snapshot, const FluidBlockTable& blocks,
          ChunkLocalCoord address) {
    const auto* cell = find_cell(snapshot, address);
    if (cell == nullptr || cell->is_air()) {
        return core::Result<CellFluidView>::success({});
    }
    if (!blocks.block(cell->type).fluid) {
        return core::Result<CellFluidView>::success({cell->type, 0, false, false, false});
    }
    auto state = decode_fluid_state(cell->state_bits);
    if (!state) {
        return core::Result<CellFluidView>::failure(state.error().code, state.error().message);
    }
    if (cell->metadata_handle != 0) {
        return core::Result<CellFluidView>::failure(
            "fluid_state.metadata_not_supported",
            "fluid simulation encountered a cell with rich-block metadata");
    }
    return core::Result<CellFluidView>::success(
        {cell->type, state.value().amount, true, state.value().source, state.value().falling});
}

struct LateralDirection {
    int dx = 0;
    int dz = 0;
    FluidFlowDirection incoming_flow = FluidFlowDirection::none;
};

constexpr std::array<LateralDirection, 4> lateral_directions{{
    {-1, 0, FluidFlowDirection::positive_x},
    {1, 0, FluidFlowDirection::negative_x},
    {0, -1, FluidFlowDirection::positive_z},
    {0, 1, FluidFlowDirection::negative_z},
}};

struct StateProposal {
    ChunkLocalCoord address{};
    VoxelCell previous{};
    VoxelCell next{};
};

[[nodiscard]] core::Status validate_active_cells(
    const FluidSimulationSnapshot& snapshot, std::span<const ChunkLocalCoord> active_cells) {
    ChunkLocalCoord previous{};
    bool has_previous = false;
    for (const auto address : active_cells) {
        if (!is_valid_local_coord(address.local) || find_chunk(snapshot, address.chunk) == nullptr) {
            return core::Status::failure(
                "fluid_simulation.invalid_active_cell",
                "active fluid cells must address resident snapshot cells");
        }
        if (has_previous && !(previous < address)) {
            return core::Status::failure(
                "fluid_simulation.unsorted_active_cells",
                "active fluid cells must be strictly sorted and unique");
        }
        previous = address;
        has_previous = true;
    }
    return core::Status::ok();
}

void activate_resident_neighbors(const FluidSimulationSnapshot& snapshot, ChunkLocalCoord address,
                                 std::set<ChunkLocalCoord>& active) {
    active.insert(address);
    constexpr std::array<std::array<int, 3>, 6> directions{{
        {{-1, 0, 0}},
        {{1, 0, 0}},
        {{0, -1, 0}},
        {{0, 1, 0}},
        {{0, 0, -1}},
        {{0, 0, 1}},
    }};
    for (const auto direction : directions) {
        const auto neighbor =
            neighbor_address(address, direction[0], direction[1], direction[2]);
        if (neighbor && find_chunk(snapshot, neighbor->chunk) != nullptr) {
            active.insert(*neighbor);
        }
    }
}

[[nodiscard]] core::Status mark_chunk_rebuild(dirty::DirtyRegionTracker& dirty_regions,
                                              ChunkCoord coordinate) {
    const dirty::DirtyRegionCoord region{coordinate.x, coordinate.y, coordinate.z};
    for (const auto kind : {dirty::DirtyRegionKind::chunk_mesh,
                            dirty::DirtyRegionKind::chunk_collision,
                            dirty::DirtyRegionKind::chunk_lighting}) {
        auto status = dirty_regions.mark_single(kind, region, "voxel fluid simulation");
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

} // namespace

FluidBlockInfo FluidBlockTable::block(std::uint16_t type) const noexcept {
    return type < blocks.size() ? blocks[type] : FluidBlockInfo{};
}

core::Status FluidBlockTable::validate() const {
    if (revision == 0 || blocks.empty() || blocks.front().fluid) {
        return core::Status::failure(
            "fluid_simulation.invalid_block_table",
            "fluid block table must contain non-fluid air at type zero");
    }
    return core::Status::ok();
}

FluidBlockTable build_fluid_block_table(const VoxelPalette& palette) {
    FluidBlockTable result;
    result.revision = palette.render_revision();
    const auto definitions = palette.definitions();
    std::uint16_t maximum_type = VoxelDefinition::air_type;
    for (const auto* definition : definitions) {
        maximum_type = std::max(maximum_type, definition->type);
    }
    result.blocks.resize(static_cast<std::size_t>(maximum_type) + 1U);
    for (const auto* definition : definitions) {
        result.blocks[definition->type].fluid =
            definition->logical_occupancy == BlockLogicalOccupancy::fluid;
    }
    return result;
}

core::Status FluidChunkSnapshot::validate() const {
    if (!identity.is_valid() || content_revision == 0 || cells.size() != VoxelChunk::total_cells) {
        return core::Status::failure(
            "fluid_simulation.invalid_chunk_snapshot",
            "fluid chunk snapshot requires a valid identity, revision, and complete cell field");
    }
    return core::Status::ok();
}

core::Status FluidSimulationSnapshot::validate() const {
    ChunkCoord previous{};
    bool has_previous = false;
    for (const auto& chunk : chunks) {
        auto status = chunk.validate();
        if (!status) {
            return status;
        }
        if (has_previous && !(previous < chunk.identity.coordinate)) {
            return core::Status::failure(
                "fluid_simulation.unsorted_snapshot",
                "fluid snapshot chunks must be strictly sorted by coordinate");
        }
        previous = chunk.identity.coordinate;
        has_previous = true;
    }
    return core::Status::ok();
}

FluidSimulationSnapshot build_fluid_simulation_snapshot(const ChunkDatabase& chunks) {
    FluidSimulationSnapshot result;
    for (const auto identity : chunks.identities()) {
        const auto* chunk = chunks.find(identity.coordinate);
        if (chunk == nullptr || chunk->identity() != identity) {
            continue;
        }
        result.chunks.push_back(
            {identity, chunk->content_revision(),
             std::vector<VoxelCell>(chunk->cells().begin(), chunk->cells().end())});
    }
    return result;
}

core::Result<FluidStepResult>
simulate_fluid_step(const FluidSimulationSnapshot& snapshot, const FluidBlockTable& blocks,
                    std::span<const ChunkLocalCoord> active_cells,
                    std::size_t maximum_active_cells, std::uint64_t tick) {
    auto status = snapshot.validate();
    if (!status) {
        return core::Result<FluidStepResult>::failure(status.error().code, status.error().message);
    }
    status = blocks.validate();
    if (!status) {
        return core::Result<FluidStepResult>::failure(status.error().code, status.error().message);
    }
    status = validate_active_cells(snapshot, active_cells);
    if (!status) {
        return core::Result<FluidStepResult>::failure(status.error().code, status.error().message);
    }
    if (maximum_active_cells == 0) {
        return core::Result<FluidStepResult>::failure(
            "fluid_simulation.zero_budget",
            "fluid simulation active-cell budget must be nonzero");
    }

    FluidStepResult result;
    result.tick = tick;
    result.stats.input_active_cell_count = active_cells.size();
    const auto process_count = std::min(active_cells.size(), maximum_active_cells);
    result.stats.processed_active_cell_count = process_count;
    result.stats.deferred_active_cell_count = active_cells.size() - process_count;
    result.stats.budget_exhausted = process_count < active_cells.size();

    std::vector<StateProposal> proposals;
    proposals.reserve(process_count);
    for (std::size_t active_index = 0; active_index < process_count; ++active_index) {
        const auto address = active_cells[active_index];
        const auto* previous = find_cell(snapshot, address);
        if (previous == nullptr) {
            continue;
        }
        auto current = cell_view(snapshot, blocks, address);
        if (!current) {
            return core::Result<FluidStepResult>::failure(current.error().code,
                                                          current.error().message);
        }
        if (!current.value().fluid && current.value().type != 0) {
            continue;
        }

        VoxelCell next = current.value().fluid
                             ? VoxelCell{VoxelDefinition::air_type, previous->light}
                             : *previous;
        const auto below = neighbor_address(address, 0, -1, 0);
        std::optional<CellFluidView> below_view;
        if (below && find_chunk(snapshot, below->chunk) != nullptr) {
            auto decoded = cell_view(snapshot, blocks, *below);
            if (!decoded) {
                return core::Result<FluidStepResult>::failure(decoded.error().code,
                                                              decoded.error().message);
            }
            below_view = decoded.value();
        }
        const auto below_is_open_for = [&below_view](std::uint16_t type) {
            return below_view.has_value() &&
                   (below_view->type == 0 ||
                    (below_view->fluid && below_view->type == type &&
                     below_view->amount < maximum_fluid_amount));
        };

        if (current.value().source) {
            FluidState state{maximum_fluid_amount, below_is_open_for(current.value().type), true,
                             FluidFlowDirection::none};
            auto encoded = encode_fluid_state(state);
            if (!encoded) {
                return core::Result<FluidStepResult>::failure(encoded.error().code,
                                                              encoded.error().message);
            }
            next = {current.value().type, previous->light, encoded.value(), 0};
            if (next != *previous) {
                proposals.push_back({address, *previous, next});
            }
            ++result.stats.accepted_transfer_count;
            result.stats.transferred_unit_count += maximum_fluid_amount;
            continue;
        }

        const auto above = neighbor_address(address, 0, 1, 0);
        if (above && find_chunk(snapshot, above->chunk) != nullptr) {
            auto above_view = cell_view(snapshot, blocks, *above);
            if (!above_view) {
                return core::Result<FluidStepResult>::failure(above_view.error().code,
                                                              above_view.error().message);
            }
            const bool compatible = current.value().type == 0 ||
                                    (current.value().fluid &&
                                     current.value().type == above_view.value().type);
            if (compatible && above_view.value().fluid) {
                FluidState state{maximum_fluid_amount, true, false,
                                 FluidFlowDirection::none};
                auto encoded = encode_fluid_state(state);
                if (!encoded) {
                    return core::Result<FluidStepResult>::failure(encoded.error().code,
                                                                  encoded.error().message);
                }
                next = {above_view.value().type, previous->light, encoded.value(), 0};
                if (next != *previous) {
                    proposals.push_back({address, *previous, next});
                }
                ++result.stats.accepted_transfer_count;
                result.stats.transferred_unit_count += maximum_fluid_amount;
                continue;
            }
        }

        const bool supported =
            !below_view.has_value() ||
            (below_view->type != 0 &&
             (!below_view->fluid ||
              (current.value().fluid && below_view->type != current.value().type) ||
              below_view->amount == maximum_fluid_amount));
        if (!supported) {
            if (current.value().fluid) {
                proposals.push_back({address, *previous, next});
            }
            continue;
        }

        struct LateralCandidate {
            std::uint8_t amount = 0;
            std::uint16_t type = 0;
            ChunkLocalCoord feeder{};
            FluidFlowDirection flow = FluidFlowDirection::none;
        };
        std::optional<LateralCandidate> best;
        for (const auto direction : lateral_directions) {
            const auto neighbor = neighbor_address(address, direction.dx, 0, direction.dz);
            if (!neighbor || find_chunk(snapshot, neighbor->chunk) == nullptr) {
                continue;
            }
            auto neighbor_view = cell_view(snapshot, blocks, *neighbor);
            if (!neighbor_view) {
                return core::Result<FluidStepResult>::failure(neighbor_view.error().code,
                                                              neighbor_view.error().message);
            }
            if (!neighbor_view.value().fluid || neighbor_view.value().falling ||
                (current.value().fluid &&
                 current.value().type != neighbor_view.value().type)) {
                continue;
            }
            const auto candidate_amount =
                neighbor_view.value().source
                    ? static_cast<std::uint8_t>(maximum_fluid_amount - 1U)
                    : (neighbor_view.value().amount > 1U
                           ? static_cast<std::uint8_t>(neighbor_view.value().amount - 1U)
                           : std::uint8_t{0});
            if (candidate_amount == 0) {
                continue;
            }
            ++result.stats.proposal_count;
            const LateralCandidate candidate{candidate_amount, neighbor_view.value().type,
                                             *neighbor, direction.incoming_flow};
            if (!best.has_value() || candidate.amount > best->amount ||
                (candidate.amount == best->amount && candidate.type < best->type) ||
                (candidate.amount == best->amount && candidate.type == best->type &&
                 candidate.feeder < best->feeder)) {
                best = candidate;
            }
        }
        if (best.has_value()) {
            FluidState state{best->amount, false, false, best->flow};
            auto encoded = encode_fluid_state(state);
            if (!encoded) {
                return core::Result<FluidStepResult>::failure(encoded.error().code,
                                                              encoded.error().message);
            }
            next = {best->type, previous->light, encoded.value(), 0};
            ++result.stats.accepted_transfer_count;
            result.stats.transferred_unit_count += best->amount;
        }
        if (next != *previous) {
            proposals.push_back({address, *previous, next});
        }
    }
    result.stats.proposal_count += proposals.size();

    std::ranges::sort(proposals, {}, &StateProposal::address);

    std::set<ChunkLocalCoord> next_active;
    next_active.insert(active_cells.begin() + static_cast<std::ptrdiff_t>(process_count),
                       active_cells.end());
    for (const auto& proposal : proposals) {
        if (proposal.previous == proposal.next) {
            continue;
        }
        const auto* source_chunk = find_chunk(snapshot, proposal.address.chunk);
        result.changes.push_back(
            {proposal.address, source_chunk->identity, source_chunk->content_revision,
             proposal.previous, proposal.next});
        activate_resident_neighbors(snapshot, proposal.address, next_active);
    }
    result.next_active.assign(next_active.begin(), next_active.end());
    result.stats.changed_cell_count = result.changes.size();
    result.stats.next_active_cell_count = result.next_active.size();
    return core::Result<FluidStepResult>::success(std::move(result));
}

core::Result<FluidApplyReport> apply_fluid_step(ChunkDatabase& chunks,
                                                dirty::DirtyRegionTracker& dirty_regions,
                                                const FluidStepResult& result) {
    ChunkLocalCoord previous_address{};
    bool has_previous = false;
    for (const auto& change : result.changes) {
        if (!is_valid_local_coord(change.address.local) ||
            change.chunk_identity.coordinate != change.address.chunk ||
            (has_previous && !(previous_address < change.address))) {
            return core::Result<FluidApplyReport>::failure(
                "fluid_simulation.invalid_change_set",
                "fluid changes must be valid, strictly sorted, and match their chunk identity");
        }
        const auto* chunk = chunks.find(change.address.chunk);
        if (chunk == nullptr || chunk->identity() != change.chunk_identity ||
            chunk->content_revision() != change.source_content_revision) {
            return core::Result<FluidApplyReport>::failure(
                "fluid_simulation.stale_result",
                "fluid change no longer matches the resident chunk identity or revision");
        }
        auto current = chunk->get(change.address.local);
        if (!current || current.value() != change.previous) {
            return core::Result<FluidApplyReport>::failure(
                "fluid_simulation.stale_result",
                "fluid change previous cell no longer matches resident state");
        }
        previous_address = change.address;
        has_previous = true;
    }

    auto staged_dirty = dirty_regions;
    std::set<ChunkCoord> changed_chunks;
    for (const auto& change : result.changes) {
        changed_chunks.insert(change.address.chunk);
        constexpr std::array<std::array<int, 3>, 6> directions{{
            {{-1, 0, 0}},
            {{1, 0, 0}},
            {{0, -1, 0}},
            {{0, 1, 0}},
            {{0, 0, -1}},
            {{0, 0, 1}},
        }};
        for (const auto direction : directions) {
            const auto neighbor = neighbor_address(
                change.address, direction[0], direction[1], direction[2]);
            if (neighbor && neighbor->chunk != change.address.chunk &&
                chunks.contains(neighbor->chunk)) {
                changed_chunks.insert(neighbor->chunk);
            }
        }
    }
    for (const auto coordinate : changed_chunks) {
        auto status = mark_chunk_rebuild(staged_dirty, coordinate);
        if (!status) {
            return core::Result<FluidApplyReport>::failure(status.error().code,
                                                           status.error().message);
        }
    }
    for (const auto& change : result.changes) {
        auto status = chunks.set(change.address.chunk, change.address.local, change.next);
        if (!status) {
            return core::Result<FluidApplyReport>::failure(status.error().code,
                                                           status.error().message);
        }
    }
    dirty_regions = std::move(staged_dirty);
    FluidApplyReport report;
    report.changed_cell_count = result.changes.size();
    report.changed_chunks.assign(changed_chunks.begin(), changed_chunks.end());
    return core::Result<FluidApplyReport>::success(std::move(report));
}

std::vector<ChunkLocalCoord>
fluid_cells_and_neighbors(const FluidSimulationSnapshot& snapshot,
                          const FluidBlockTable& blocks) {
    std::set<ChunkLocalCoord> active;
    for (const auto& chunk : snapshot.chunks) {
        for (std::uint16_t z = 0; z < VoxelChunk::edge_length; ++z) {
            for (std::uint16_t y = 0; y < VoxelChunk::edge_length; ++y) {
                for (std::uint16_t x = 0; x < VoxelChunk::edge_length; ++x) {
                    const VoxelCoord local{x, y, z};
                    const auto& cell = chunk.cells[cell_index(local)];
                    if (blocks.block(cell.type).fluid) {
                        activate_resident_neighbors(snapshot,
                                                    {chunk.identity.coordinate, local}, active);
                    }
                }
            }
        }
    }
    return {active.begin(), active.end()};
}

} // namespace heartstead::world
