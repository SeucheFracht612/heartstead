#include "heartstead/world/world_generation.hpp"

#include "heartstead/core/parallel.hpp"
#include "heartstead/voxel/block_registry.hpp"
#include "heartstead/voxel/greedy_mesher.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace heartstead::world {

constexpr std::uint32_t world_seed = 0x6d2b79f5U;

[[nodiscard]] std::vector<heartstead::Int3> world_coordinates(const WorldArea& area) {
    std::vector<heartstead::Int3> coordinates;
    coordinates.reserve(static_cast<std::size_t>(area.chunks_per_axis * area.chunks_per_axis));
    const auto radius = static_cast<float>(area.chunks_per_axis) * 0.5F;
    const auto radius_squared = radius * radius;
    for (std::int32_t chunk_z = area.first_z(); chunk_z < area.first_z() + area.chunks_per_axis; ++chunk_z) {
        for (std::int32_t chunk_x = area.first_x(); chunk_x < area.first_x() + area.chunks_per_axis; ++chunk_x) {
            const auto dx = static_cast<float>(chunk_x - area.center_chunk.x) + 0.5F;
            const auto dz = static_cast<float>(chunk_z - area.center_chunk.z) + 0.5F;
            if (dx * dx + dz * dz > radius_squared) continue;
            coordinates.push_back({chunk_x, 0, chunk_z});
        }
    }
    return coordinates;
}

