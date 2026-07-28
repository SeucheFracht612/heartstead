#include "engine/world/worldgen/terrain_generator.hpp"

#include "engine/core/hash.hpp"
#include "engine/world/fluids/fluid_state.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace heartstead::world {

namespace {

[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

[[nodiscard]] std::uint64_t stable_string_hash(std::string_view value) noexcept {
    return core::stable_hash64(value);
}

[[nodiscard]] std::uint64_t coordinate_bits(std::int64_t value) noexcept {
    return static_cast<std::uint64_t>(value) ^ 0x8000000000000000ULL;
}

[[nodiscard]] constexpr bool uses_legacy_coordinate_hash(std::int64_t value) noexcept {
    constexpr auto legacy_min = -(std::int64_t{1} << 62);
    constexpr auto legacy_max = (std::int64_t{1} << 62) - 1;
    return value >= legacy_min && value <= legacy_max;
}

[[nodiscard]] std::uint64_t combine_hash(std::uint64_t hash, std::uint64_t value,
                                         std::uint64_t domain) noexcept {
    return mix(hash ^ mix(value ^ domain));
}

[[nodiscard]] std::int64_t saturated_add(std::int64_t value, std::int64_t offset) noexcept {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if (offset > 0 && value > max - offset) {
        return max;
    }
    if (offset < 0 && value < min - offset) {
        return min;
    }
    return value + offset;
}

[[nodiscard]] core::Result<VoxelCell> select_region_terrain_cell(const RegionDescriptor& region,
                                                                 const VoxelPalette& palette) {
    for (const auto& rule : region.resource_rules) {
        auto cell = palette.cell_for(rule.prototype_id);
        if (cell) {
            return cell;
        }
    }

    return core::Result<VoxelCell>::failure(
        "terrain_generator.missing_region_voxel",
        "region has no resource rule that resolves to a voxel palette entry: " + region.id);
}

[[nodiscard]] std::uint64_t cell_noise(std::uint64_t seed, std::int64_t x, std::int64_t y,
                                       std::int64_t z, std::string_view salt) noexcept {
    if (uses_legacy_coordinate_hash(x) && uses_legacy_coordinate_hash(y) &&
        uses_legacy_coordinate_hash(z)) {
        // Preserve the established cave/resource/feature distribution throughout the practical
        // coordinate domain.
        return mix(seed ^ mix(coordinate_bits(x)) ^ mix(coordinate_bits(y) << 1U) ^
                   mix(coordinate_bits(z) << 2U) ^ mix(stable_string_hash(salt)));
    }
    // The legacy shifts alias coordinates whose signed representations differ only in the sign
    // bit. Domain-separate all axes in the extreme half of the coordinate range.
    auto hash = mix(seed ^ stable_string_hash(salt));
    hash = combine_hash(hash, coordinate_bits(x), 0x243f6a8885a308d3ULL);
    hash = combine_hash(hash, coordinate_bits(y), 0x13198a2e03707344ULL);
    return combine_hash(hash, coordinate_bits(z), 0xa4093822299f31d0ULL);
}

[[nodiscard]] bool placement_is_external(std::string_view placement) noexcept {
    return placement.contains("object") || placement.contains("entity") ||
           placement.contains("resource_site") || placement.contains("large_static");
}

[[nodiscard]] GeneratedWorldFeatureKind feature_kind(std::string_view placement) noexcept {
    if (placement.contains("block_entity"))
        return GeneratedWorldFeatureKind::block_entity;
    if (placement.contains("large_static"))
        return GeneratedWorldFeatureKind::large_static_object;
    if (placement.contains("resource_site"))
        return GeneratedWorldFeatureKind::resource_site;
    if (placement.contains("object"))
        return GeneratedWorldFeatureKind::surface_object;
    return GeneratedWorldFeatureKind::rich_block;
}

[[nodiscard]] bool rule_depth_matches(std::string_view placement, std::uint64_t depth) noexcept {
    if (placement.contains("surface"))
        return depth <= 1;
    if (placement.contains("deep"))
        return depth >= 16;
    return true;
}

[[nodiscard]] bool rule_selected(const RegionResourceRule& rule,
                                 const TerrainGenerationConfig& config, std::int64_t x,
                                 std::int64_t y, std::int64_t z) noexcept {
    const auto abundance = std::clamp(rule.abundance, 0.0, 1.0);
    const auto threshold = static_cast<std::uint64_t>(
        abundance *
        static_cast<double>(std::min<std::uint16_t>(config.feature_frequency_per_mille, 1000)));
    return cell_noise(config.world_seed, x, y, z, rule.prototype_id.value()) % 1000U < threshold;
}

} // namespace

core::Result<VoxelChunk> DeterministicTerrainGenerator::generate_chunk(
    ChunkCoord coord, const TerrainGenerationConfig& config, const RegionGraph& regions,
    const VoxelPalette& palette) {
    if (config.region_id.empty()) {
        return core::Result<VoxelChunk>::failure("terrain_generator.missing_region_id",
                                                 "terrain generation requires a region id");
    }
    if (palette.empty()) {
        return core::Result<VoxelChunk>::failure("terrain_generator.empty_palette",
                                                 "terrain generation requires a voxel palette");
    }
    if (config.cave_frequency_per_mille > 1000 || config.feature_frequency_per_mille > 1000) {
        return core::Result<VoxelChunk>::failure(
            "terrain_generator.invalid_frequency",
            "worldgen cave and feature frequencies must be 0..1000 per mille");
    }
    std::optional<VoxelCell> ocean_cell;
    if (config.sea_level.has_value()) {
        if (!config.ocean_voxel_id.is_valid()) {
            return core::Result<VoxelChunk>::failure(
                "terrain_generator.missing_ocean_voxel",
                "terrain generation with a sea level requires an ocean voxel prototype");
        }
        auto resolved = palette.cell_for(config.ocean_voxel_id);
        if (!resolved) {
            return core::Result<VoxelChunk>::failure(resolved.error().code,
                                                     resolved.error().message);
        }
        const auto* definition = palette.find_by_type(resolved.value().type);
        if (definition == nullptr ||
            definition->logical_occupancy != BlockLogicalOccupancy::fluid) {
            return core::Result<VoxelChunk>::failure(
                "terrain_generator.invalid_ocean_voxel",
                "terrain generation ocean voxel must have fluid logical occupancy");
        }
        resolved.value().state_bits = full_fluid_source_state_bits();
        ocean_cell = resolved.value();
    }

    const auto* region = regions.find(config.region_id);
    if (region == nullptr) {
        return core::Result<VoxelChunk>::failure("terrain_generator.missing_region",
                                                 "terrain generation region does not exist: " +
                                                     config.region_id);
    }

    auto terrain_cell = select_region_terrain_cell(*region, palette);
    if (!terrain_cell) {
        return core::Result<VoxelChunk>::failure(terrain_cell.error().code,
                                                 terrain_cell.error().message);
    }

    auto chunk_origin = chunk_local_to_block(coord, {0, 0, 0});
    auto chunk_max =
        chunk_local_to_block(coord, {VoxelChunk::edge_length - 1, VoxelChunk::edge_length - 1,
                                     VoxelChunk::edge_length - 1});
    if (!chunk_origin || !chunk_max) {
        return core::Result<VoxelChunk>::failure(
            "terrain_generator.chunk_coord_overflow",
            "chunk coordinate cannot be represented by signed 64-bit block coordinates");
    }

    std::vector<VoxelCell> cells;
    cells.reserve(VoxelChunk::total_cells);
    for (std::uint16_t z = 0; z < VoxelChunk::edge_length; ++z) {
        const auto global_z = chunk_origin.value().z + static_cast<std::int64_t>(z);
        for (std::uint16_t y = 0; y < VoxelChunk::edge_length; ++y) {
            const auto global_y = chunk_origin.value().y + static_cast<std::int64_t>(y);
            for (std::uint16_t x = 0; x < VoxelChunk::edge_length; ++x) {
                const auto global_x = chunk_origin.value().x + static_cast<std::int64_t>(x);
                const auto surface_y = surface_height_at(config, global_x, global_z);
                if (global_y > surface_y) {
                    if (ocean_cell.has_value() && global_y <= *config.sea_level) {
                        cells.push_back(*ocean_cell);
                    } else {
                        cells.push_back(VoxelCell{VoxelPalette::air_type, 255});
                    }
                    continue;
                }
                // The mathematical depth can span the entire signed coordinate range. Unsigned
                // subtraction is exact here because the above branch established surface_y >=
                // global_y, and avoids signed overflow at the world-coordinate extremes.
                const auto depth =
                    static_cast<std::uint64_t>(surface_y) - static_cast<std::uint64_t>(global_y);
                if (config.enable_caves && depth >= config.cave_min_depth &&
                    cell_noise(config.world_seed, global_x, global_y, global_z, "cave") % 1000U <
                        config.cave_frequency_per_mille) {
                    cells.push_back(VoxelCell{VoxelPalette::air_type, 0});
                    continue;
                }
                auto selected = terrain_cell.value();
                for (const auto& rule : region->resource_rules) {
                    if (placement_is_external(rule.placement) ||
                        !rule_depth_matches(rule.placement, depth) ||
                        !rule_selected(rule, config, global_x, global_y, global_z)) {
                        continue;
                    }
                    if (auto feature_cell = palette.cell_for(rule.prototype_id); feature_cell) {
                        selected = feature_cell.value();
                    }
                }
                cells.push_back(selected);
            }
        }
    }

    VoxelChunk chunk(coord);
    auto status = chunk.load_generated_cells(std::move(cells));
    if (!status) {
        return core::Result<VoxelChunk>::failure(status.error().code, status.error().message);
    }
    return core::Result<VoxelChunk>::success(std::move(chunk));
}

core::Result<GeneratedChunk> DeterministicTerrainGenerator::generate_chunk_with_features(
    ChunkCoord coord, const TerrainGenerationConfig& config, const RegionGraph& regions,
    const VoxelPalette& palette) {
    auto chunk = generate_chunk(coord, config, regions, palette);
    if (!chunk) {
        return core::Result<GeneratedChunk>::failure(chunk.error().code, chunk.error().message);
    }
    const auto* region = regions.find(config.region_id);
    auto origin = chunk_local_to_block(coord, {0, 0, 0});
    if (region == nullptr || !origin) {
        return core::Result<GeneratedChunk>::failure(
            "terrain_generator.feature_context_missing",
            "worldgen feature pass requires a valid region and chunk origin");
    }
    GeneratedChunk result{std::move(chunk).value(), {}};
    for (const auto& rule : region->resource_rules) {
        if (!placement_is_external(rule.placement))
            continue;
        for (std::uint16_t z = 0; z < VoxelChunk::edge_length; ++z) {
            for (std::uint16_t x = 0; x < VoxelChunk::edge_length; ++x) {
                const auto global_x = origin.value().x + x;
                const auto global_z = origin.value().z + z;
                const BlockCoord surface_position{
                    global_x, surface_height_at(config, global_x, global_z), global_z};
                auto feature_position =
                    checked_block_coord_offset(surface_position, BlockCoord{0, 1, 0});
                if (!feature_position || chunk_coord_for_block(feature_position.value()) != coord ||
                    !rule_selected(rule, config, feature_position.value().x,
                                   feature_position.value().y, feature_position.value().z)) {
                    continue;
                }
                result.features.push_back({
                    rule.prototype_id,
                    feature_kind(rule.placement),
                    feature_position.value(),
                    cell_noise(config.world_seed, feature_position.value().x,
                               feature_position.value().y, feature_position.value().z,
                               rule.placement),
                    rule.placement,
                });
            }
        }
    }
    return core::Result<GeneratedChunk>::success(std::move(result));
}

std::int64_t DeterministicTerrainGenerator::surface_height_at(const TerrainGenerationConfig& config,
                                                              std::int64_t global_x,
                                                              std::int64_t global_z) noexcept {
    if (config.surface_variation == 0) {
        return config.base_surface_y;
    }

    std::uint64_t hash = 0;
    if (uses_legacy_coordinate_hash(global_x) && uses_legacy_coordinate_hash(global_z)) {
        // This is the original worldgen formula. Preserve it throughout the practical coordinate
        // domain so existing terrain and saved edit baselines remain stable.
        hash =
            mix(mix(config.world_seed) ^ mix(coordinate_bits(global_x)) ^
                mix(coordinate_bits(global_z) << 1U) ^ mix(stable_string_hash(config.region_id)));
    } else {
        // Shifting coordinate_bits(z) aliases z with the value obtained by toggling its sign bit.
        // Only the extreme half of the signed range uses the corrected domain-separated hash; its
        // counterpart remains on the legacy path, so the pair can no longer alias.
        hash = mix(config.world_seed ^ stable_string_hash(config.region_id));
        hash = combine_hash(hash, coordinate_bits(global_x), 0x243f6a8885a308d3ULL);
        hash = combine_hash(hash, coordinate_bits(global_z), 0xa4093822299f31d0ULL);
    }
    const auto range = static_cast<std::uint64_t>(config.surface_variation) * 2ULL + 1ULL;
    const auto offset = static_cast<std::int64_t>(hash % range) -
                        static_cast<std::int64_t>(config.surface_variation);
    return saturated_add(config.base_surface_y, offset);
}

} // namespace heartstead::world
