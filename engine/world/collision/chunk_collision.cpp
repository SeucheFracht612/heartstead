#include "engine/world/collision/chunk_collision.hpp"

#include "engine/profiling/profiler.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace heartstead::world {

namespace {

constexpr math::Bounds3f full_cube_bounds{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
constexpr std::size_t maximum_collision_boxes = 65'535;

[[nodiscard]] bool has_positive_extent(const math::Bounds3f& bounds) noexcept {
    return bounds.is_valid() && bounds.max.x > bounds.min.x && bounds.max.y > bounds.min.y &&
           bounds.max.z > bounds.min.z;
}

[[nodiscard]] constexpr std::size_t cell_index(std::uint16_t x, std::uint16_t y,
                                               std::uint16_t z) noexcept {
    constexpr auto edge = static_cast<std::size_t>(VoxelChunk::edge_length);
    return static_cast<std::size_t>(z) * edge * edge + static_cast<std::size_t>(y) * edge +
           static_cast<std::size_t>(x);
}

[[nodiscard]] physics::CompoundShapeChild box_child(math::Vec3f minimum,
                                                    math::Vec3f maximum) noexcept {
    physics::CompoundShapeChild child;
    child.kind = physics::ShapeKind::box;
    child.local_position = (minimum + maximum) * 0.5F;
    child.half_extents = (maximum - minimum) * 0.5F;
    return child;
}

[[nodiscard]] core::Status append_box(ChunkCollisionShape& shape,
                                      physics::CompoundShapeChild child) {
    if (shape.boxes.size() >= maximum_collision_boxes) {
        return core::Status::failure("chunk_collision.box_limit_exceeded",
                                     "chunk collision output exceeds the per-shape box limit");
    }
    shape.boxes.push_back(child);
    return core::Status::ok();
}

} // namespace

bool ChunkCollisionBlockInfo::is_full_cube() const noexcept {
    return bounds.size() == 1 && bounds.front().min == full_cube_bounds.min &&
           bounds.front().max == full_cube_bounds.max;
}

const ChunkCollisionBlockInfo*
ChunkCollisionTableSnapshot::find(std::uint16_t type) const noexcept {
    if (type == VoxelDefinition::air_type) {
        return nullptr;
    }
    if (type < blocks.size() && blocks[type].defined) {
        return &blocks[type];
    }
    if (!legacy_cube_fallback) {
        return nullptr;
    }
    static const ChunkCollisionBlockInfo legacy{true, {full_cube_bounds}};
    return &legacy;
}

core::Status ChunkCollisionTableSnapshot::validate() const {
    if (revision == 0) {
        return core::Status::failure("chunk_collision.invalid_table_revision",
                                     "chunk collision table revision must be nonzero");
    }
    for (std::size_t type = 1; type < blocks.size(); ++type) {
        const auto& block = blocks[type];
        if (!block.defined) {
            continue;
        }
        if (std::ranges::any_of(block.bounds,
                                [](const auto& bounds) { return !has_positive_extent(bounds); })) {
            return core::Status::failure(
                "chunk_collision.invalid_table_bounds",
                "chunk collision table contains a non-volumetric collision box");
        }
    }
    return core::Status::ok();
}

core::Status ChunkCollisionSnapshot::validate() const {
    if (!identity.is_valid() || content_revision == 0 || collision_table_revision == 0) {
        return core::Status::failure("chunk_collision.invalid_snapshot_metadata",
                                     "chunk collision snapshot metadata is invalid");
    }
    if (cells.size() != VoxelChunk::total_cells) {
        return core::Status::failure("chunk_collision.invalid_snapshot_cells",
                                     "chunk collision snapshot cell storage is incomplete");
    }
    return core::Status::ok();
}

bool ChunkCollisionShape::empty() const noexcept {
    return boxes.empty();
}

core::Status ChunkCollisionShape::validate() const {
    if (!identity.is_valid() || content_revision == 0 || collision_table_revision == 0 ||
        boxes.size() > maximum_collision_boxes || stats.output_boxes != boxes.size()) {
        return core::Status::failure("chunk_collision.invalid_shape_metadata",
                                     "chunk collision shape metadata is inconsistent");
    }
    for (const auto& box : boxes) {
        physics::PhysicsShapeDesc shape;
        shape.kind = box.kind;
        shape.half_extents = box.half_extents;
        shape.radius = box.radius;
        shape.half_height = box.half_height;
        auto status = physics::validate_physics_shape_desc(shape);
        if (!status || !box.local_position.is_finite()) {
            return core::Status::failure("chunk_collision.invalid_shape_box",
                                         "chunk collision shape contains an invalid box");
        }
    }
    return core::Status::ok();
}

core::Result<ChunkCollisionTableSnapshot>
build_chunk_collision_table_snapshot(const VoxelPalette* palette) {
    ChunkCollisionTableSnapshot result;
    if (palette == nullptr) {
        result.legacy_cube_fallback = true;
        return core::Result<ChunkCollisionTableSnapshot>::success(std::move(result));
    }

    result.revision = palette->render_revision();
    std::uint16_t maximum_type = 0;
    for (const auto* definition : palette->definitions()) {
        maximum_type = std::max(maximum_type, definition->type);
    }
    result.blocks.resize(static_cast<std::size_t>(maximum_type) + 1);
    for (const auto* definition : palette->definitions()) {
        result.blocks[definition->type] = {true, definition->collision_bounds};
    }
    auto status = result.validate();
    if (!status) {
        return core::Result<ChunkCollisionTableSnapshot>::failure(status.error().code,
                                                                  status.error().message);
    }
    return core::Result<ChunkCollisionTableSnapshot>::success(std::move(result));
}

core::Result<ChunkCollisionSnapshot>
build_chunk_collision_snapshot(const ChunkDatabase& chunks, ChunkIdentity identity,
                               const ChunkCollisionTableSnapshot& collision_table,
                               std::vector<VoxelCell> reusable_cells) {
    auto table_status = collision_table.validate();
    if (!table_status) {
        return core::Result<ChunkCollisionSnapshot>::failure(table_status.error().code,
                                                             table_status.error().message);
    }
    const auto* chunk = chunks.find(identity.coordinate);
    if (chunk == nullptr || chunk->identity() != identity) {
        return core::Result<ChunkCollisionSnapshot>::failure(
            "chunk_collision.stale_snapshot_identity",
            "cannot snapshot an unloaded or superseded chunk identity");
    }

    reusable_cells.assign(chunk->cells().begin(), chunk->cells().end());
    ChunkCollisionSnapshot result;
    result.identity = identity;
    result.content_revision = chunk->content_revision();
    result.collision_table_revision = collision_table.revision;
    result.cells = std::move(reusable_cells);
    return core::Result<ChunkCollisionSnapshot>::success(std::move(result));
}

core::Result<ChunkCollisionShape>
cook_chunk_collision(const ChunkCollisionSnapshot& snapshot,
                     const ChunkCollisionTableSnapshot& collision_table) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("chunk_collision.cook");
    auto snapshot_status = snapshot.validate();
    auto table_status = collision_table.validate();
    if (!snapshot_status || !table_status ||
        snapshot.collision_table_revision != collision_table.revision) {
        const auto* error = !snapshot_status ? &snapshot_status.error()
                            : !table_status  ? &table_status.error()
                                             : nullptr;
        return core::Result<ChunkCollisionShape>::failure(
            error == nullptr ? "chunk_collision.table_revision_mismatch" : error->code,
            error == nullptr ? "chunk collision snapshot and table revisions differ"
                             : error->message);
    }

