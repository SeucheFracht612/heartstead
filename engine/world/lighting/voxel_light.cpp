#include "engine/world/lighting/voxel_light.hpp"

#include "engine/profiling/profiler.hpp"
#include "engine/world/chunks/chunk_database.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <utility>

namespace heartstead::world {

namespace {

constexpr auto edge = static_cast<std::size_t>(VoxelChunk::edge_length);

[[nodiscard]] constexpr std::size_t cell_index(VoxelCoord coordinate) noexcept {
    return static_cast<std::size_t>(coordinate.z) * edge * edge +
           static_cast<std::size_t>(coordinate.y) * edge + static_cast<std::size_t>(coordinate.x);
}

struct ScratchChunk {
    const ChunkLightSnapshot* source = nullptr;
    std::vector<std::uint8_t> sunlight;
    std::vector<std::uint8_t> block_light;
};

using ScratchChunks = std::map<ChunkCoord, ScratchChunk>;

struct LightQueueEntry {
    std::uint8_t light = 0;
    ChunkLocalCoord address{};
};

struct LightQueueLess {
    [[nodiscard]] bool operator()(const LightQueueEntry& left,
                                  const LightQueueEntry& right) const noexcept {
        if (left.light != right.light) {
            return left.light < right.light;
        }
        return left.address > right.address;
    }
};

using LightQueue =
    std::priority_queue<LightQueueEntry, std::vector<LightQueueEntry>, LightQueueLess>;

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
    auto chunk = address.chunk;
    auto local = address.local;
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
    if (!offset_axis(chunk.x, local.x, dx) || !offset_axis(chunk.y, local.y, dy) ||
        !offset_axis(chunk.z, local.z, dz)) {
        return std::nullopt;
    }
    return ChunkLocalCoord{chunk, local};
}

[[nodiscard]] const VoxelCell* find_cell(const ScratchChunks& chunks,
                                         ChunkLocalCoord address) noexcept {
    const auto found = chunks.find(address.chunk);
    if (found == chunks.end()) {
        return nullptr;
    }
    return &found->second.source->cells[cell_index(address.local)];
}

[[nodiscard]] std::uint8_t* find_light(ScratchChunks& chunks, ChunkLocalCoord address,
                                       bool sunlight) noexcept {
    const auto found = chunks.find(address.chunk);
    if (found == chunks.end()) {
        return nullptr;
    }
    auto& values = sunlight ? found->second.sunlight : found->second.block_light;
    return &values[cell_index(address.local)];
}

[[nodiscard]] std::uint8_t attenuated(std::uint8_t light,
                                      VoxelLightBlockInfo destination) noexcept {
    if (destination.absorption == maximum_voxel_light) {
        return 0;
    }
    const auto attenuation = std::max(destination.absorption, minimum_voxel_light_attenuation);
    return light > attenuation ? static_cast<std::uint8_t>(light - attenuation) : 0;
}

void propagate_light(ScratchChunks& chunks, const VoxelLightBlockTable& blocks, LightQueue& queue,
                     bool sunlight, std::size_t& visits) {
    constexpr std::array<std::array<int, 3>, 6> directions{{
        {{-1, 0, 0}},
        {{1, 0, 0}},
        {{0, -1, 0}},
        {{0, 1, 0}},
        {{0, 0, -1}},
        {{0, 0, 1}},
    }};
    while (!queue.empty()) {
        const auto entry = queue.top();
        queue.pop();
        auto* current = find_light(chunks, entry.address, sunlight);
        if (current == nullptr || *current != entry.light) {
            continue;
        }
        ++visits;
        for (const auto direction : directions) {
            const auto neighbor =
                neighbor_address(entry.address, direction[0], direction[1], direction[2]);
            if (!neighbor) {
                continue;
            }
            const auto* cell = find_cell(chunks, *neighbor);
            auto* neighbor_light = find_light(chunks, *neighbor, sunlight);
            if (cell == nullptr || neighbor_light == nullptr) {
                continue;
            }
            const auto propagated = attenuated(entry.light, blocks.block(cell->type));
            if (propagated <= *neighbor_light) {
                continue;
            }
            *neighbor_light = propagated;
            queue.push({propagated, *neighbor});
        }
    }
}

[[nodiscard]] core::Status validate_patch_set(const ChunkDatabase& chunks,
                                              const VoxelLightSolveResult& result) {
    std::set<ChunkCoord> coordinates;
    for (const auto& patch : result.patches) {
        auto status = patch.validate();
        if (!status) {
            return status;
        }
        if (!coordinates.insert(patch.identity.coordinate).second) {
            return core::Status::failure("voxel_light.duplicate_patch",
                                         "voxel light result contains a duplicate chunk patch");
        }
        const auto* chunk = chunks.find(patch.identity.coordinate);
        if (chunk == nullptr || chunk->identity() != patch.identity ||
            !chunk->stage_ticket_is_current(patch.stage_ticket) ||
            chunk->content_revision() != patch.source_content_revision) {
            return core::Status::failure(
                "voxel_light.stale_result",
                "voxel light result no longer matches the resident chunk generation or revision");
        }
    }
    const auto resident = chunks.identities();
    if (resident.size() != result.patches.size()) {
        return core::Status::failure(
            "voxel_light.stale_result",
            "voxel light result does not cover the current resident chunk topology");
    }
    for (std::size_t index = 0; index < resident.size(); ++index) {
        if (resident[index] != result.patches[index].identity) {
            return core::Status::failure(
                "voxel_light.stale_result",
                "voxel light result does not match the current resident chunk topology");
        }
    }
    return core::Status::ok();
}

} // namespace

