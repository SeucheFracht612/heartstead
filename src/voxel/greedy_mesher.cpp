#include "heartstead/voxel/greedy_mesher.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace heartstead {
namespace {

struct Face {
    BlockId block{air_block};
    std::int8_t sign{};

    [[nodiscard]] bool visible() const noexcept { return sign != 0; }
    friend bool operator==(const Face&, const Face&) = default;
};

using Position = std::array<std::int32_t, 3>;

[[nodiscard]] BlockId sample(
    const Chunk& chunk,
    const ChunkNeighbors& neighbors,
    const Position& position) noexcept {
    if (position[0] < 0) {
        return neighbors.negative_x ? neighbors.negative_x->get(Chunk::edge - 1, position[1], position[2]) : air_block;
    }
    if (position[0] >= Chunk::edge) {
        return neighbors.positive_x ? neighbors.positive_x->get(0, position[1], position[2]) : air_block;
    }
    if (position[1] < 0) {
        return neighbors.negative_y ? neighbors.negative_y->get(position[0], Chunk::edge - 1, position[2]) : air_block;
    }
    if (position[1] >= Chunk::edge) {
        return neighbors.positive_y ? neighbors.positive_y->get(position[0], 0, position[2]) : air_block;
    }
    if (position[2] < 0) {
        return neighbors.negative_z ? neighbors.negative_z->get(position[0], position[1], Chunk::edge - 1) : air_block;
    }
    if (position[2] >= Chunk::edge) {
        return neighbors.positive_z ? neighbors.positive_z->get(position[0], position[1], 0) : air_block;
    }
    return chunk.get(position[0], position[1], position[2]);
}

[[nodiscard]] Face face_between(BlockId near_block, BlockId far_block, const BlockRegistry& blocks) noexcept {
    if (near_block == far_block) {
        return {};
    }
    if (blocks.is_renderable(near_block) && !blocks.is_occluding(far_block)) {
        return {near_block, 1};
    }
    if (blocks.is_renderable(far_block) && !blocks.is_occluding(near_block)) {
        return {far_block, -1};
    }
    return {};
}

MeshVertex make_vertex(
    const Position& position,
    std::size_t axis,
    std::int8_t sign,
    BlockId block,
    std::uint16_t texture_u,
    std::uint16_t texture_v) noexcept {
    MeshVertex vertex{
        .x = static_cast<std::int16_t>(position[0]),
        .y = static_cast<std::int16_t>(position[1]),
        .z = static_cast<std::int16_t>(position[2]),
        .block = block,
        .texture_u = texture_u,
        .texture_v = texture_v,
    };
    if (axis == 0) vertex.normal_x = sign;
    if (axis == 1) vertex.normal_y = sign;
    if (axis == 2) vertex.normal_z = sign;
    return vertex;
}

void emit_quad(
    ChunkMesh& mesh,
    const Position& origin,
    const Position& delta_u,
    const Position& delta_v,
    std::size_t axis,
    const Face& face,
    std::uint16_t width,
    std::uint16_t height) {
    const Position p0 = origin;
    const Position p1{origin[0] + delta_u[0], origin[1] + delta_u[1], origin[2] + delta_u[2]};
    const Position p2{p1[0] + delta_v[0], p1[1] + delta_v[1], p1[2] + delta_v[2]};
    const Position p3{origin[0] + delta_v[0], origin[1] + delta_v[1], origin[2] + delta_v[2]};

    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(make_vertex(p0, axis, face.sign, face.block, 0, 0));
    mesh.vertices.push_back(make_vertex(p1, axis, face.sign, face.block, width, 0));
    mesh.vertices.push_back(make_vertex(p2, axis, face.sign, face.block, width, height));
    mesh.vertices.push_back(make_vertex(p3, axis, face.sign, face.block, 0, height));

    if (face.sign > 0) {
        mesh.indices.insert(mesh.indices.end(), {base, base + 1U, base + 2U, base, base + 2U, base + 3U});
    } else {
        mesh.indices.insert(mesh.indices.end(), {base, base + 3U, base + 2U, base, base + 2U, base + 1U});
    }
    ++mesh.quad_count;
}

} // namespace

ChunkMesh GreedyMesher::build(
    const Chunk& chunk,
    const BlockRegistry& blocks,
    const ChunkNeighbors& neighbors) {
    constexpr auto size = Chunk::edge;
    std::array<Face, static_cast<std::size_t>(size * size)> mask{};
    ChunkMesh mesh;
    mesh.vertices.reserve(4096);
    mesh.indices.reserve(6144);

    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto axis_u = (axis + 1U) % 3U;
        const auto axis_v = (axis + 2U) % 3U;
        Position position{};
        Position step{};
        step[axis] = 1;

        for (position[axis] = -1; position[axis] < size;) {
            std::size_t mask_index = 0;
            for (position[axis_v] = 0; position[axis_v] < size; ++position[axis_v]) {
                for (position[axis_u] = 0; position[axis_u] < size; ++position[axis_u]) {
                    const auto near_block = sample(chunk, neighbors, position);
                    const Position far_position{
                        position[0] + step[0], position[1] + step[1], position[2] + step[2]};
                    const auto far_block = sample(chunk, neighbors, far_position);
                    auto face = face_between(near_block, far_block, blocks);
                    // Boundary geometry belongs to the chunk containing the rendered block.
                    // This keeps adjacent chunks from both emitting the same face.
                    if ((position[axis] < 0 && face.sign > 0) ||
                        (position[axis] == size - 1 && face.sign < 0)) {
                        face = {};
                    }
                    mask[mask_index++] = face;
                }
            }

            ++position[axis];
            for (std::int32_t row = 0; row < size; ++row) {
                for (std::int32_t column = 0; column < size;) {
                    const auto index = static_cast<std::size_t>(column + row * size);
                    const auto face = mask[index];
                    if (!face.visible()) {
                        ++column;
                        continue;
                    }

                    std::int32_t width = 1;
                    while (column + width < size && mask[index + static_cast<std::size_t>(width)] == face) {
                        ++width;
                    }

                    std::int32_t height = 1;
                    bool can_extend = true;
                    while (row + height < size && can_extend) {
                        for (std::int32_t offset = 0; offset < width; ++offset) {
                            const auto next = index + static_cast<std::size_t>(offset + height * size);
                            if (!(mask[next] == face)) {
                                can_extend = false;
                                break;
                            }
                        }
                        if (can_extend) ++height;
                    }

                    position[axis_u] = column;
                    position[axis_v] = row;
                    Position delta_u{};
                    Position delta_v{};
                    delta_u[axis_u] = width;
                    delta_v[axis_v] = height;
                    emit_quad(
                        mesh, position, delta_u, delta_v, axis, face,
                        static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height));

                    for (std::int32_t clear_row = 0; clear_row < height; ++clear_row) {
                        for (std::int32_t clear_column = 0; clear_column < width; ++clear_column) {
                            mask[index + static_cast<std::size_t>(clear_column + clear_row * size)] = {};
                        }
                    }
                    column += width;
                }
            }
        }
    }
    return mesh;
}

} // namespace heartstead