    ChunkCollisionShape result;
    result.identity = snapshot.identity;
    result.content_revision = snapshot.content_revision;
    result.collision_table_revision = snapshot.collision_table_revision;

    std::vector<bool> mergeable(VoxelChunk::total_cells, false);
    for (std::uint16_t z = 0; z < VoxelChunk::edge_length; ++z) {
        for (std::uint16_t y = 0; y < VoxelChunk::edge_length; ++y) {
            for (std::uint16_t x = 0; x < VoxelChunk::edge_length; ++x) {
                const auto index = cell_index(x, y, z);
                const auto cell = snapshot.cells[index];
                if (cell.is_air()) {
                    continue;
                }
                const auto* block = collision_table.find(cell.type);
                if (block == nullptr) {
                    return core::Result<ChunkCollisionShape>::failure(
                        "chunk_collision.unknown_voxel_type",
                        "chunk collision snapshot contains a voxel missing from the collision "
                        "table");
                }
                if (block->bounds.empty()) {
                    continue;
                }
                ++result.stats.source_colliding_cells;
                result.stats.source_collision_boxes +=
                    static_cast<std::uint32_t>(block->bounds.size());
                if (block->is_full_cube()) {
                    mergeable[index] = true;
                    continue;
                }
                const math::Vec3f cell_min{static_cast<float>(x), static_cast<float>(y),
                                           static_cast<float>(z)};
                for (const auto& bounds : block->bounds) {
                    auto status =
                        append_box(result, box_child(cell_min + bounds.min, cell_min + bounds.max));
                    if (!status) {
                        return core::Result<ChunkCollisionShape>::failure(status.error().code,
                                                                          status.error().message);
                    }
                }
            }
        }
    }