VoxelLightBlockInfo VoxelLightBlockTable::block(std::uint16_t type) const noexcept {
    if (type == VoxelDefinition::air_type) {
        return {0, 0};
    }
    if (type >= blocks.size()) {
        return {};
    }
    return blocks[type];
}

core::Status VoxelLightBlockTable::validate() const {
    if (revision == 0 || blocks.empty() || blocks.front().emission != 0 ||
        blocks.front().absorption != 0) {
        return core::Status::failure(
            "voxel_light.invalid_block_table",
            "voxel light block table must contain transparent, non-emitting air at type zero");
    }
    return core::Status::ok();
}

VoxelLightBlockTable build_voxel_light_block_table(const VoxelPalette& palette) {
    VoxelLightBlockTable result;
    result.revision = palette.render_revision();
    result.blocks.resize(1, VoxelLightBlockInfo{0, 0});
    for (const auto* definition : palette.definitions()) {
        if (definition->type >= result.blocks.size()) {
            result.blocks.resize(static_cast<std::size_t>(definition->type) + 1);
        }
        result.blocks[definition->type] = {
            definition->light_emission,
            definition->light_absorption,
        };
    }
    return result;
}

core::Status ChunkLightSnapshot::validate() const {
    if (!identity.is_valid() || !stage_ticket.is_valid() || stage_ticket.identity != identity ||
        stage_ticket.stage != ChunkStage::lighting || content_revision == 0 ||
        cells.size() != VoxelChunk::total_cells) {
        return core::Status::failure(
            "voxel_light.invalid_chunk_snapshot",
            "voxel light chunk snapshot requires a valid identity, revision, and complete cells");
    }
    return core::Status::ok();
}

core::Status VoxelLightSnapshot::validate() const {
    ChunkCoord previous{};
    bool has_previous = false;
    for (const auto& chunk : chunks) {
        auto status = chunk.validate();
        if (!status) {
            return status;
        }
        if (has_previous && !(previous < chunk.identity.coordinate)) {
            return core::Status::failure(
                "voxel_light.unsorted_snapshot",
                "voxel light chunks must be strictly sorted by coordinate");
        }
        previous = chunk.identity.coordinate;
        has_previous = true;
    }
    BlockCoord previous_source{};
    bool has_previous_source = false;
    for (const auto& source : sources) {
        if (source.light == 0 || (has_previous_source && !(previous_source < source.position))) {
            return core::Status::failure(
                "voxel_light.invalid_sources",
                "voxel light sources must be nonzero and strictly sorted by block position");
        }
        previous_source = source.position;
        has_previous_source = true;
    }
    return core::Status::ok();
}

