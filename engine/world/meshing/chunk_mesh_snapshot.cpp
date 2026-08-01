#include "engine/world/meshing/chunk_mesh_snapshot.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace heartstead::world {

namespace {

[[nodiscard]] constexpr std::int32_t floor_div(std::int32_t value, std::int32_t divisor) noexcept {
    auto quotient = value / divisor;
    if (value % divisor < 0) {
        --quotient;
    }
    return quotient;
}

[[nodiscard]] std::optional<std::int64_t> checked_add(std::int64_t value,
                                                      std::int32_t offset) noexcept {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if ((offset > 0 && value > maximum - offset) || (offset < 0 && value < minimum - offset)) {
        return std::nullopt;
    }
    return value + offset;
}

[[nodiscard]] MeshingGeometryKind geometry_kind(const BlockModelDefinition& model) noexcept {
    if (model.is_unit_cube()) {
        return MeshingGeometryKind::full_cube;
    }
    if (model.kind == BlockModelKind::cross_plane) {
        return MeshingGeometryKind::cross_plane;
    }
    if (model.kind == BlockModelKind::mesh) {
        return MeshingGeometryKind::rich_model;
    }
    if (!model.triangles.empty()) {
        return MeshingGeometryKind::authored_faces;
    }
    return MeshingGeometryKind::boxes;
}

[[nodiscard]] MeshingRenderPhase render_phase(const VoxelDefinition* definition,
                                              const BlockModelDefinition& model) noexcept {
    if (definition != nullptr && definition->logical_occupancy == BlockLogicalOccupancy::fluid) {
        return MeshingRenderPhase::fluid;
    }
    if (model.kind == BlockModelKind::cross_plane) {
        return MeshingRenderPhase::alpha_tested;
    }
    return MeshingRenderPhase::opaque;
}

[[nodiscard]] MeshingBlockInfo snapshot_model(const VoxelDefinition* definition,
                                              const BlockModelDefinition& model,
                                              std::uint16_t material_index) {
    MeshingBlockInfo result;
    result.defined = true;
    result.geometry = geometry_kind(model);
    result.render_phase = render_phase(definition, model);
    result.material_index = material_index;
    result.flags = definition != nullptr && definition->light_emission > 0
                       ? static_cast<std::uint16_t>(MeshingBlockFlags::emissive)
                       : 0;
    if (model.kind == BlockModelKind::cross_plane) {
        result.flags |= static_cast<std::uint16_t>(MeshingBlockFlags::two_sided);
    }
    if (model.kind == BlockModelKind::state_dependent) {
        result.flags |= static_cast<std::uint16_t>(MeshingBlockFlags::state_dependent);
    }
    result.full_occluder =
        definition == nullptr || definition->occlusion == BlockOcclusionBehavior::full_cube;
    result.occlusion_mask = result.full_occluder ? 0x3FU : 0U;
    result.neighbor_dependency_radius = model.neighbor_dependency_radius;
    if (definition != nullptr && definition->logical_occupancy == BlockLogicalOccupancy::fluid) {
        result.neighbor_dependency_radius =
            std::max<std::uint16_t>(result.neighbor_dependency_radius, 1);
    }
    result.boxes = model.boxes;
    result.triangles = model.triangles;
    result.model_prototype_id = model.prototype_id;
    result.render_bounds = model.render_bounds;
    return result;
}

} // namespace

const MeshingBlockInfo* BlockRenderTableSnapshot::find(std::uint16_t type) const noexcept {
    if (type == VoxelDefinition::air_type) {
        return nullptr;
    }
    if (type < blocks.size() && blocks[type].defined) {
        return &blocks[type];
    }
    if (!legacy_cube_fallback) {
        return nullptr;
    }
    static const MeshingBlockInfo legacy = snapshot_model(nullptr, legacy_cube_block_model(), 0);
    return &legacy;
}