    std::vector<bool> consumed(VoxelChunk::total_cells, false);
    const auto available = [&](std::uint16_t x, std::uint16_t y, std::uint16_t z) {
        const auto index = cell_index(x, y, z);
        return mergeable[index] && !consumed[index];
    };
    for (std::uint16_t z = 0; z < VoxelChunk::edge_length; ++z) {
        for (std::uint16_t y = 0; y < VoxelChunk::edge_length; ++y) {
            for (std::uint16_t x = 0; x < VoxelChunk::edge_length; ++x) {
                if (!available(x, y, z)) {
                    continue;
                }

                std::uint16_t end_x = x + 1;
                while (end_x < VoxelChunk::edge_length && available(end_x, y, z)) {
                    ++end_x;
                }
                std::uint16_t end_y = y + 1;
                for (; end_y < VoxelChunk::edge_length; ++end_y) {
                    bool row_available = true;
                    for (auto candidate_x = x; candidate_x < end_x; ++candidate_x) {
                        row_available &= available(candidate_x, end_y, z);
                    }
                    if (!row_available) {
                        break;
                    }
                }
                std::uint16_t end_z = z + 1;
                for (; end_z < VoxelChunk::edge_length; ++end_z) {
                    bool plane_available = true;
                    for (auto candidate_y = y; candidate_y < end_y; ++candidate_y) {
                        for (auto candidate_x = x; candidate_x < end_x; ++candidate_x) {
                            plane_available &= available(candidate_x, candidate_y, end_z);
                        }
                    }
                    if (!plane_available) {
                        break;
                    }
                }

                for (auto consumed_z = z; consumed_z < end_z; ++consumed_z) {
                    for (auto consumed_y = y; consumed_y < end_y; ++consumed_y) {
                        for (auto consumed_x = x; consumed_x < end_x; ++consumed_x) {
                            consumed[cell_index(consumed_x, consumed_y, consumed_z)] = true;
                        }
                    }
                }
                auto status = append_box(
                    result,
                    box_child({static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)},
                              {static_cast<float>(end_x), static_cast<float>(end_y),
                               static_cast<float>(end_z)}));
                if (!status) {
                    return core::Result<ChunkCollisionShape>::failure(status.error().code,
                                                                      status.error().message);
                }
                ++result.stats.merged_full_cube_boxes;
            }
        }
    }

    result.stats.output_boxes = static_cast<std::uint32_t>(result.boxes.size());
    auto shape_status = result.validate();
    if (!shape_status) {
        return core::Result<ChunkCollisionShape>::failure(shape_status.error().code,
                                                          shape_status.error().message);
    }
    return core::Result<ChunkCollisionShape>::success(std::move(result));
}

} // namespace heartstead::world