VoxelLightSnapshot build_voxel_light_snapshot(const ChunkDatabase& chunks) {
    VoxelLightSnapshot result;
    const auto identities = chunks.identities();
    result.chunks.reserve(identities.size());
    for (const auto identity : identities) {
        const auto* chunk = chunks.find(identity.coordinate);
        if (chunk == nullptr || chunk->identity() != identity) {
            continue;
        }
        result.chunks.push_back(
            {identity, chunk->stage_ticket(ChunkStage::lighting), chunk->content_revision(),
             std::vector<VoxelCell>(chunk->cells().begin(), chunk->cells().end())});
    }
    return result;
}

core::Status ChunkLightPatch::validate() const {
    if (!identity.is_valid() || !stage_ticket.is_valid() || stage_ticket.identity != identity ||
        stage_ticket.stage != ChunkStage::lighting || source_content_revision == 0 ||
        lights.size() != VoxelChunk::total_cells) {
        return core::Status::failure(
            "voxel_light.invalid_patch",
            "voxel light patch requires a valid identity, source revision, and complete field");
    }
    return core::Status::ok();
}

core::Result<VoxelLightSolveResult> solve_voxel_light(const VoxelLightSnapshot& snapshot,
                                                      const VoxelLightBlockTable& blocks) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("voxel_light.solve");
    auto status = snapshot.validate();
    if (!status) {
        return core::Result<VoxelLightSolveResult>::failure(status.error().code,
                                                            status.error().message);
    }
    status = blocks.validate();
    if (!status) {
        return core::Result<VoxelLightSolveResult>::failure(status.error().code,
                                                            status.error().message);
    }

    ScratchChunks scratch;
    VoxelLightSolveResult result;
    result.stats.chunk_count = snapshot.chunks.size();
    result.stats.cell_count = snapshot.chunks.size() * VoxelChunk::total_cells;
    for (const auto& chunk : snapshot.chunks) {
        scratch.emplace(chunk.identity.coordinate,
                        ScratchChunk{&chunk, std::vector<std::uint8_t>(VoxelChunk::total_cells),
                                     std::vector<std::uint8_t>(VoxelChunk::total_cells)});
    }

    LightQueue sunlight_queue;
    using ColumnCoord = std::pair<std::int64_t, std::int64_t>;
    std::map<ColumnCoord, std::vector<ChunkCoord>> columns;
    for (const auto& [coordinate, _] : scratch) {
        columns[{coordinate.x, coordinate.z}].push_back(coordinate);
    }
    for (auto& [_, column_chunks] : columns) {
        std::ranges::sort(column_chunks,
                          [](ChunkCoord left, ChunkCoord right) { return left.y > right.y; });
        std::optional<std::int64_t> previous_chunk_y;
        std::array<std::uint8_t, edge * edge> direct_sunlight{};
        for (const auto coordinate : column_chunks) {
            const bool begins_segment =
                !previous_chunk_y.has_value() ||
                (*previous_chunk_y != std::numeric_limits<std::int64_t>::min() &&
                 coordinate.y != *previous_chunk_y - 1);
            if (begins_segment) {
                direct_sunlight.fill(maximum_voxel_light);
            }
            previous_chunk_y = coordinate.y;
            auto& chunk = scratch.at(coordinate);
            for (std::size_t y = edge; y-- > 0;) {
                for (std::size_t z = 0; z < edge; ++z) {
                    for (std::size_t x = 0; x < edge; ++x) {
                        const auto column_index = z * edge + x;
                        auto direct = direct_sunlight[column_index];
                        if (direct == 0) {
                            continue;
                        }
                        const VoxelCoord local{static_cast<std::uint16_t>(x),
                                               static_cast<std::uint16_t>(y),
                                               static_cast<std::uint16_t>(z)};
                        const auto block =
                            blocks.block(chunk.source->cells[cell_index(local)].type);
                        if (block.absorption >= direct) {
                            direct_sunlight[column_index] = 0;
                            continue;
                        }
                        direct = static_cast<std::uint8_t>(direct - block.absorption);
                        direct_sunlight[column_index] = direct;
                        chunk.sunlight[cell_index(local)] = direct;
                        sunlight_queue.push({direct, {coordinate, local}});
                        ++result.stats.sunlight_seed_count;
                    }
                }
            }
        }
    }

    LightQueue block_light_queue;
    for (auto& [coordinate, chunk] : scratch) {
        for (std::uint16_t z = 0; z < VoxelChunk::edge_length; ++z) {
            for (std::uint16_t y = 0; y < VoxelChunk::edge_length; ++y) {
                for (std::uint16_t x = 0; x < VoxelChunk::edge_length; ++x) {
                    const VoxelCoord local{x, y, z};
                    const auto emission =
                        blocks.block(chunk.source->cells[cell_index(local)].type).emission;
                    if (emission == 0) {
                        continue;
                    }
                    chunk.block_light[cell_index(local)] = emission;
                    block_light_queue.push({emission, {coordinate, local}});
                    ++result.stats.block_light_seed_count;
                }
            }
        }
    }
    for (const auto& source : snapshot.sources) {
        const auto address = block_to_chunk_local(source.position);
        auto* light = find_light(scratch, address, false);
        if (light == nullptr || source.light <= *light) {
            continue;
        }
        *light = source.light;
        block_light_queue.push({source.light, address});
        ++result.stats.block_light_seed_count;
    }

    propagate_light(scratch, blocks, sunlight_queue, true, result.stats.sunlight_queue_visits);
    propagate_light(scratch, blocks, block_light_queue, false,
                    result.stats.block_light_queue_visits);

    result.patches.reserve(scratch.size());
    for (auto& [_, chunk] : scratch) {
        ChunkLightPatch patch;
        patch.identity = chunk.source->identity;
        patch.stage_ticket = chunk.source->stage_ticket;
        patch.source_content_revision = chunk.source->content_revision;
        patch.lights.resize(VoxelChunk::total_cells);
        for (std::size_t index = 0; index < patch.lights.size(); ++index) {
            patch.lights[index] = std::max(chunk.sunlight[index], chunk.block_light[index]);
        }
        result.patches.push_back(std::move(patch));
    }
    return core::Result<VoxelLightSolveResult>::success(std::move(result));
}