core::Status BlockRenderTableSnapshot::validate() const {
    if (revision == 0) {
        return core::Status::failure("chunk_mesh.invalid_render_table_revision",
                                     "block render table revision must be nonzero");
    }
    for (std::size_t type = 1; type < blocks.size(); ++type) {
        const auto& block = blocks[type];
        if (!block.defined) {
            continue;
        }
        if (block.neighbor_dependency_radius > BlockModelDefinition::max_dependency_radius ||
            !block.render_bounds.is_valid()) {
            return core::Status::failure("chunk_mesh.invalid_render_table_entry",
                                         "block render table contains an invalid entry");
        }
        if (block.material_index == 0 || block.material_index != type ||
            block.occlusion_mask > 0x3FU ||
            (block.full_occluder && block.occlusion_mask != 0x3FU)) {
            return core::Status::failure(
                "chunk_mesh.invalid_compact_render_entry",
                "block render table compact material or occlusion metadata is invalid");
        }
        if ((block.geometry == MeshingGeometryKind::full_cube ||
             block.geometry == MeshingGeometryKind::boxes) &&
            block.boxes.empty()) {
            return core::Status::failure("chunk_mesh.missing_render_table_boxes",
                                         "box geometry requires at least one box");
        }
        if (block.geometry == MeshingGeometryKind::authored_faces && block.triangles.empty()) {
            return core::Status::failure("chunk_mesh.missing_render_table_triangles",
                                         "authored-face geometry requires at least one triangle");
        }
        if (block.geometry == MeshingGeometryKind::rich_model &&
            !block.model_prototype_id.is_valid()) {
            return core::Status::failure("chunk_mesh.invalid_render_table_model",
                                         "rich geometry requires a stable model id");
        }
    }
    return core::Status::ok();
}

core::Result<BlockRenderTableSnapshot>
build_block_render_table_snapshot(const VoxelPalette* palette) {
    BlockRenderTableSnapshot result;
    if (palette == nullptr) {
        result.legacy_cube_fallback = true;
        return core::Result<BlockRenderTableSnapshot>::success(std::move(result));
    }
    result.revision = palette->render_revision();
    std::uint16_t maximum_type = 0;
    for (const auto* definition : palette->definitions()) {
        maximum_type = std::max(maximum_type, definition->type);
    }
    result.blocks.resize(static_cast<std::size_t>(maximum_type) + 1);
    for (const auto* definition : palette->definitions()) {
        const auto& model = palette->model_for(*definition);
        result.blocks[definition->type] = snapshot_model(definition, model, definition->type);
    }
    auto status = result.validate();
    if (!status) {
        return core::Result<BlockRenderTableSnapshot>::failure(status.error().code,
                                                               status.error().message);
    }
    return core::Result<BlockRenderTableSnapshot>::success(std::move(result));
}

VoxelCell ChunkNeighborhoodSnapshot::cell(std::uint16_t x, std::uint16_t y,
                                          std::uint16_t z) const noexcept {
    if (x >= VoxelChunk::edge_length || y >= VoxelChunk::edge_length ||
        z >= VoxelChunk::edge_length) {
        return VoxelCell::air();
    }
    return cell_relative(x, y, z);
}

VoxelCell ChunkNeighborhoodSnapshot::cell_relative(std::int32_t x, std::int32_t y,
                                                   std::int32_t z) const noexcept {
    const auto halo = static_cast<std::int32_t>(halo_radius);
    const auto side = static_cast<std::int32_t>(side_length);
    const auto snapshot_x = x + halo;
    const auto snapshot_y = y + halo;
    const auto snapshot_z = z + halo;
    if (snapshot_x < 0 || snapshot_y < 0 || snapshot_z < 0 || snapshot_x >= side ||
        snapshot_y >= side || snapshot_z >= side) {
        return VoxelCell::air();
    }
    const auto index =
        static_cast<std::size_t>(snapshot_z) * static_cast<std::size_t>(side_length) *
            static_cast<std::size_t>(side_length) +
        static_cast<std::size_t>(snapshot_y) * static_cast<std::size_t>(side_length) +
        static_cast<std::size_t>(snapshot_x);
    return cells[index];
}

