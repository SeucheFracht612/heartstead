#include "engine/world/meshing/chunk_mesher.hpp"

#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/fluids/fluid_state.hpp"
#include "engine/world/meshing/chunk_mesh_snapshot.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace heartstead::world {

namespace {

struct FaceTemplate {
    ChunkMeshFaceDirection direction = ChunkMeshFaceDirection::positive_x;
    std::int32_t offset_x = 0;
    std::int32_t offset_y = 0;
    std::int32_t offset_z = 0;
    std::array<math::Vec3f, 4> corners{};
};

struct RelativeCellAddress {
    ChunkCoord chunk;
    VoxelCoord local;
};

struct CellModelView {
    VoxelCell cell{};
    const VoxelDefinition* definition = nullptr;
    const BlockModelDefinition* model = nullptr;
};

struct SnapshotCellModelView {
    VoxelCell cell{};
    const MeshingBlockInfo* block = nullptr;
};

[[nodiscard]] constexpr std::array<FaceTemplate, 6> face_templates() noexcept {
    return {
        FaceTemplate{ChunkMeshFaceDirection::negative_x,
                     -1,
                     0,
                     0,
                     {math::Vec3f{0.0F, 0.0F, 1.0F}, math::Vec3f{0.0F, 1.0F, 1.0F},
                      math::Vec3f{0.0F, 1.0F, 0.0F}, math::Vec3f{0.0F, 0.0F, 0.0F}}},
        FaceTemplate{ChunkMeshFaceDirection::positive_x,
                     1,
                     0,
                     0,
                     {math::Vec3f{1.0F, 0.0F, 0.0F}, math::Vec3f{1.0F, 1.0F, 0.0F},
                      math::Vec3f{1.0F, 1.0F, 1.0F}, math::Vec3f{1.0F, 0.0F, 1.0F}}},
        FaceTemplate{ChunkMeshFaceDirection::negative_y,
                     0,
                     -1,
                     0,
                     {math::Vec3f{0.0F, 0.0F, 0.0F}, math::Vec3f{1.0F, 0.0F, 0.0F},
                      math::Vec3f{1.0F, 0.0F, 1.0F}, math::Vec3f{0.0F, 0.0F, 1.0F}}},
        FaceTemplate{ChunkMeshFaceDirection::positive_y,
                     0,
                     1,
                     0,
                     {math::Vec3f{0.0F, 1.0F, 1.0F}, math::Vec3f{1.0F, 1.0F, 1.0F},
                      math::Vec3f{1.0F, 1.0F, 0.0F}, math::Vec3f{0.0F, 1.0F, 0.0F}}},
        FaceTemplate{ChunkMeshFaceDirection::negative_z,
                     0,
                     0,
                     -1,
                     {math::Vec3f{0.0F, 0.0F, 0.0F}, math::Vec3f{0.0F, 1.0F, 0.0F},
                      math::Vec3f{1.0F, 1.0F, 0.0F}, math::Vec3f{1.0F, 0.0F, 0.0F}}},
        FaceTemplate{ChunkMeshFaceDirection::positive_z,
                     0,
                     0,
                     1,
                     {math::Vec3f{1.0F, 0.0F, 1.0F}, math::Vec3f{1.0F, 1.0F, 1.0F},
                      math::Vec3f{0.0F, 1.0F, 1.0F}, math::Vec3f{0.0F, 0.0F, 1.0F}}},
    };
}

[[nodiscard]] constexpr std::int32_t floor_div(std::int32_t value, std::int32_t divisor) noexcept {
    auto quotient = value / divisor;
    if (value % divisor < 0) {
        --quotient;
    }
    return quotient;
}

[[nodiscard]] constexpr std::int32_t floor_mod(std::int32_t value, std::int32_t divisor) noexcept {
    auto remainder = value % divisor;
    if (remainder < 0) {
        remainder += divisor;
    }
    return remainder;
}

[[nodiscard]] std::optional<std::int64_t> checked_add(std::int64_t value,
                                                      std::int32_t offset) noexcept {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if ((offset > 0 && value > max - offset) || (offset < 0 && value < min - offset)) {
        return std::nullopt;
    }
    return value + offset;
}

[[nodiscard]] std::optional<RelativeCellAddress>
resolve_relative_cell(ChunkCoord center, std::int32_t x, std::int32_t y, std::int32_t z) noexcept {
    constexpr auto edge = static_cast<std::int32_t>(VoxelChunk::edge_length);
    const auto chunk_dx = floor_div(x, edge);
    const auto chunk_dy = floor_div(y, edge);
    const auto chunk_dz = floor_div(z, edge);
    auto chunk_x = checked_add(center.x, chunk_dx);
    auto chunk_y = checked_add(center.y, chunk_dy);
    auto chunk_z = checked_add(center.z, chunk_dz);
    if (!chunk_x || !chunk_y || !chunk_z) {
        return std::nullopt;
    }
    return RelativeCellAddress{
        {*chunk_x, *chunk_y, *chunk_z},
        {static_cast<std::uint16_t>(floor_mod(x, edge)),
         static_cast<std::uint16_t>(floor_mod(y, edge)),
         static_cast<std::uint16_t>(floor_mod(z, edge))},
    };
}

[[nodiscard]] const VoxelChunk* chunk_for_address(const ChunkMeshingContext& context,
                                                  ChunkCoord coord) noexcept {
    if (coord == context.chunk.coord()) {
        return &context.chunk;
    }
    return context.chunks == nullptr ? nullptr : context.chunks->find(coord);
}

[[nodiscard]] CellModelView query_cell(const ChunkMeshingContext& context, std::int32_t x,
                                       std::int32_t y, std::int32_t z) {
    const auto address = resolve_relative_cell(context.chunk.coord(), x, y, z);
    if (!address) {
        return {};
    }
    const auto* chunk = chunk_for_address(context, address->chunk);
    if (chunk == nullptr) {
        return {};
    }
    auto cell = chunk->get(address->local);
    if (!cell || cell.value().is_air()) {
        return {};
    }

    CellModelView view;
    view.cell = cell.value();
    if (context.palette == nullptr) {
        view.model = &legacy_cube_block_model();
        return view;
    }
    view.definition = context.palette->find_by_type(view.cell.type);
    if (view.definition == nullptr) {
        view.model = &legacy_cube_block_model();
        return view;
    }
    view.model = &context.palette->model_for(*view.definition);
    return view;
}

[[nodiscard]] bool is_full_occluder(const CellModelView& view) noexcept {
    if (view.cell.is_air()) {
        return false;
    }
    return view.definition == nullptr ||
           view.definition->occlusion == BlockOcclusionBehavior::full_cube;
}

[[nodiscard]] SnapshotCellModelView query_cell(const ChunkNeighborhoodSnapshot& neighborhood,
                                               const BlockRenderTableSnapshot& render_table,
                                               std::int32_t x, std::int32_t y,
                                               std::int32_t z) noexcept {
    SnapshotCellModelView result;
    result.cell = neighborhood.cell_relative(x, y, z);
    if (!result.cell.is_air()) {
        result.block = render_table.find(result.cell.type);
    }
    return result;
}

[[nodiscard]] bool is_full_occluder(const SnapshotCellModelView& view) noexcept {
    return !view.cell.is_air() && view.block != nullptr && view.block->full_occluder;
}

[[nodiscard]] bool nearly_equal(float left, float right) noexcept {
    return std::abs(left - right) <= 0.0001F;
}

[[nodiscard]] bool box_face_is_on_cell_boundary(const BlockModelBox& box,
                                                ChunkMeshFaceDirection direction) noexcept {
    switch (direction) {
    case ChunkMeshFaceDirection::negative_x:
        return nearly_equal(box.bounds.min.x, 0.0F);
    case ChunkMeshFaceDirection::positive_x:
        return nearly_equal(box.bounds.max.x, 1.0F);
    case ChunkMeshFaceDirection::negative_y:
        return nearly_equal(box.bounds.min.y, 0.0F);
    case ChunkMeshFaceDirection::positive_y:
        return nearly_equal(box.bounds.max.y, 1.0F);
    case ChunkMeshFaceDirection::negative_z:
        return nearly_equal(box.bounds.min.z, 0.0F);
    case ChunkMeshFaceDirection::positive_z:
        return nearly_equal(box.bounds.max.z, 1.0F);
    }
    return false;
}

void include_point(ChunkMesh& mesh, math::Vec3f point) noexcept {
    const auto had_geometry = !mesh.vertices.empty() || !mesh.rich_instances.empty();
    if (!had_geometry) {
        mesh.local_bounds = {point, point};
        return;
    }
    mesh.local_bounds.min = math::component_min(mesh.local_bounds.min, point);
    mesh.local_bounds.max = math::component_max(mesh.local_bounds.max, point);
}

void append_section_indices(ChunkMesh& mesh, std::uint16_t material_index,
                            MeshingRenderPhase render_phase, std::uint32_t first_index,
                            std::uint32_t index_count) {
    if (!mesh.sections.empty()) {
        auto& section = mesh.sections.back();
        if (section.material_index == material_index && section.render_phase == render_phase &&
            section.first_index + section.index_count == first_index) {
            section.index_count += index_count;
            return;
        }
    }
    mesh.sections.push_back({material_index, render_phase, first_index, index_count});
}

void add_quad(ChunkMesh& mesh, const std::array<math::Vec3f, 4>& positions, math::Vec3f normal,
              VoxelCell cell, std::uint16_t material_index, MeshingRenderPhase render_phase) {
    const auto base_index = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto first_index = static_cast<std::uint32_t>(mesh.indices.size());
    constexpr std::array<std::pair<float, float>, 4> uvs{
        std::pair{0.0F, 0.0F}, std::pair{1.0F, 0.0F}, std::pair{1.0F, 1.0F}, std::pair{0.0F, 1.0F}};
    for (std::size_t index = 0; index < positions.size(); ++index) {
        include_point(mesh, positions[index]);
        mesh.vertices.push_back(ChunkMeshVertex{positions[index], normal, uvs[index].first,
                                                uvs[index].second, cell.type, cell.light,
                                                cell.state_bits});
    }
    mesh.indices.insert(mesh.indices.end(), {base_index, base_index + 1, base_index + 2, base_index,
                                             base_index + 2, base_index + 3});
    append_section_indices(mesh, material_index, render_phase, first_index, 6);
    ++mesh.face_count;
}

void add_fluid_quad(ChunkMesh& mesh, const std::array<math::Vec3f, 4>& positions,
                    const std::array<math::Vec2f, 4>& uvs, math::Vec3f normal, VoxelCell cell,
                    std::uint16_t material_index) {
    const auto base_index = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto first_index = static_cast<std::uint32_t>(mesh.indices.size());
    for (std::size_t index = 0; index < positions.size(); ++index) {
        include_point(mesh, positions[index]);
        mesh.vertices.push_back({positions[index], normal, uvs[index].x, uvs[index].y, cell.type,
                                 cell.light, cell.state_bits});
    }
    mesh.indices.insert(mesh.indices.end(), {base_index, base_index + 1U, base_index + 2U,
                                             base_index, base_index + 2U, base_index + 3U});
    append_section_indices(mesh, material_index, MeshingRenderPhase::fluid, first_index, 6);
    ++mesh.face_count;
}

struct FluidMeshCell {
    VoxelCell cell{};
    bool full_occluder = false;
};

[[nodiscard]] math::Vec3f fluid_top_normal(const std::array<math::Vec3f, 4>& positions) noexcept {
    auto normal = math::cross(positions[1] - positions[0], positions[3] - positions[0]);
    const auto normal_length = static_cast<float>(math::length(normal));
    return normal_length > 0.00001F ? normal / normal_length : math::Vec3f{0.0F, 1.0F, 0.0F};
}

[[nodiscard]] std::array<math::Vec2f, 4>
fluid_top_uvs(FluidFlowDirection flow) noexcept {
    constexpr std::array<math::Vec2f, 4> positive_x{{
        {0.0F, 1.0F},
        {1.0F, 1.0F},
        {1.0F, 0.0F},
        {0.0F, 0.0F},
    }};
    switch (flow) {
    case FluidFlowDirection::negative_x:
        return {{{1.0F, 1.0F}, {0.0F, 1.0F}, {0.0F, 0.0F}, {1.0F, 0.0F}}};
    case FluidFlowDirection::positive_z:
        return {{{1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}, {0.0F, 0.0F}}};
    case FluidFlowDirection::negative_z:
        return {{{0.0F, 1.0F}, {0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}}};
    case FluidFlowDirection::none:
    case FluidFlowDirection::positive_x:
        return positive_x;
    }
    return positive_x;
}

template <typename Query>
[[nodiscard]] core::Result<float>
fluid_corner_height(Query&& query, std::int32_t x, std::int32_t y, std::int32_t z,
                    std::uint16_t fluid_type, int corner_x, int corner_z) {
    const std::array<int, 2> x_offsets = corner_x == 0 ? std::array{-1, 0} : std::array{0, 1};
    const std::array<int, 2> z_offsets = corner_z == 0 ? std::array{-1, 0} : std::array{0, 1};
    float total = 0.0F;
    std::uint32_t samples = 0;
    for (const auto dx : x_offsets) {
        for (const auto dz : z_offsets) {
            const auto sample = query(x + dx, y, z + dz);
            if (sample.cell.type != fluid_type) {
                continue;
            }
            auto state = decode_fluid_state(sample.cell.state_bits);
            if (!state) {
                return core::Result<float>::failure(state.error().code, state.error().message);
            }
            const auto above = query(x + dx, y + 1, z + dz);
            const auto height = state.value().falling || above.cell.type == fluid_type
                                    ? 1.0F
                                    : fluid_surface_height(state.value().amount);
            total += height;
            ++samples;
        }
    }
    if (samples == 0) {
        return core::Result<float>::failure(
            "chunk_mesh.missing_fluid_corner",
            "fluid surface corner has no contributing fluid cell");
    }
    return core::Result<float>::success(total / static_cast<float>(samples));
}

template <typename Query>
[[nodiscard]] core::Status add_fluid_cell(ChunkMesh& mesh, Query&& query, VoxelCoord coordinate,
                                          VoxelCell cell, std::uint16_t material_index) {
    auto state = decode_fluid_state(cell.state_bits);
    if (!state) {
        return core::Status::failure(state.error().code, state.error().message);
    }
    const auto x = static_cast<std::int32_t>(coordinate.x);
    const auto y = static_cast<std::int32_t>(coordinate.y);
    const auto z = static_cast<std::int32_t>(coordinate.z);
    std::array<float, 4> heights{};
    for (std::size_t index = 0; index < heights.size(); ++index) {
        constexpr std::array<std::array<int, 2>, 4> corners{{
            {{0, 1}},
            {{1, 1}},
            {{1, 0}},
            {{0, 0}},
        }};
        auto height = fluid_corner_height(query, x, y, z, cell.type, corners[index][0],
                                          corners[index][1]);
        if (!height) {
            return core::Status::failure(height.error().code, height.error().message);
        }
        heights[index] = height.value();
    }
    const math::Vec3f origin{static_cast<float>(coordinate.x),
                             static_cast<float>(coordinate.y),
                             static_cast<float>(coordinate.z)};
    const std::array<math::Vec3f, 4> top{{
        origin + math::Vec3f{0.0F, heights[0], 1.0F},
        origin + math::Vec3f{1.0F, heights[1], 1.0F},
        origin + math::Vec3f{1.0F, heights[2], 0.0F},
        origin + math::Vec3f{0.0F, heights[3], 0.0F},
    }};
    const auto above = query(x, y + 1, z);
    if (above.cell.type != cell.type && !above.full_occluder) {
        auto face_cell = cell;
        face_cell.light = std::max(face_cell.light, above.cell.light);
        add_fluid_quad(mesh, top, fluid_top_uvs(state.value().flow), fluid_top_normal(top),
                       face_cell, material_index);
    }

    constexpr std::array<math::Vec2f, 4> side_uvs{{
        {0.0F, 0.0F},
        {0.0F, 1.0F},
        {1.0F, 1.0F},
        {1.0F, 0.0F},
    }};
    struct Side {
        int dx;
        int dz;
        ChunkMeshFaceDirection direction;
        std::array<std::size_t, 2> top_indices;
        std::array<math::Vec3f, 2> bottom;
    };
    const std::array sides{
        Side{-1,
             0,
             ChunkMeshFaceDirection::negative_x,
             {0, 3},
             {origin + math::Vec3f{0.0F, 0.0F, 1.0F},
              origin + math::Vec3f{0.0F, 0.0F, 0.0F}}},
        Side{1,
             0,
             ChunkMeshFaceDirection::positive_x,
             {2, 1},
             {origin + math::Vec3f{1.0F, 0.0F, 0.0F},
              origin + math::Vec3f{1.0F, 0.0F, 1.0F}}},
        Side{0,
             -1,
             ChunkMeshFaceDirection::negative_z,
             {3, 2},
             {origin + math::Vec3f{0.0F, 0.0F, 0.0F},
              origin + math::Vec3f{1.0F, 0.0F, 0.0F}}},
        Side{0,
             1,
             ChunkMeshFaceDirection::positive_z,
             {1, 0},
             {origin + math::Vec3f{1.0F, 0.0F, 1.0F},
              origin + math::Vec3f{0.0F, 0.0F, 1.0F}}},
    };
    for (const auto& side : sides) {
        const auto neighbor = query(x + side.dx, y, z + side.dz);
        if (neighbor.cell.type == cell.type || neighbor.full_occluder) {
            continue;
        }
        auto face_cell = cell;
        face_cell.light = std::max(face_cell.light, neighbor.cell.light);
        const std::array<math::Vec3f, 4> positions{{
            side.bottom[0],
            top[side.top_indices[0]],
            top[side.top_indices[1]],
            side.bottom[1],
        }};
        add_fluid_quad(mesh, positions, side_uvs, chunk_mesh_face_normal(side.direction),
                       face_cell, material_index);
    }
    const auto below = query(x, y - 1, z);
    if (below.cell.type != cell.type && !below.full_occluder) {
        const std::array<math::Vec3f, 4> bottom{{
            origin + math::Vec3f{0.0F, 0.0F, 0.0F},
            origin + math::Vec3f{1.0F, 0.0F, 0.0F},
            origin + math::Vec3f{1.0F, 0.0F, 1.0F},
            origin + math::Vec3f{0.0F, 0.0F, 1.0F},
        }};
        add_fluid_quad(mesh, bottom, side_uvs,
                       chunk_mesh_face_normal(ChunkMeshFaceDirection::negative_y), cell,
                       material_index);
    }
    return core::Status::ok();
}

[[nodiscard]] MeshingRenderPhase mesh_render_phase(const CellModelView& view) noexcept {
    if (view.definition != nullptr &&
        view.definition->logical_occupancy == BlockLogicalOccupancy::fluid) {
        return MeshingRenderPhase::fluid;
    }
    if (view.model != nullptr && view.model->kind == BlockModelKind::cross_plane) {
        return MeshingRenderPhase::alpha_tested;
    }
    return MeshingRenderPhase::opaque;
}

[[nodiscard]] math::Vec3f box_corner(const BlockModelBox& box, math::Vec3f corner,
                                     math::Vec3f origin) noexcept {
    const auto extent = box.bounds.max - box.bounds.min;
    return origin + box.bounds.min +
           math::Vec3f{extent.x * corner.x, extent.y * corner.y, extent.z * corner.z};
}

void add_box(ChunkMesh& mesh, const ChunkMeshingContext& context, VoxelCoord coord, VoxelCell cell,
             const BlockModelBox& box, std::uint16_t material_index,
             MeshingRenderPhase render_phase) {
    const math::Vec3f origin{static_cast<float>(coord.x), static_cast<float>(coord.y),
                             static_cast<float>(coord.z)};
    for (const auto& face : face_templates()) {
        auto face_cell = cell;
        if (box_face_is_on_cell_boundary(box, face.direction)) {
            const auto neighbor =
                query_cell(context, static_cast<std::int32_t>(coord.x) + face.offset_x,
                           static_cast<std::int32_t>(coord.y) + face.offset_y,
                           static_cast<std::int32_t>(coord.z) + face.offset_z);
            if (is_full_occluder(neighbor)) {
                continue;
            }
            face_cell.light = std::max(face_cell.light, neighbor.cell.light);
        }
        std::array<math::Vec3f, 4> positions{};
        for (std::size_t index = 0; index < positions.size(); ++index) {
            positions[index] = box_corner(box, face.corners[index], origin);
        }
        add_quad(mesh, positions, chunk_mesh_face_normal(face.direction), face_cell, material_index,
                 render_phase);
    }
}

void add_box(ChunkMesh& mesh, const ChunkNeighborhoodSnapshot& neighborhood,
             const BlockRenderTableSnapshot& render_table, VoxelCoord coord, VoxelCell cell,
             const BlockModelBox& box, std::uint16_t material_index,
             MeshingRenderPhase render_phase) {
    const math::Vec3f origin{static_cast<float>(coord.x), static_cast<float>(coord.y),
                             static_cast<float>(coord.z)};
    for (const auto& face : face_templates()) {
        auto face_cell = cell;
        if (box_face_is_on_cell_boundary(box, face.direction)) {
            const auto neighbor = query_cell(neighborhood, render_table,
                                             static_cast<std::int32_t>(coord.x) + face.offset_x,
                                             static_cast<std::int32_t>(coord.y) + face.offset_y,
                                             static_cast<std::int32_t>(coord.z) + face.offset_z);
            if (is_full_occluder(neighbor)) {
                continue;
            }
            face_cell.light = std::max(face_cell.light, neighbor.cell.light);
        }
        std::array<math::Vec3f, 4> positions{};
        for (std::size_t index = 0; index < positions.size(); ++index) {
            positions[index] = box_corner(box, face.corners[index], origin);
        }
        add_quad(mesh, positions, chunk_mesh_face_normal(face.direction), face_cell, material_index,
                 render_phase);
    }
}

void add_cross_planes(ChunkMesh& mesh, VoxelCoord coord, VoxelCell cell,
                      std::uint16_t material_index, MeshingRenderPhase render_phase) {
    const math::Vec3f origin{static_cast<float>(coord.x), static_cast<float>(coord.y),
                             static_cast<float>(coord.z)};
    const std::array<math::Vec3f, 4> diagonal_a{
        origin + math::Vec3f{0.0F, 0.0F, 0.0F}, origin + math::Vec3f{0.0F, 1.0F, 0.0F},
        origin + math::Vec3f{1.0F, 1.0F, 1.0F}, origin + math::Vec3f{1.0F, 0.0F, 1.0F}};
    const std::array<math::Vec3f, 4> diagonal_b{
        origin + math::Vec3f{1.0F, 0.0F, 0.0F}, origin + math::Vec3f{1.0F, 1.0F, 0.0F},
        origin + math::Vec3f{0.0F, 1.0F, 1.0F}, origin + math::Vec3f{0.0F, 0.0F, 1.0F}};
    constexpr float diagonal = 0.70710677F;
    add_quad(mesh, diagonal_a, {diagonal, 0.0F, -diagonal}, cell, material_index, render_phase);
    add_quad(mesh, {diagonal_a[3], diagonal_a[2], diagonal_a[1], diagonal_a[0]},
             {-diagonal, 0.0F, diagonal}, cell, material_index, render_phase);
    add_quad(mesh, diagonal_b, {-diagonal, 0.0F, -diagonal}, cell, material_index, render_phase);
    add_quad(mesh, {diagonal_b[3], diagonal_b[2], diagonal_b[1], diagonal_b[0]},
             {diagonal, 0.0F, diagonal}, cell, material_index, render_phase);
}

void add_rich_instance(ChunkMesh& mesh, VoxelCoord coord, VoxelCell cell,
                       const BlockModelDefinition& model) {
    const math::Vec3f origin{static_cast<float>(coord.x), static_cast<float>(coord.y),
                             static_cast<float>(coord.z)};
    const math::Bounds3f bounds{origin + model.render_bounds.min, origin + model.render_bounds.max};
    if (mesh.vertices.empty() && mesh.rich_instances.empty()) {
        mesh.local_bounds = bounds;
    } else {
        mesh.local_bounds = mesh.local_bounds.merged_with(bounds);
    }
    mesh.rich_instances.push_back(
        {model.prototype_id, coord, bounds, cell.type, cell.state_bits, cell.metadata_handle});
}

void add_rich_instance(ChunkMesh& mesh, VoxelCoord coord, VoxelCell cell,
                       const MeshingBlockInfo& block) {
    const math::Vec3f origin{static_cast<float>(coord.x), static_cast<float>(coord.y),
                             static_cast<float>(coord.z)};
    const math::Bounds3f bounds{origin + block.render_bounds.min, origin + block.render_bounds.max};
    if (mesh.vertices.empty() && mesh.rich_instances.empty()) {
        mesh.local_bounds = bounds;
    } else {
        mesh.local_bounds = mesh.local_bounds.merged_with(bounds);
    }
    mesh.rich_instances.push_back({block.model_prototype_id, coord, bounds, cell.type,
                                   cell.state_bits, cell.metadata_handle});
}

[[nodiscard]] std::uint16_t required_halo_radius(const ChunkMeshingContext& context) {
    if (context.palette == nullptr) {
        return 0;
    }
    std::uint16_t required = 0;
    for (std::uint16_t z = 0; z < VoxelChunk::edge_length; ++z) {
        for (std::uint16_t y = 0; y < VoxelChunk::edge_length; ++y) {
            for (std::uint16_t x = 0; x < VoxelChunk::edge_length; ++x) {
                const auto cell = context.chunk.get({x, y, z});
                if (cell && !cell.value().is_air()) {
                    required = std::max(required,
                                        context.palette->neighbor_dependency_radius(cell.value()));
                }
            }
        }
    }
    return required;
}

} // namespace

