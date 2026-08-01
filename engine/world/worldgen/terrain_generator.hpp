#pragma once

#include "engine/core/result.hpp"
#include "engine/world/regions/region_graph.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace heartstead::world {

// Increment when deterministic terrain output changes for identical generation inputs. Saves
// record this independently from the game and save-schema versions so compatibility diagnostics
// can explain world-generation differences precisely.
inline constexpr std::uint32_t deterministic_terrain_generator_version = 1;

struct TerrainGenerationConfig {
    std::uint64_t world_seed = 0;
    std::string region_id;
    // Authoritative vertical identity remains integral at far-world coordinates.
    std::int64_t base_surface_y = 0;
    std::uint16_t surface_variation = 0;
    bool enable_caves = false;
    std::uint16_t cave_frequency_per_mille = 0;
    std::uint16_t cave_min_depth = 8;
    std::uint16_t feature_frequency_per_mille = 1000;
    std::optional<std::int64_t> sea_level;
    core::PrototypeId ocean_voxel_id;
};

enum class GeneratedWorldFeatureKind {
    rich_block,
    block_entity,
    surface_object,
    large_static_object,
    resource_site,
};

struct GeneratedWorldFeature {
    core::PrototypeId prototype_id;
    GeneratedWorldFeatureKind kind = GeneratedWorldFeatureKind::rich_block;
    BlockCoord position;
    std::uint64_t deterministic_seed = 0;
    std::string placement;
};

struct GeneratedChunk {
    VoxelChunk chunk;
    std::vector<GeneratedWorldFeature> features;
};

class DeterministicTerrainGenerator {
  public:
    [[nodiscard]] static core::Result<VoxelChunk>
    generate_chunk(ChunkCoord coord, const TerrainGenerationConfig& config,
                   const RegionGraph& regions, const VoxelPalette& palette);
    [[nodiscard]] static core::Result<GeneratedChunk>
    generate_chunk_with_features(ChunkCoord coord, const TerrainGenerationConfig& config,
                                 const RegionGraph& regions, const VoxelPalette& palette);

    [[nodiscard]] static std::int64_t surface_height_at(const TerrainGenerationConfig& config,
                                                        std::int64_t global_x,
                                                        std::int64_t global_z) noexcept;
};

} // namespace heartstead::world