std::size_t ChunkNeighborhoodSnapshot::cell_count() const noexcept {
    return cells.size();
}

core::Status ChunkNeighborhoodSnapshot::validate() const {
    if (!center_identity.is_valid() || center_revision == 0 ||
        halo_radius > BlockModelDefinition::max_dependency_radius) {
        return core::Status::failure("chunk_mesh.invalid_neighborhood_metadata",
                                     "chunk neighborhood metadata is invalid");
    }
    const auto expected_side = static_cast<std::uint32_t>(VoxelChunk::edge_length) +
                               static_cast<std::uint32_t>(halo_radius) * 2U;
    if (side_length != expected_side) {
        return core::Status::failure("chunk_mesh.invalid_neighborhood_extent",
                                     "chunk neighborhood side length does not match its halo");
    }
    const auto side = static_cast<std::size_t>(side_length);
    if (cells.size() != side * side * side) {
        return core::Status::failure("chunk_mesh.invalid_neighborhood_cells",
                                     "chunk neighborhood cell storage is incomplete");
    }
    if (dependencies.empty()) {
        return core::Status::failure("chunk_mesh.missing_dependency_revisions",
                                     "chunk neighborhood has no dependency revisions");
    }
    return core::Status::ok();
}

core::Result<std::uint16_t> required_chunk_halo(std::span<const VoxelCell> center_cells,
                                                const BlockRenderTableSnapshot& render_table) {
    if (center_cells.size() != VoxelChunk::total_cells) {
        return core::Result<std::uint16_t>::failure("chunk_mesh.invalid_center_snapshot",
                                                    "center chunk snapshot has an invalid size");
    }
    std::uint16_t required = 0;
    for (const auto cell : center_cells) {
        if (cell.is_air()) {
            continue;
        }
        const auto* block = render_table.find(cell.type);
        if (block == nullptr) {
            return core::Result<std::uint16_t>::failure(
                "chunk_mesh.unknown_voxel_type",
                "center chunk contains a voxel missing from the block render table");
        }
        required = std::max(required, block->neighbor_dependency_radius);
    }
    return core::Result<std::uint16_t>::success(required);
}