bool ChunkMesh::empty() const noexcept {
    return vertices.empty() && indices.empty() && rich_instances.empty() && face_count == 0;
}

core::Status ChunkMesh::finalize_sections() {
    if (indices.empty()) {
        sections.clear();
        return core::Status::ok();
    }
    if (sections.empty()) {
        return core::Status::failure("chunk_mesh.missing_sections",
                                     "indexed chunk geometry has no material sections");
    }
    std::size_t covered_indices = 0;
    for (const auto& section : sections) {
        if (section.material_index == 0 || section.index_count == 0 ||
            section.first_index != covered_indices ||
            section.index_count > indices.size() - covered_indices) {
            return core::Status::failure(
                "chunk_mesh.invalid_section_range",
                "chunk mesh sections must cover the complete index buffer exactly once");
        }
        covered_indices += section.index_count;
    }
    if (covered_indices != indices.size()) {
        return core::Status::failure("chunk_mesh.incomplete_section_coverage",
                                     "chunk mesh sections do not cover the complete index buffer");
    }

    const auto section_less = [](const ChunkMeshSection& left, const ChunkMeshSection& right) {
        if (left.render_phase != right.render_phase) {
            return static_cast<std::uint8_t>(left.render_phase) <
                   static_cast<std::uint8_t>(right.render_phase);
        }
        return left.material_index < right.material_index;
    };
    if (std::ranges::is_sorted(sections, section_less)) {
        return core::Status::ok();
    }
    std::ranges::stable_sort(sections, section_less);
    std::vector<std::uint32_t> grouped_indices;
    grouped_indices.reserve(indices.size());
    std::vector<ChunkMeshSection> grouped_sections;
    grouped_sections.reserve(sections.size());
    for (const auto section : sections) {
        const auto output_first = static_cast<std::uint32_t>(grouped_indices.size());
        grouped_indices.insert(grouped_indices.end(), indices.begin() + section.first_index,
                               indices.begin() + section.first_index + section.index_count);
        if (!grouped_sections.empty() &&
            grouped_sections.back().material_index == section.material_index &&
            grouped_sections.back().render_phase == section.render_phase) {
            grouped_sections.back().index_count += section.index_count;
        } else {
            grouped_sections.push_back(
                {section.material_index, section.render_phase, output_first, section.index_count});
        }
    }
    indices = std::move(grouped_indices);
    sections = std::move(grouped_sections);
    return core::Status::ok();
}

