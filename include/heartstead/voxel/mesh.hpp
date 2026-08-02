#pragma once

#include "heartstead/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace heartstead {

// Compact, upload-ready vertex. Integer positions are exact within a chunk.
struct MeshVertex {
    std::int16_t x{};
    std::int16_t y{};
    std::int16_t z{};
    BlockId block{};
    std::int8_t normal_x{};
    std::int8_t normal_y{};
    std::int8_t normal_z{};
    std::uint8_t ambient_occlusion{3};
    std::uint16_t texture_u{};
    std::uint16_t texture_v{};
};
static_assert(sizeof(MeshVertex) == 16);

struct ChunkMesh {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint32_t quad_count{};

    [[nodiscard]] bool empty() const noexcept { return indices.empty(); }
    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return vertices.size() * sizeof(MeshVertex) + indices.size() * sizeof(std::uint32_t);
    }
};

struct ChunkDrawRange {
    Int3 coordinates{};
    std::uint32_t first_vertex{};
    std::uint32_t vertex_count{};
    std::uint32_t opaque_first_index{};
    std::uint32_t opaque_index_count{};
    std::uint32_t cutout_first_index{};
    std::uint32_t cutout_index_count{};
    std::int16_t minimum_x{};
    std::int16_t minimum_z{};
    std::int16_t maximum_x{};
    std::int16_t maximum_z{};
};

struct ChunkMeshUpdate {
    Int3 coordinates{};
    ChunkMesh mesh;
};

struct WorldMesh {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> opaque_indices;
    std::vector<std::uint32_t> cutout_indices;
    std::vector<ChunkDrawRange> chunks;
    std::uint32_t quad_count{};

    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return vertices.size() * sizeof(MeshVertex) +
            (opaque_indices.size() + cutout_indices.size()) * sizeof(std::uint32_t) +
            chunks.size() * sizeof(ChunkDrawRange);
    }
};

} // namespace heartstead