core::Result<ChunkNeighborhoodSnapshot>
build_chunk_neighborhood_snapshot(const ChunkDatabase& chunks, ChunkIdentity center,
                                  const BlockRenderTableSnapshot& render_table,
                                  std::vector<VoxelCell> reusable_cells) {
    const auto* center_chunk = chunks.find(center.coordinate);
    if (center_chunk == nullptr || center_chunk->identity() != center) {
        return core::Result<ChunkNeighborhoodSnapshot>::failure(
            "chunk_mesh.stale_snapshot_identity",
            "cannot snapshot an unloaded or superseded chunk identity");
    }
    const auto halo = required_chunk_halo(center_chunk->cells(), render_table);
    if (!halo) {
        return core::Result<ChunkNeighborhoodSnapshot>::failure(halo.error().code,
                                                                halo.error().message);
    }

    ChunkNeighborhoodSnapshot result;
    result.center_identity = center;
    result.center_revision = center_chunk->content_revision();
    result.halo_radius = halo.value();
    result.side_length =
        static_cast<std::uint16_t>(static_cast<std::uint32_t>(VoxelChunk::edge_length) +
                                   static_cast<std::uint32_t>(result.halo_radius) * 2U);
    const auto side = static_cast<std::size_t>(result.side_length);
    reusable_cells.clear();
    reusable_cells.resize(side * side * side, VoxelCell::air());
    result.cells = std::move(reusable_cells);

    const auto halo_i32 = static_cast<std::int32_t>(result.halo_radius);
    const auto edge_i32 = static_cast<std::int32_t>(VoxelChunk::edge_length);
    const auto minimum_delta = floor_div(-halo_i32, edge_i32);
    const auto maximum_delta = floor_div(edge_i32 - 1 + halo_i32, edge_i32);
    const auto output_minimum = -halo_i32;
    const auto output_maximum = edge_i32 + halo_i32;
    const auto source_edge = static_cast<std::size_t>(edge_i32);
    const auto output_side = static_cast<std::size_t>(result.side_length);
    for (auto dz = minimum_delta; dz <= maximum_delta; ++dz) {
        for (auto dy = minimum_delta; dy <= maximum_delta; ++dy) {
            for (auto dx = minimum_delta; dx <= maximum_delta; ++dx) {
                const auto chunk_x = checked_add(center.coordinate.x, dx);
                const auto chunk_y = checked_add(center.coordinate.y, dy);
                const auto chunk_z = checked_add(center.coordinate.z, dz);
                if (!chunk_x || !chunk_y || !chunk_z) {
                    return core::Result<ChunkNeighborhoodSnapshot>::failure(
                        "chunk_mesh.snapshot_coordinate_overflow",
                        "chunk dependency crosses the signed coordinate limit");
                }
                ChunkDependencyRevision dependency;
                dependency.coordinate = {*chunk_x, *chunk_y, *chunk_z};
                const auto* chunk = chunks.find(dependency.coordinate);
                if (chunk != nullptr) {
                    dependency.present = true;
                    dependency.identity = chunk->identity();
                    dependency.content_revision = chunk->content_revision();

                    const auto begin_x = std::max(output_minimum, dx * edge_i32);
                    const auto end_x = std::min(output_maximum, (dx + 1) * edge_i32);
                    const auto begin_y = std::max(output_minimum, dy * edge_i32);
                    const auto end_y = std::min(output_maximum, (dy + 1) * edge_i32);
                    const auto begin_z = std::max(output_minimum, dz * edge_i32);
                    const auto end_z = std::min(output_maximum, (dz + 1) * edge_i32);
                    const auto copy_count = static_cast<std::size_t>(end_x - begin_x);
                    const auto source_cells = chunk->cells();
                    for (auto z = begin_z; z < end_z; ++z) {
                        const auto source_z = static_cast<std::size_t>(z - dz * edge_i32);
                        const auto output_z = static_cast<std::size_t>(z + halo_i32);
                        for (auto y = begin_y; y < end_y; ++y) {
                            const auto source_y = static_cast<std::size_t>(y - dy * edge_i32);
                            const auto output_y = static_cast<std::size_t>(y + halo_i32);
                            const auto source_x = static_cast<std::size_t>(begin_x - dx * edge_i32);
                            const auto output_x = static_cast<std::size_t>(begin_x + halo_i32);
                            const auto source_index = source_z * source_edge * source_edge +
                                                      source_y * source_edge + source_x;
                            const auto output_index = output_z * output_side * output_side +
                                                      output_y * output_side + output_x;
                            std::copy_n(source_cells.data() + source_index, copy_count,
                                        result.cells.data() + output_index);
                        }
                    }
                }
                result.dependencies.push_back(dependency);
            }
        }
    }
    auto status = result.validate();
    if (!status) {
        return core::Result<ChunkNeighborhoodSnapshot>::failure(status.error().code,
                                                                status.error().message);
    }
    return core::Result<ChunkNeighborhoodSnapshot>::success(std::move(result));
}

bool dependency_revisions_match(const ChunkDatabase& chunks,
                                std::span<const ChunkDependencyRevision> dependencies) noexcept {
    for (const auto& dependency : dependencies) {
        const auto* chunk = chunks.find(dependency.coordinate);
        if (!dependency.present) {
            if (chunk != nullptr) {
                return false;
            }
            continue;
        }
        if (chunk == nullptr || chunk->identity() != dependency.identity ||
            chunk->content_revision() != dependency.content_revision) {
            return false;
        }
    }
    return true;
}

} // namespace heartstead::world