core::Status ChunkMesh::validate() const {
    if (required_halo_radius > provided_halo_radius) {
        return core::Status::failure("chunk_mesh.insufficient_halo",
                                     "chunk mesh was built without its required neighbor halo");
    }
    if (empty()) {
        return core::Status::ok();
    }
    if (!local_bounds.is_valid()) {
        return core::Status::failure("chunk_mesh.invalid_bounds", "chunk mesh bounds are invalid");
    }
    if (vertices.empty() != indices.empty()) {
        return core::Status::failure("chunk_mesh.incomplete",
                                     "chunk mesh has incomplete vertex/index data");
    }
    if (indices.empty() != sections.empty()) {
        return core::Status::failure("chunk_mesh.incomplete_sections",
                                     "indexed chunk geometry and sections must exist together");
    }
    if (vertices.size() != face_count * 4) {
        return core::Status::failure("chunk_mesh.invalid_vertex_count",
                                     "chunk mesh vertex count does not match face count");
    }
    if (indices.size() != face_count * 6) {
        return core::Status::failure("chunk_mesh.invalid_index_count",
                                     "chunk mesh index count does not match face count");
    }
    if (vertices.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return core::Status::failure("chunk_mesh.too_many_vertices",
                                     "chunk mesh has too many vertices for uint32 indices");
    }
    for (const auto index : indices) {
        if (index >= vertices.size()) {
            return core::Status::failure("chunk_mesh.index_out_of_range",
                                         "chunk mesh index references a missing vertex");
        }
    }
    std::size_t covered_indices = 0;
    for (const auto& section : sections) {
        const auto phase = static_cast<std::uint8_t>(section.render_phase);
        if (section.material_index == 0 ||
            phase > static_cast<std::uint8_t>(MeshingRenderPhase::fluid) ||
            section.index_count == 0 || section.index_count % 3U != 0 ||
            section.first_index != covered_indices ||
            section.index_count > indices.size() - covered_indices) {
            return core::Status::failure(
                "chunk_mesh.invalid_section",
                "chunk mesh section material, phase, or index range is invalid");
        }
        covered_indices += section.index_count;
    }
    if (covered_indices != indices.size()) {
        return core::Status::failure("chunk_mesh.incomplete_section_coverage",
                                     "chunk mesh sections do not cover the complete index buffer");
    }
    for (const auto& vertex : vertices) {
        if (!vertex.position.is_finite() || !vertex.normal.is_finite()) {
            return core::Status::failure("chunk_mesh.invalid_vertex",
                                         "chunk mesh vertex contains non-finite values");
        }
        if (vertex.voxel_type == VoxelCell::air().type) {
            return core::Status::failure("chunk_mesh.air_vertex",
                                         "chunk mesh vertex cannot reference air");
        }
    }
    for (const auto& instance : rich_instances) {
        if (!instance.block_model_id.is_valid() || !instance.local_render_bounds.is_valid() ||
            instance.voxel_type == VoxelCell::air().type) {
            return core::Status::failure("chunk_mesh.invalid_rich_instance",
                                         "chunk mesh rich instance is invalid");
        }
    }
    return core::Status::ok();
}