[[nodiscard]] std::uint32_t coordinate_hash(std::int32_t x, std::int32_t z, std::uint32_t seed) noexcept {
    auto value = static_cast<std::uint32_t>(x) * 0x9e3779b1U;
    value ^= static_cast<std::uint32_t>(z) * 0x85ebca77U;
    value ^= seed;
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

[[nodiscard]] float hash_noise(std::int32_t x, std::int32_t z, std::uint32_t seed) noexcept {
    return static_cast<float>(coordinate_hash(x, z, seed) >> 8U) * (2.0F / 16'777'215.0F) - 1.0F;
}

[[nodiscard]] float smooth_curve(float value) noexcept {
    return value * value * (3.0F - 2.0F * value);
}

[[nodiscard]] float value_noise(float x, float z, std::uint32_t seed) noexcept {
    const auto cell_x = static_cast<std::int32_t>(std::floor(x));
    const auto cell_z = static_cast<std::int32_t>(std::floor(z));
    const auto local_x = smooth_curve(x - static_cast<float>(cell_x));
    const auto local_z = smooth_curve(z - static_cast<float>(cell_z));
    const auto a = std::lerp(hash_noise(cell_x, cell_z, seed), hash_noise(cell_x + 1, cell_z, seed), local_x);
    const auto b = std::lerp(hash_noise(cell_x, cell_z + 1, seed), hash_noise(cell_x + 1, cell_z + 1, seed), local_x);
    return std::lerp(a, b, local_z);
}

[[nodiscard]] float fractal_noise(float x, float z, std::uint32_t seed, std::int32_t octaves) noexcept {
    float value = 0.0F;
    float amplitude = 0.5F;
    float total_amplitude = 0.0F;
    for (std::int32_t octave = 0; octave < octaves; ++octave) {
        value += value_noise(x, z, seed + static_cast<std::uint32_t>(octave) * 0x68bc21ebU) * amplitude;
        total_amplitude += amplitude;
        x = x * 2.03F + 17.17F;
        z = z * 2.03F - 11.41F;
        amplitude *= 0.5F;
    }
    return value / total_amplitude;
}

[[nodiscard]] float ridged_noise(float x, float z, std::uint32_t seed) noexcept {
    float value = 0.0F;
    float amplitude = 0.55F;
    float total_amplitude = 0.0F;
    for (std::int32_t octave = 0; octave < 5; ++octave) {
        auto ridge = 1.0F - std::abs(value_noise(x, z, seed + static_cast<std::uint32_t>(octave) * 0x9e3779b9U));
        ridge *= ridge;
        value += ridge * amplitude;
        total_amplitude += amplitude;
        x = x * 2.07F - 23.7F;
        z = z * 2.07F + 8.3F;
        amplitude *= 0.48F;
    }
    return value / total_amplitude;
}

[[nodiscard]] float smooth_step(float lower, float upper, float value) noexcept {
    const auto normalized = std::clamp((value - lower) / (upper - lower), 0.0F, 1.0F);
    return smooth_curve(normalized);
}

[[nodiscard]] std::int32_t terrain_height(std::int32_t world_x, std::int32_t world_z) noexcept {
    const auto x = static_cast<float>(world_x);
    const auto z = static_cast<float>(world_z);
    const auto continents = fractal_noise(x * 0.00115F, z * 0.00115F, world_seed, 5);
    const auto rolling_hills = fractal_noise(x * 0.0105F, z * 0.0105F, world_seed ^ 0xa511e9b3U, 5);
    const auto mountain_region = smooth_step(-0.25F, 0.38F, continents);
    const auto ridges = ridged_noise(x * 0.0032F, z * 0.0032F, world_seed ^ 0x63d83595U);
    const auto small_ridges = ridged_noise(x * 0.009F, z * 0.009F, world_seed ^ 0x94d049bbU);
    const auto regional_variation = fractal_noise(x * 0.0018F, z * 0.0018F, world_seed ^ 0x369dea0fU, 4);
    const auto mountain_height = mountain_region * (2.0F + 23.0F * ridges * ridges * ridges);
    const auto height = 5.0F + rolling_hills * 3.0F + small_ridges * 2.5F +
        regional_variation * 2.2F + mountain_height;
    return std::clamp(static_cast<std::int32_t>(std::round(height)), 2, heartstead::Chunk::edge - 3);
}

struct TreePlacement {
    std::int32_t x{};
    std::int32_t z{};
    std::int32_t ground_y{};
    std::uint32_t random{};
    bool valid{};
};

[[nodiscard]] TreePlacement tree_placement(std::int32_t cell_x, std::int32_t cell_z) noexcept {
    constexpr std::int32_t tree_cell = 14;
    TreePlacement placement;
    placement.random = coordinate_hash(cell_x, cell_z, world_seed ^ 0x27d4eb2fU);
    placement.x = cell_x * tree_cell + 2 + static_cast<std::int32_t>(placement.random % 10U);
    placement.z = cell_z * tree_cell + 2 + static_cast<std::int32_t>((placement.random >> 8U) % 10U);
    placement.ground_y = terrain_height(placement.x, placement.z);
    const auto slope = std::max({
        std::abs(placement.ground_y - terrain_height(placement.x + 2, placement.z)),
        std::abs(placement.ground_y - terrain_height(placement.x - 2, placement.z)),
        std::abs(placement.ground_y - terrain_height(placement.x, placement.z + 2)),
        std::abs(placement.ground_y - terrain_height(placement.x, placement.z - 2)),
    });
    const auto forest = fractal_noise(static_cast<float>(placement.x) * 0.005F,
        static_cast<float>(placement.z) * 0.005F, world_seed ^ 0xb5297a4dU, 4);
    const auto chance = static_cast<float>((placement.random >> 16U) & 0xffffU) / 65535.0F;
    placement.valid = placement.ground_y <= 18 && slope <= 2 &&
        chance < smooth_step(-0.25F, 0.45F, forest) * 0.72F;
    return placement;
}

[[nodiscard]] heartstead::Chunk generate_chunk(
    heartstead::Int3 coordinate,
    const WorldEdits& edits) {
    using namespace heartstead;
    Chunk chunk;
    const auto world_minimum_x = coordinate.x * Chunk::edge;
    const auto world_minimum_z = coordinate.z * Chunk::edge;
    for (std::int32_t local_z = 0; local_z < Chunk::edge; ++local_z) {
        for (std::int32_t local_x = 0; local_x < Chunk::edge; ++local_x) {
            const auto world_x = world_minimum_x + local_x;
            const auto world_z = world_minimum_z + local_z;
            const auto height = terrain_height(world_x, world_z);
            const auto slope = std::max({
                std::abs(height - terrain_height(world_x + 1, world_z)),
                std::abs(height - terrain_height(world_x - 1, world_z)),
                std::abs(height - terrain_height(world_x, world_z + 1)),
                std::abs(height - terrain_height(world_x, world_z - 1)),
            });
            const auto rocky_surface = height >= 17 || slope >= 2;
            for (std::int32_t y = 0; y <= height; ++y) {
                BlockId block = 1;
                if (y == height) block = height >= 25 ? 8 : (rocky_surface ? 1 : (height <= 7 ? 7 : 3));
                else if (!rocky_surface && height < 25 && y > height - 4) block = height <= 7 ? 7 : 2;
                chunk.set(local_x, y, local_z, block);
            }
        }
    }
    const auto set_tree_block = [&](std::int32_t world_x, std::int32_t y, std::int32_t world_z,
        BlockId block, bool only_air) {
        if (world_x < world_minimum_x || world_x >= world_minimum_x + Chunk::edge ||
            world_z < world_minimum_z || world_z >= world_minimum_z + Chunk::edge ||
            y < 0 || y >= Chunk::edge) return;
        const auto local_x = world_x - world_minimum_x;
        const auto local_z = world_z - world_minimum_z;
        if (!only_air || chunk.get(local_x, y, local_z) == air_block) chunk.set(local_x, y, local_z, block);
    };
    constexpr std::int32_t tree_cell = 14;
    const auto first_cell_x = floor_div(world_minimum_x - 2, tree_cell);
    const auto first_cell_z = floor_div(world_minimum_z - 2, tree_cell);
    const auto last_cell_x = floor_div(world_minimum_x + Chunk::edge + 1, tree_cell);
    const auto last_cell_z = floor_div(world_minimum_z + Chunk::edge + 1, tree_cell);
    for (auto cell_z = first_cell_z; cell_z <= last_cell_z; ++cell_z) {
        for (auto cell_x = first_cell_x; cell_x <= last_cell_x; ++cell_x) {
            const auto tree = tree_placement(cell_x, cell_z);
            if (!tree.valid) continue;
            const auto trunk_height = 4 + static_cast<std::int32_t>((tree.random >> 5U) % 5U);
            const auto canopy_y = tree.ground_y + trunk_height - 1;
            for (std::int32_t dy = -2; dy <= 2; ++dy) {
                for (std::int32_t dz = -2; dz <= 2; ++dz) {
                    for (std::int32_t dx = -2; dx <= 2; ++dx) {
                        const auto distance = static_cast<float>(dx * dx + dz * dz) +
                            static_cast<float>(dy * dy) * 1.45F;
                        const auto leaf_random = coordinate_hash(tree.x + dx, tree.z + dz,
                            tree.random ^ static_cast<std::uint32_t>(dy));
                        if (distance <= 6.8F && (distance < 4.0F || (leaf_random & 3U) != 0U))
                            set_tree_block(tree.x + dx, canopy_y + dy, tree.z + dz, 6, true);
                    }
                }
            }
            for (std::int32_t y = tree.ground_y + 1; y <= tree.ground_y + trunk_height; ++y)
                set_tree_block(tree.x, y, tree.z, 5, false);
        }
    }
    const auto edited_chunk = edits.chunks.find(coordinate);
    if (edited_chunk != edits.chunks.end()) {
        for (const auto& [index, block] : edited_chunk->second) chunk.set(index, block);
    }
    return chunk;
}
[[nodiscard]] heartstead::ChunkWorld make_world(const WorldArea& area, const WorldEdits& edits) {
    using namespace heartstead;
    ChunkWorld world;
    const auto coordinates = world_coordinates(area);
    std::vector<Chunk*> chunks;
    chunks.reserve(coordinates.size());
    for (const auto coordinate : coordinates) chunks.push_back(&world.ensure_chunk(coordinate));
    parallel_for(chunks.size(), [&](std::size_t index) {
        *chunks[index] = generate_chunk(coordinates[index], edits);
    });
    return world;
}

[[nodiscard]] heartstead::WorldMesh mesh_world(const heartstead::ChunkWorld& world, const WorldArea& area) {
    using namespace heartstead;
    const auto blocks = BlockRegistry::defaults();
    const auto coordinates = world_coordinates(area);
    std::vector<ChunkMesh> chunk_meshes(coordinates.size());

    parallel_for(coordinates.size(), [&](std::size_t index) {
        const auto coordinate = coordinates[index];
        const auto* chunk = world.find_chunk(coordinate);
        if (chunk == nullptr) return;
        const ChunkNeighbors neighbors{
            .negative_x = world.find_chunk({coordinate.x - 1, coordinate.y, coordinate.z}),
            .positive_x = world.find_chunk({coordinate.x + 1, coordinate.y, coordinate.z}),
            .negative_y = world.find_chunk({coordinate.x, coordinate.y - 1, coordinate.z}),
            .positive_y = world.find_chunk({coordinate.x, coordinate.y + 1, coordinate.z}),
            .negative_z = world.find_chunk({coordinate.x, coordinate.y, coordinate.z - 1}),
            .positive_z = world.find_chunk({coordinate.x, coordinate.y, coordinate.z + 1}),
        };
        chunk_meshes[index] = GreedyMesher::build(*chunk, blocks, neighbors);
    });

    std::size_t total_vertices = 0;
    std::size_t total_indices = 0;
    for (const auto& chunk_mesh : chunk_meshes) {
        total_vertices += chunk_mesh.vertices.size();
        total_indices += chunk_mesh.indices.size();
    }

    WorldMesh combined;
    combined.vertices.reserve(total_vertices);
    combined.opaque_indices.reserve(total_indices);
    combined.cutout_indices.reserve(total_indices / 8U);
    combined.chunks.reserve(chunk_meshes.size());
    for (std::size_t mesh_index = 0; mesh_index < chunk_meshes.size(); ++mesh_index) {
        const auto& chunk_mesh = chunk_meshes[mesh_index];
        const auto coordinate = coordinates[mesh_index];
        const auto vertex_base = static_cast<std::uint32_t>(combined.vertices.size());
        const auto offset_x = (coordinate.x - area.center_chunk.x) * Chunk::edge;
        const auto offset_z = (coordinate.z - area.center_chunk.z) * Chunk::edge;
        for (auto vertex : chunk_mesh.vertices) {
            vertex.x = static_cast<std::int16_t>(static_cast<std::int32_t>(vertex.x) + offset_x);
            vertex.z = static_cast<std::int16_t>(static_cast<std::int32_t>(vertex.z) + offset_z);
            combined.vertices.push_back(vertex);
        }
        ChunkDrawRange range{
            .coordinates = coordinate,
            .first_vertex = vertex_base,
            .vertex_count = static_cast<std::uint32_t>(chunk_mesh.vertices.size()),
            .opaque_first_index = static_cast<std::uint32_t>(combined.opaque_indices.size()),
            .cutout_first_index = static_cast<std::uint32_t>(combined.cutout_indices.size()),
            .minimum_x = static_cast<std::int16_t>(offset_x),
            .minimum_z = static_cast<std::int16_t>(offset_z),
            .maximum_x = static_cast<std::int16_t>(offset_x + Chunk::edge),
            .maximum_z = static_cast<std::int16_t>(offset_z + Chunk::edge),
        };
        for (std::size_t index_offset = 0; index_offset + 5U < chunk_mesh.indices.size(); index_offset += 6U) {
            const auto first_vertex = chunk_mesh.indices[index_offset];
            const auto block = chunk_mesh.vertices[first_vertex].block;
            auto& destination = blocks.is_occluding(block)
                ? combined.opaque_indices
                : combined.cutout_indices;
            for (std::size_t triangle_index = 0; triangle_index < 6U; ++triangle_index) {
                destination.push_back(vertex_base + chunk_mesh.indices[index_offset + triangle_index]);
            }
        }
        range.opaque_index_count = static_cast<std::uint32_t>(combined.opaque_indices.size()) -
            range.opaque_first_index;
        range.cutout_index_count = static_cast<std::uint32_t>(combined.cutout_indices.size()) -
            range.cutout_first_index;
        combined.chunks.push_back(range);
        combined.quad_count += chunk_mesh.quad_count;
    }
    return combined;
}



} // namespace heartstead::world
