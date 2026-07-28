#include "engine/world/meshing/greedy_chunk_mesher.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace heartstead::world {

namespace {

constexpr auto edge = static_cast<std::uint16_t>(VoxelChunk::edge_length);
constexpr auto mask_cell_count = static_cast<std::size_t>(edge) * edge;

struct GreedyFaceKey {
    std::uint16_t voxel_type = 0;
    std::uint16_t material_index = 0;
    std::uint16_t state_bits = 0;
    std::uint16_t block_flags = 0;
    std::uint8_t light = 0;
    MeshingRenderPhase render_phase = MeshingRenderPhase::opaque;

    friend bool operator==(const GreedyFaceKey&, const GreedyFaceKey&) = default;
};

struct MaskCell {
    GreedyFaceKey key{};
    bool present = false;
};

[[nodiscard]] VoxelCell snapshot_cell(const ChunkNeighborhoodSnapshot& neighborhood, std::int32_t x,
                                      std::int32_t y, std::int32_t z) noexcept {
    const auto snapshot_x = x + static_cast<std::int32_t>(neighborhood.halo_radius);
    const auto snapshot_y = y + static_cast<std::int32_t>(neighborhood.halo_radius);
    const auto snapshot_z = z + static_cast<std::int32_t>(neighborhood.halo_radius);
    const auto side = static_cast<std::int32_t>(neighborhood.side_length);
    if (snapshot_x < 0 || snapshot_y < 0 || snapshot_z < 0 || snapshot_x >= side ||
        snapshot_y >= side || snapshot_z >= side) {
        return VoxelCell::air();
    }
    const auto side_size = static_cast<std::size_t>(neighborhood.side_length);
    const auto index = static_cast<std::size_t>(snapshot_z) * side_size * side_size +
                       static_cast<std::size_t>(snapshot_y) * side_size +
                       static_cast<std::size_t>(snapshot_x);
    return neighborhood.cells[index];
}

[[nodiscard]] const MeshingBlockInfo* find_block(const BlockRenderTableSnapshot& render_table,
                                                 std::uint16_t type) noexcept {
    if (type < render_table.blocks.size() && render_table.blocks[type].defined) {
        return &render_table.blocks[type];
    }
    return render_table.find(type);
}

[[nodiscard]] constexpr std::uint8_t face_bit(ChunkMeshFaceDirection direction) noexcept {
    return static_cast<std::uint8_t>(1U << static_cast<std::uint8_t>(direction));
}

struct CellAddress {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
};

[[nodiscard]] constexpr CellAddress address(ChunkMeshFaceDirection direction, std::int32_t slice,
                                            std::uint16_t u, std::uint16_t v) noexcept {
    switch (direction) {
    case ChunkMeshFaceDirection::negative_x:
    case ChunkMeshFaceDirection::positive_x:
        return {slice, u, v};
    case ChunkMeshFaceDirection::negative_y:
    case ChunkMeshFaceDirection::positive_y:
        return {u, slice, v};
    case ChunkMeshFaceDirection::negative_z:
    case ChunkMeshFaceDirection::positive_z:
        return {u, v, slice};
    }
    return {};
}

void include_point(ChunkMesh& mesh, math::Vec3f point) noexcept {
    if (mesh.vertices.empty()) {
        mesh.local_bounds = {point, point};
        return;
    }
    mesh.local_bounds.min = math::component_min(mesh.local_bounds.min, point);
    mesh.local_bounds.max = math::component_max(mesh.local_bounds.max, point);
}

void emit_quad(ChunkMesh& mesh, ChunkMeshFaceDirection direction, std::uint16_t slice,
               std::uint16_t u, std::uint16_t v, std::uint16_t width, std::uint16_t height,
               const GreedyFaceKey& key) {
    const auto plane_min = static_cast<float>(slice);
    const auto plane_max = static_cast<float>(slice + 1U);
    const auto u0 = static_cast<float>(u);
    const auto u1 = static_cast<float>(u + width);
    const auto v0 = static_cast<float>(v);
    const auto v1 = static_cast<float>(v + height);
    std::array<math::Vec3f, 4> positions{};
    switch (direction) {
    case ChunkMeshFaceDirection::negative_x:
        positions = {
            {{plane_min, u0, v1}, {plane_min, u1, v1}, {plane_min, u1, v0}, {plane_min, u0, v0}}};
        break;
    case ChunkMeshFaceDirection::positive_x:
        positions = {
            {{plane_max, u0, v0}, {plane_max, u1, v0}, {plane_max, u1, v1}, {plane_max, u0, v1}}};
        break;
    case ChunkMeshFaceDirection::negative_y:
        positions = {
            {{u0, plane_min, v0}, {u1, plane_min, v0}, {u1, plane_min, v1}, {u0, plane_min, v1}}};
        break;
    case ChunkMeshFaceDirection::positive_y:
        positions = {
            {{u0, plane_max, v1}, {u1, plane_max, v1}, {u1, plane_max, v0}, {u0, plane_max, v0}}};
        break;
    case ChunkMeshFaceDirection::negative_z:
        positions = {
            {{u0, v0, plane_min}, {u0, v1, plane_min}, {u1, v1, plane_min}, {u1, v0, plane_min}}};
        break;
    case ChunkMeshFaceDirection::positive_z:
        positions = {
            {{u1, v0, plane_max}, {u1, v1, plane_max}, {u0, v1, plane_max}, {u0, v0, plane_max}}};
        break;
    }

    const auto base_index = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto first_index = static_cast<std::uint32_t>(mesh.indices.size());
    const std::array<std::pair<float, float>, 4> uvs{
        std::pair{0.0F, 0.0F}, std::pair{static_cast<float>(width), 0.0F},
        std::pair{static_cast<float>(width), static_cast<float>(height)},
        std::pair{0.0F, static_cast<float>(height)}};
    const auto normal = chunk_mesh_face_normal(direction);
    for (std::size_t index = 0; index < positions.size(); ++index) {
        include_point(mesh, positions[index]);
        mesh.vertices.push_back({positions[index], normal, uvs[index].first, uvs[index].second,
                                 key.voxel_type, key.light, key.state_bits});
    }
    mesh.indices.insert(mesh.indices.end(), {base_index, base_index + 1U, base_index + 2U,
                                             base_index, base_index + 2U, base_index + 3U});
    if (!mesh.sections.empty() && mesh.sections.back().material_index == key.material_index &&
        mesh.sections.back().render_phase == key.render_phase &&
        mesh.sections.back().first_index + mesh.sections.back().index_count == first_index) {
        mesh.sections.back().index_count += 6;
    } else {
        mesh.sections.push_back({key.material_index, key.render_phase, first_index, 6});
    }
    ++mesh.face_count;
}

[[nodiscard]] bool cell_occludes(VoxelCell cell, ChunkMeshFaceDirection face,
                                 const BlockRenderTableSnapshot& render_table) noexcept {
    if (cell.is_air()) {
        return false;
    }
    const auto* block = find_block(render_table, cell.type);
    return block != nullptr && (block->occlusion_mask & face_bit(face)) != 0;
}

void write_mask_cell(MaskCell& item, VoxelCell cell, std::uint8_t face_light,
                     const BlockRenderTableSnapshot& render_table) noexcept {
    const auto* block = find_block(render_table, cell.type);
    item.present = block != nullptr;
    if (!item.present) {
        return;
    }
    item.key = {cell.type,
                block->material_index == 0 ? cell.type : block->material_index,
                cell.state_bits,
                block->flags,
                std::max(cell.light, face_light),
                block->render_phase};
}

void consume_mask(ChunkMesh& mesh, std::array<MaskCell, mask_cell_count>& mask,
                  ChunkMeshFaceDirection direction, std::uint16_t slice, std::uint16_t u_min,
                  std::uint16_t u_max, std::uint16_t v_min, std::uint16_t v_max) {
    for (std::uint16_t v = v_min; v <= v_max; ++v) {
        for (std::uint16_t u = u_min; u <= u_max;) {
            auto& first = mask[static_cast<std::size_t>(v) * edge + u];
            if (!first.present) {
                ++u;
                continue;
            }
            std::uint16_t width = 1;
            while (u + width <= u_max) {
                const auto& candidate = mask[static_cast<std::size_t>(v) * edge + u + width];
                if (!candidate.present || candidate.key != first.key) {
                    break;
                }
                ++width;
            }
            std::uint16_t height = 1;
            for (; v + height <= v_max; ++height) {
                bool compatible = true;
                for (std::uint16_t offset = 0; offset < width; ++offset) {
                    const auto& candidate =
                        mask[static_cast<std::size_t>(v + height) * edge + u + offset];
                    if (!candidate.present || candidate.key != first.key) {
                        compatible = false;
                        break;
                    }
                }
                if (!compatible) {
                    break;
                }
            }
            const auto key = first.key;
            for (std::uint16_t row = 0; row < height; ++row) {
                for (std::uint16_t column = 0; column < width; ++column) {
                    mask[static_cast<std::size_t>(v + row) * edge + u + column].present = false;
                }
            }
            emit_quad(mesh, direction, slice, u, v, width, height, key);
            u = static_cast<std::uint16_t>(u + width);
        }
    }
}

struct CubeCellSummary {
    std::size_t count = 0;
    VoxelCoord minimum{edge, edge, edge};
    VoxelCoord maximum{};
};

[[nodiscard]] core::Result<CubeCellSummary>
validate_and_count_cube_cells(const ChunkNeighborhoodSnapshot& neighborhood,
                              const BlockRenderTableSnapshot& render_table) {
    CubeCellSummary result;
    for (std::uint16_t z = 0; z < edge; ++z) {
        for (std::uint16_t y = 0; y < edge; ++y) {
            for (std::uint16_t x = 0; x < edge; ++x) {
                const auto cell = snapshot_cell(neighborhood, x, y, z);
                if (cell.is_air()) {
                    continue;
                }
                ++result.count;
                result.minimum = {std::min(result.minimum.x, x), std::min(result.minimum.y, y),
                                  std::min(result.minimum.z, z)};
                result.maximum = {std::max(result.maximum.x, x), std::max(result.maximum.y, y),
                                  std::max(result.maximum.z, z)};
                const auto* block = find_block(render_table, cell.type);
                if (block == nullptr) {
                    return core::Result<CubeCellSummary>::failure(
                        "chunk_mesh.unknown_voxel_type",
                        "snapshot contains a voxel type missing from its block render table");
                }
                if (block->geometry != MeshingGeometryKind::full_cube) {
                    return core::Result<CubeCellSummary>::success({});
                }
            }
        }
    }
    return core::Result<CubeCellSummary>::success(result);
}

} // namespace