core::Result<ChunkMesh> ChunkMesher::build_surface_mesh(const VoxelChunk& chunk) {
    return build_surface_mesh(ChunkMeshingContext{chunk, nullptr, nullptr, 0});
}

core::Result<ChunkMesh> ChunkMesher::build_surface_mesh(const ChunkMeshingContext& context) {
    ChunkMesh mesh;
    mesh.chunk_coord = context.chunk.coord();
    mesh.provided_halo_radius = context.available_halo_radius;
    mesh.required_halo_radius = required_halo_radius(context);
    if (mesh.required_halo_radius > mesh.provided_halo_radius) {
        return core::Result<ChunkMesh>::failure(
            "chunk_mesh.insufficient_halo",
            "block models in the chunk require a larger neighbor halo");
    }
    if (mesh.required_halo_radius > 0 && context.chunks == nullptr) {
        return core::Result<ChunkMesh>::failure(
            "chunk_mesh.missing_halo_source",
            "palette-driven meshing requires a chunk database for its neighbor halo");
    }

    for (std::uint16_t z = 0; z < VoxelChunk::edge_length; ++z) {
        for (std::uint16_t y = 0; y < VoxelChunk::edge_length; ++y) {
            for (std::uint16_t x = 0; x < VoxelChunk::edge_length; ++x) {
                const auto cell = context.chunk.get({x, y, z});
                if (!cell) {
                    return core::Result<ChunkMesh>::failure(cell.error().code,
                                                            cell.error().message);
                }
                if (cell.value().is_air()) {
                    continue;
                }
                const auto view = query_cell(context, x, y, z);
                if (context.palette != nullptr && view.definition == nullptr) {
                    return core::Result<ChunkMesh>::failure(
                        "chunk_mesh.unknown_voxel_type",
                        "chunk contains a voxel type missing from the active palette");
                }
                const auto& model = view.model == nullptr ? legacy_cube_block_model() : *view.model;
                const auto material_index = cell.value().type;
                const auto render_phase = mesh_render_phase(view);
                if (render_phase == MeshingRenderPhase::fluid) {
                    const auto fluid_query = [&context](std::int32_t query_x,
                                                        std::int32_t query_y,
                                                        std::int32_t query_z) {
                        const auto queried =
                            query_cell(context, query_x, query_y, query_z);
                        return FluidMeshCell{queried.cell, is_full_occluder(queried)};
                    };
                    auto fluid_status =
                        add_fluid_cell(mesh, fluid_query, {x, y, z}, cell.value(), material_index);
                    if (!fluid_status) {
                        return core::Result<ChunkMesh>::failure(
                            fluid_status.error().code, fluid_status.error().message);
                    }
                } else if (model.kind == BlockModelKind::mesh) {
                    add_rich_instance(mesh, {x, y, z}, cell.value(), model);
                } else if (model.kind == BlockModelKind::cross_plane) {
                    add_cross_planes(mesh, {x, y, z}, cell.value(), material_index, render_phase);
                } else {
                    for (const auto& box : model.boxes) {
                        add_box(mesh, context, {x, y, z}, cell.value(), box, material_index,
                                render_phase);
                    }
                }
            }
        }
    }

    auto status = mesh.finalize_sections();
    if (!status) {
        return core::Result<ChunkMesh>::failure(status.error().code, status.error().message);
    }
    status = mesh.validate();
    if (!status) {
        return core::Result<ChunkMesh>::failure(status.error().code, status.error().message);
    }
    return core::Result<ChunkMesh>::success(std::move(mesh));
}

