#include "engine/world/fluids/fluid_volume_query.hpp"

#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/fluids/fluid_state.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace heartstead::world {

namespace {

constexpr double query_epsilon = 1.0 / 1'048'576.0;

[[nodiscard]] core::Result<std::int64_t> checked_floor(double local,
                                                       std::int64_t origin_axis) {
    if (!std::isfinite(local)) {
        return core::Result<std::int64_t>::failure(
            "fluid_volume.non_finite_bounds", "fluid-volume bounds must be finite");
    }
    const auto floor_value = std::floor(local);
    if (floor_value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        floor_value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return core::Result<std::int64_t>::failure(
            "fluid_volume.bounds_out_of_range", "fluid-volume bounds exceed the world range");
    }
    const auto offset = static_cast<std::int64_t>(floor_value);
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if ((offset > 0 && origin_axis > maximum - offset) ||
        (offset < 0 && origin_axis < minimum - offset)) {
        return core::Result<std::int64_t>::failure(
            "fluid_volume.bounds_out_of_range", "fluid-volume bounds exceed the world range");
    }
    return core::Result<std::int64_t>::success(origin_axis + offset);
}

[[nodiscard]] double axis_overlap(double first_min, double first_max, double second_min,
                                  double second_max) noexcept {
    return std::max(0.0, std::min(first_max, second_max) -
                             std::max(first_min, second_min));
}

} // namespace

core::Result<double>
query_fluid_submersion(const ChunkDatabase& chunks, const VoxelPalette& palette,
                       BlockCoord local_origin, math::Bounds3d local_bounds) {
    if (!local_bounds.is_valid()) {
        return core::Result<double>::failure(
            "fluid_volume.invalid_bounds", "fluid-volume query bounds are invalid");
    }
    const auto extent = local_bounds.max - local_bounds.min;
    const auto volume = extent.x * extent.y * extent.z;
    if (!std::isfinite(volume) || volume <= 0.0) {
        return core::Result<double>::failure(
            "fluid_volume.empty_bounds", "fluid-volume query bounds must have positive volume");
    }

    auto min_x = checked_floor(local_bounds.min.x, local_origin.x);
    auto min_y = checked_floor(local_bounds.min.y, local_origin.y);
    auto min_z = checked_floor(local_bounds.min.z, local_origin.z);
    auto max_x = checked_floor(local_bounds.max.x - query_epsilon, local_origin.x);
    auto max_y = checked_floor(local_bounds.max.y - query_epsilon, local_origin.y);
    auto max_z = checked_floor(local_bounds.max.z - query_epsilon, local_origin.z);
    if (!min_x || !min_y || !min_z || !max_x || !max_y || !max_z) {
        const auto& error = !min_x   ? min_x.error()
                            : !min_y ? min_y.error()
                            : !min_z ? min_z.error()
                            : !max_x ? max_x.error()
                            : !max_y ? max_y.error()
                                     : max_z.error();
        return core::Result<double>::failure(error.code, error.message);
    }
    constexpr std::int64_t maximum_axis_span = 64;
    if (max_x.value() - min_x.value() > maximum_axis_span ||
        max_y.value() - min_y.value() > maximum_axis_span ||
        max_z.value() - min_z.value() > maximum_axis_span) {
        return core::Result<double>::failure(
            "fluid_volume.query_too_large", "fluid-volume query is too large");
    }

    const auto cell_overlap = [&](BlockCoord block) -> core::Result<double> {
        const auto location = block_to_chunk_local(block);
        const auto* chunk = chunks.find(location.chunk);
        if (chunk == nullptr) {
            return core::Result<double>::success(0.0);
        }
        auto cell = chunk->get(location.local);
        if (!cell || cell.value().is_air()) {
            return core::Result<double>::success(0.0);
        }
        const auto* definition = palette.find_by_type(cell.value().type);
        if (definition == nullptr ||
            definition->logical_occupancy != BlockLogicalOccupancy::fluid) {
            return core::Result<double>::success(0.0);
        }
        auto state = decode_fluid_cell(cell.value(), palette);
        if (!state) {
            return core::Result<double>::failure(state.error().code, state.error().message);
        }
        auto above_block = checked_block_coord_offset(block, {0, 1, 0});
        if (!above_block) {
            return core::Result<double>::failure(above_block.error().code,
                                                 above_block.error().message);
        }
        const auto above_location = block_to_chunk_local(above_block.value());
        const auto* above_chunk = chunks.find(above_location.chunk);
        auto above = VoxelCell::air();
        if (above_chunk != nullptr) {
            auto above_cell = above_chunk->get(above_location.local);
            if (!above_cell) {
                return core::Result<double>::failure(above_cell.error().code,
                                                     above_cell.error().message);
            }
            above = above_cell.value();
        }
        const auto height =
            state.value().falling || above.type == cell.value().type
                ? 1.0
                : static_cast<double>(fluid_surface_height(state.value().amount));
        const math::Vec3d cell_min{
            static_cast<double>(static_cast<long double>(block.x) - local_origin.x),
            static_cast<double>(static_cast<long double>(block.y) - local_origin.y),
            static_cast<double>(static_cast<long double>(block.z) - local_origin.z)};
        const math::Vec3d cell_max{cell_min.x + 1.0, cell_min.y + height,
                                   cell_min.z + 1.0};
        return core::Result<double>::success(
            axis_overlap(local_bounds.min.x, local_bounds.max.x, cell_min.x, cell_max.x) *
            axis_overlap(local_bounds.min.y, local_bounds.max.y, cell_min.y, cell_max.y) *
            axis_overlap(local_bounds.min.z, local_bounds.max.z, cell_min.z, cell_max.z));
    };

    double submerged_volume = 0.0;
    for (auto x = min_x.value();;) {
        for (auto y = min_y.value();;) {
            for (auto z = min_z.value();;) {
                auto overlap = cell_overlap({x, y, z});
                if (!overlap) {
                    return overlap;
                }
                submerged_volume += overlap.value();
                if (z == max_z.value()) {
                    break;
                }
                ++z;
            }
            if (y == max_y.value()) {
                break;
            }
            ++y;
        }
        if (x == max_x.value()) {
            break;
        }
        ++x;
    }
    return core::Result<double>::success(
        std::clamp(submerged_volume / volume, 0.0, 1.0));
}

} // namespace heartstead::world