core::Result<ChunkMesh>
GreedyChunkMesher::build_surface_mesh(const ChunkNeighborhoodSnapshot& neighborhood,
                                      const BlockRenderTableSnapshot& render_table) {
    return build_surface_mesh(neighborhood, render_table, {});
}

core::Result<ChunkMesh>
GreedyChunkMesher::build_surface_mesh(const ChunkNeighborhoodSnapshot& neighborhood,
                                      const BlockRenderTableSnapshot& render_table,
                                      ChunkMesh reusable_mesh) {
    auto status = neighborhood.validate();
    if (!status) {
        return core::Result<ChunkMesh>::failure(status.error().code, status.error().message);
    }
    status = render_table.validate();
    if (!status) {
        return core::Result<ChunkMesh>::failure(status.error().code, status.error().message);
    }
    auto cube_cells = validate_and_count_cube_cells(neighborhood, render_table);
    if (!cube_cells) {
        return core::Result<ChunkMesh>::failure(cube_cells.error().code,
                                                cube_cells.error().message);
    }
    if (cube_cells.value().count == 0) {
        // Specialized geometry remains on independent, reference-checked emitters while the full
        // cube hot loop is optimized. Empty chunks also take this harmless path.
        return ChunkMesher::build_surface_mesh(neighborhood, render_table);
    }

    ChunkMesh mesh = std::move(reusable_mesh);
    mesh.vertices.clear();
    mesh.indices.clear();
    mesh.sections.clear();
    mesh.rich_instances.clear();
    mesh.local_bounds = {};
    mesh.face_count = 0;
    mesh.chunk_coord = neighborhood.center_identity.coordinate;
    mesh.provided_halo_radius = neighborhood.halo_radius;
    mesh.required_halo_radius = neighborhood.halo_radius;
    const auto reserve_quads = std::min<std::size_t>(cube_cells.value().count * 2U, 16U * 1024U);
    mesh.vertices.reserve(reserve_quads * 4U);
    mesh.indices.reserve(reserve_quads * 6U);

    std::array<MaskCell, mask_cell_count> negative_mask{};
    std::array<MaskCell, mask_cell_count> positive_mask{};
    struct DirectionPair {
        ChunkMeshFaceDirection negative;
        ChunkMeshFaceDirection positive;
        std::uint16_t boundary_min;
        std::uint16_t boundary_max;
        std::uint16_t u_min;
        std::uint16_t u_max;
        std::uint16_t v_min;
        std::uint16_t v_max;
    };
    const auto minimum = cube_cells.value().minimum;
    const auto maximum = cube_cells.value().maximum;
    const std::array direction_pairs{
        DirectionPair{ChunkMeshFaceDirection::negative_x, ChunkMeshFaceDirection::positive_x,
                      minimum.x, static_cast<std::uint16_t>(maximum.x + 1U), minimum.y, maximum.y,
                      minimum.z, maximum.z},
        DirectionPair{ChunkMeshFaceDirection::negative_y, ChunkMeshFaceDirection::positive_y,
                      minimum.y, static_cast<std::uint16_t>(maximum.y + 1U), minimum.x, maximum.x,
                      minimum.z, maximum.z},
        DirectionPair{ChunkMeshFaceDirection::negative_z, ChunkMeshFaceDirection::positive_z,
                      minimum.z, static_cast<std::uint16_t>(maximum.z + 1U), minimum.x, maximum.x,
                      minimum.y, maximum.y},
    };
    for (const auto directions : direction_pairs) {
        for (std::uint16_t boundary = directions.boundary_min; boundary <= directions.boundary_max;
             ++boundary) {
            for (std::uint16_t v = directions.v_min; v <= directions.v_max; ++v) {
                for (std::uint16_t u = directions.u_min; u <= directions.u_max; ++u) {
                    const auto negative_source =
                        address(directions.negative, static_cast<std::int32_t>(boundary), u, v);
                    const auto positive_source =
                        address(directions.positive, static_cast<std::int32_t>(boundary) - 1, u, v);
                    const auto negative_cell = snapshot_cell(neighborhood, negative_source.x,
                                                             negative_source.y, negative_source.z);
                    const auto positive_cell = snapshot_cell(neighborhood, positive_source.x,
                                                             positive_source.y, positive_source.z);
                    const auto mask_index = static_cast<std::size_t>(v) * edge + u;
                    if (boundary < edge && !negative_cell.is_air() &&
                        !cell_occludes(positive_cell, directions.positive, render_table)) {
                        write_mask_cell(negative_mask[mask_index], negative_cell,
                                        positive_cell.light, render_table);
                    }
                    if (boundary > 0 && !positive_cell.is_air() &&
                        !cell_occludes(negative_cell, directions.negative, render_table)) {
                        write_mask_cell(positive_mask[mask_index], positive_cell,
                                        negative_cell.light, render_table);
                    }
                }
            }
            if (boundary <= directions.boundary_max - 1U) {
                consume_mask(mesh, negative_mask, directions.negative, boundary, directions.u_min,
                             directions.u_max, directions.v_min, directions.v_max);
            }
            if (boundary > directions.boundary_min) {
                consume_mask(mesh, positive_mask, directions.positive,
                             static_cast<std::uint16_t>(boundary - 1), directions.u_min,
                             directions.u_max, directions.v_min, directions.v_max);
            }
        }
    }

    status = mesh.finalize_sections();
    if (!status) {
        return core::Result<ChunkMesh>::failure(status.error().code, status.error().message);
    }
    status = mesh.validate();
    if (!status) {
        return core::Result<ChunkMesh>::failure(status.error().code, status.error().message);
    }
    return core::Result<ChunkMesh>::success(std::move(mesh));
}

} // namespace heartstead::world