core::Result<ChunkMesh>
ChunkMesher::build_surface_mesh(const ChunkNeighborhoodSnapshot& neighborhood,
                                const BlockRenderTableSnapshot& render_table) {
    auto status = neighborhood.validate();
    if (!status) {
        return core::Result<ChunkMesh>::failure(status.error().code, status.error().message);
    }
    status = render_table.validate();
    if (!status) {
        return core::Result<ChunkMesh>::failure(status.error().code, status.error().message);
    }

    ChunkMesh mesh;
    mesh.chunk_coord = neighborhood.center_identity.coordinate;
    mesh.provided_halo_radius = neighborhood.halo_radius;
    mesh.required_halo_radius = neighborhood.halo_radius;
    for (std::uint16_t z = 0; z < VoxelChunk::edge_length; ++z) {
        for (std::uint16_t y = 0; y < VoxelChunk::edge_length; ++y) {
            for (std::uint16_t x = 0; x < VoxelChunk::edge_length; ++x) {
                const auto cell = neighborhood.cell(x, y, z);
                if (cell.is_air()) {
                    continue;
                }
                const auto* block = render_table.find(cell.type);
                if (block == nullptr) {
                    return core::Result<ChunkMesh>::failure(
                        "chunk_mesh.unknown_voxel_type",
                        "snapshot contains a voxel type missing from its block render table");
                }
                if (block->render_phase == MeshingRenderPhase::fluid) {
                    const auto fluid_query = [&neighborhood, &render_table](
                                                 std::int32_t query_x,
                                                 std::int32_t query_y,
                                                 std::int32_t query_z) {
                        const auto queried = query_cell(neighborhood, render_table, query_x,
                                                       query_y, query_z);
                        return FluidMeshCell{queried.cell, is_full_occluder(queried)};
                    };
                    auto fluid_status = add_fluid_cell(
                        mesh, fluid_query, {x, y, z}, cell,
                        block->material_index == 0 ? cell.type : block->material_index);
                    if (!fluid_status) {
                        return core::Result<ChunkMesh>::failure(
                            fluid_status.error().code, fluid_status.error().message);
                    }
                } else if (block->geometry == MeshingGeometryKind::rich_model) {
                    add_rich_instance(mesh, {x, y, z}, cell, *block);
                } else if (block->geometry == MeshingGeometryKind::cross_plane) {
                    add_cross_planes(mesh, {x, y, z}, cell,
                                     block->material_index == 0 ? cell.type : block->material_index,
                                     block->render_phase);
                } else {
                    for (const auto& box : block->boxes) {
                        add_box(mesh, neighborhood, render_table, {x, y, z}, cell, box,
                                block->material_index == 0 ? cell.type : block->material_index,
                                block->render_phase);
                    }
                }
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

math::Vec3f chunk_mesh_face_normal(ChunkMeshFaceDirection direction) noexcept {
    switch (direction) {
    case ChunkMeshFaceDirection::negative_x:
        return {-1.0F, 0.0F, 0.0F};
    case ChunkMeshFaceDirection::positive_x:
        return {1.0F, 0.0F, 0.0F};
    case ChunkMeshFaceDirection::negative_y:
        return {0.0F, -1.0F, 0.0F};
    case ChunkMeshFaceDirection::positive_y:
        return {0.0F, 1.0F, 0.0F};
    case ChunkMeshFaceDirection::negative_z:
        return {0.0F, 0.0F, -1.0F};
    case ChunkMeshFaceDirection::positive_z:
        return {0.0F, 0.0F, 1.0F};
    }
    return {0.0F, 1.0F, 0.0F};
}

} // namespace heartstead::world
