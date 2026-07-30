#include "engine/renderer/terrain/gpu_chunk_vertex.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace heartstead::renderer::terrain {

namespace {

[[nodiscard]] std::int16_t encode_position(float value) noexcept {
    const auto scaled = std::round(value * terrain_vertex_fixed_scale);
    return static_cast<std::int16_t>(
        std::clamp(scaled, static_cast<float>(std::numeric_limits<std::int16_t>::min()),
                   static_cast<float>(std::numeric_limits<std::int16_t>::max())));
}

[[nodiscard]] std::uint16_t encode_uv(float value) noexcept {
    const auto scaled = std::round(value * terrain_vertex_fixed_scale);
    return static_cast<std::uint16_t>(
        std::clamp(scaled, 0.0F, static_cast<float>(std::numeric_limits<std::uint16_t>::max())));
}

[[nodiscard]] std::int8_t encode_normal(float value) noexcept {
    const auto scaled = std::round(std::clamp(value, -1.0F, 1.0F) * 127.0F);
    return static_cast<std::int8_t>(scaled);
}

} // namespace

GpuTerrainVertex to_gpu_chunk_vertex(const world::ChunkMeshVertex& vertex,
                                     std::uint8_t corner) noexcept {
    return GpuTerrainVertex{
        {encode_position(vertex.position.x), encode_position(vertex.position.y),
         encode_position(vertex.position.z), 0},
        {encode_uv(vertex.u), encode_uv(vertex.v)},
        vertex.voxel_type,
        vertex.state_bits,
        {encode_normal(vertex.normal.x), encode_normal(vertex.normal.y),
         encode_normal(vertex.normal.z), 0},
        {vertex.light, vertex.ambient_occlusion, corner, 0},
    };
}

std::vector<GpuTerrainVertex>
make_gpu_chunk_vertices(const std::vector<world::ChunkMeshVertex>& vertices) {
    return make_gpu_chunk_vertices(vertices, {});
}

std::vector<GpuTerrainVertex>
make_gpu_chunk_vertices(const std::vector<world::ChunkMeshVertex>& vertices,
                        std::vector<GpuTerrainVertex> reusable_vertices) {
    auto result = std::move(reusable_vertices);
    result.clear();
    result.reserve(vertices.size());
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        result.push_back(
            to_gpu_chunk_vertex(vertices[index], static_cast<std::uint8_t>(index % 4U)));
    }
    return result;
}

math::Vec3f decode_terrain_vertex_position(const GpuTerrainVertex& vertex) noexcept {
    return {static_cast<float>(vertex.position[0]) / terrain_vertex_fixed_scale,
            static_cast<float>(vertex.position[1]) / terrain_vertex_fixed_scale,
            static_cast<float>(vertex.position[2]) / terrain_vertex_fixed_scale};
}

math::Vec3f decode_terrain_vertex_normal(const GpuTerrainVertex& vertex) noexcept {
    return {static_cast<float>(vertex.normal[0]) / 127.0F,
            static_cast<float>(vertex.normal[1]) / 127.0F,
            static_cast<float>(vertex.normal[2]) / 127.0F};
}

std::array<float, 2> decode_terrain_vertex_uv(const GpuTerrainVertex& vertex) noexcept {
    return {static_cast<float>(vertex.uv[0]) / terrain_vertex_fixed_scale,
            static_cast<float>(vertex.uv[1]) / terrain_vertex_fixed_scale};
}

} // namespace heartstead::renderer::terrain
