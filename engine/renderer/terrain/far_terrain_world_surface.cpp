#include "engine/renderer/terrain/far_terrain_world_surface.hpp"

#include "engine/core/hash.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <span>

namespace heartstead::renderer {

FarTerrainSurfaceSample sample_far_terrain_world_surface(const world::WorldState& world,
                                                         double world_x, double world_z,
                                                         FarTerrainDomain domain) noexcept {
    if (domain != FarTerrainDomain::surface || !std::isfinite(world_x) || !std::isfinite(world_z)) {
        return {0.0, 0, false};
    }
    const auto floored_x = std::floor(world_x);
    const auto floored_z = std::floor(world_z);
    constexpr auto minimum = static_cast<double>(std::numeric_limits<std::int64_t>::min());
    constexpr auto maximum = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    if (floored_x < minimum || floored_x >= maximum || floored_z < minimum ||
        floored_z >= maximum) {
        return {0.0, 0, false};
    }
    const world::BlockCoord column{static_cast<std::int64_t>(floored_x), 0,
                                   static_cast<std::int64_t>(floored_z)};
    const auto address = world::block_to_chunk_local(column);
    std::optional<std::int64_t> highest_block;
    std::uint16_t material = 0;
    for (const auto* chunk : world.chunks().records()) {
        if (chunk->coord().x != address.chunk.x || chunk->coord().z != address.chunk.z) {
            continue;
        }
        for (std::int32_t y = world::VoxelChunk::edge_length - 1; y >= 0; --y) {
            const world::VoxelCoord local{address.local.x, static_cast<std::uint16_t>(y),
                                          address.local.z};
            const auto cell = chunk->get(local);
            if (!cell || cell.value().is_air()) {
                continue;
            }
            const auto block = world::chunk_local_to_block(chunk->coord(), local);
            if (block && (!highest_block.has_value() || block.value().y > *highest_block)) {
                highest_block = block.value().y;
                material = cell.value().type;
            }
            break;
        }
    }
    if (!highest_block.has_value() || *highest_block == std::numeric_limits<std::int64_t>::max()) {
        return {0.0, 0, false};
    }
    return {static_cast<double>(*highest_block + 1), material, true};
}

std::uint64_t far_terrain_world_surface_revision(const world::WorldState& world) noexcept {
    auto records = world.chunks().records();
    std::ranges::sort(records, [](const world::VoxelChunk* left, const world::VoxelChunk* right) {
        return left->coord() < right->coord();
    });
    core::StableHash64 hash;
    hash.add_u64_le(world.metadata().world_seed);
    for (const auto* chunk : records) {
        hash.add_u64_le(static_cast<std::uint64_t>(chunk->coord().x));
        hash.add_u64_le(static_cast<std::uint64_t>(chunk->coord().y));
        hash.add_u64_le(static_cast<std::uint64_t>(chunk->coord().z));
        hash.add_u64_le(chunk->identity().load_generation);
        hash.add_u64_le(chunk->content_revision());
    }
    return hash.nonzero_value();
}

std::vector<math::Bounds3d> FarTerrainWorldSurfaceCache::synchronize(const world::WorldState& world,
                                                                     std::uint64_t revision) {
    const auto chunks = world.chunks().records();
    std::map<world::ChunkCoord, ChunkSurfaceState> current_states;
    std::map<HorizontalChunkCoord, std::vector<const world::VoxelChunk*>> column_chunks;
    std::set<HorizontalChunkCoord> dirty_columns;
    for (const auto* chunk : chunks) {
        const auto identity = chunk->identity();
        const ChunkSurfaceState state{identity.load_generation, chunk->content_revision()};
        current_states.insert_or_assign(identity.coordinate, state);
        column_chunks[{identity.coordinate.x, identity.coordinate.z}].push_back(chunk);
        const auto previous = chunk_states_.find(identity.coordinate);
        if (previous == chunk_states_.end() || previous->second != state) {
            dirty_columns.insert({identity.coordinate.x, identity.coordinate.z});
        }
    }
    for (const auto& [coord, state] : chunk_states_) {
        static_cast<void>(state);
        if (!current_states.contains(coord)) {
            dirty_columns.insert({coord.x, coord.z});
        }
    }

    constexpr auto edge = world::VoxelChunk::edge_length;
    constexpr auto edge_size = static_cast<std::size_t>(edge);
    std::vector<math::Bounds3d> invalidated_regions;
    invalidated_regions.reserve(dirty_columns.size());
    for (const auto& [chunk_x, chunk_z] : dirty_columns) {
        const world::ChunkCoord horizontal_coord{chunk_x, 0, chunk_z};
        const auto minimum = world::chunk_local_to_block(horizontal_coord, {0, 0, 0});
        const auto maximum =
            world::chunk_local_to_block(horizontal_coord, {static_cast<std::uint16_t>(edge - 1U), 0,
                                                           static_cast<std::uint16_t>(edge - 1U)});
        if (!minimum || !maximum) {
            continue;
        }
        invalidated_regions.push_back(
            {{static_cast<double>(minimum.value().x), 0.0, static_cast<double>(minimum.value().z)},
             {static_cast<double>(maximum.value().x) + 1.0, 0.0,
              static_cast<double>(maximum.value().z) + 1.0}});

        const auto found_chunks = column_chunks.find({chunk_x, chunk_z});
        const auto resident_chunks =
            found_chunks == column_chunks.end()
                ? std::span<const world::VoxelChunk* const>{}
                : std::span<const world::VoxelChunk* const>{found_chunks->second};
        for (std::uint16_t z = 0; z < edge; ++z) {
            for (std::uint16_t x = 0; x < edge; ++x) {
                const auto world_x = minimum.value().x + static_cast<std::int64_t>(x);
                const auto world_z = minimum.value().z + static_cast<std::int64_t>(z);
                const auto key = std::pair{world_x, world_z};
                samples_.erase(key);
                std::optional<std::int64_t> highest_block;
                std::uint16_t material = 0;
                for (const auto* chunk : resident_chunks) {
                    const auto cells = chunk->cells();
                    for (std::int32_t y = static_cast<std::int32_t>(edge) - 1; y >= 0; --y) {
                        const auto index = static_cast<std::size_t>(z) * edge_size * edge_size +
                                           static_cast<std::size_t>(y) * edge_size + x;
                        const auto cell = cells[index];
                        if (cell.is_air()) {
                            continue;
                        }
                        const auto block = world::chunk_local_to_block(
                            chunk->coord(), {x, static_cast<std::uint16_t>(y), z});
                        if (block &&
                            (!highest_block.has_value() || block.value().y > *highest_block)) {
                            highest_block = block.value().y;
                            material = cell.type;
                        }
                        break;
                    }
                }
                if (highest_block.has_value() &&
                    *highest_block != std::numeric_limits<std::int64_t>::max()) {
                    samples_.insert_or_assign(
                        key, FarTerrainSurfaceSample{static_cast<double>(*highest_block + 1),
                                                     material, true});
                }
            }
        }
    }
    chunk_states_ = std::move(current_states);
    revision_ = revision;
    return invalidated_regions;
}

FarTerrainSurfaceSample
FarTerrainWorldSurfaceCache::sample(double world_x, double world_z,
                                    FarTerrainDomain domain) const noexcept {
    if (domain != FarTerrainDomain::surface || !std::isfinite(world_x) || !std::isfinite(world_z)) {
        return {0.0, 0, false};
    }
    const auto floored_x = std::floor(world_x);
    const auto floored_z = std::floor(world_z);
    constexpr auto minimum = static_cast<double>(std::numeric_limits<std::int64_t>::min());
    constexpr auto maximum = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    if (floored_x < minimum || floored_x >= maximum || floored_z < minimum ||
        floored_z >= maximum) {
        return {0.0, 0, false};
    }
    const auto found =
        samples_.find({static_cast<std::int64_t>(floored_x), static_cast<std::int64_t>(floored_z)});
    return found == samples_.end() ? FarTerrainSurfaceSample{0.0, 0, false} : found->second;
}

void FarTerrainWorldSurfaceCache::clear() noexcept {
    samples_.clear();
    chunk_states_.clear();
    revision_ = 0;
}

std::uint64_t FarTerrainWorldSurfaceCache::revision() const noexcept {
    return revision_;
}

std::size_t FarTerrainWorldSurfaceCache::sample_count() const noexcept {
    return samples_.size();
}

} // namespace heartstead::renderer