core::Result<VoxelLightApplyReport> apply_voxel_light(ChunkDatabase& chunks,
                                                      dirty::DirtyRegionTracker& dirty_regions,
                                                      const VoxelLightSolveResult& result) {
    auto status = validate_patch_set(chunks, result);
    if (!status) {
        return core::Result<VoxelLightApplyReport>::failure(status.error().code,
                                                            status.error().message);
    }

    auto staged_dirty = dirty_regions;
    VoxelLightApplyReport report;
    report.patch_count = result.patches.size();
    for (const auto& patch : result.patches) {
        auto* chunk = chunks.find(patch.identity.coordinate);
        auto applied = chunk->apply_derived_light(patch.lights);
        if (!applied) {
            return core::Result<VoxelLightApplyReport>::failure(applied.error().code,
                                                                applied.error().message);
        }
        report.changed_cell_count += applied.value();
        chunk->clear_dirty(ChunkDirtyFlag::lighting);
        if (applied.value() == 0) {
            continue;
        }
        ++report.changed_chunk_count;
        report.changed_chunks.push_back(patch.identity.coordinate);
        status = staged_dirty.mark_single(
            dirty::DirtyRegionKind::chunk_mesh,
            {patch.identity.coordinate.x, patch.identity.coordinate.y, patch.identity.coordinate.z},
            "voxel relight");
        if (!status) {
            return core::Result<VoxelLightApplyReport>::failure(status.error().code,
                                                                status.error().message);
        }
    }
    dirty_regions = std::move(staged_dirty);
    return core::Result<VoxelLightApplyReport>::success(report);
}

} // namespace heartstead::world
