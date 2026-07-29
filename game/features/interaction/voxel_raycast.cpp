#include "game/features/interaction/voxel_raycast.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace heartstead::game::interaction {

namespace {

struct AxisTraversal {
    std::int64_t step = 0;
    double next_distance = std::numeric_limits<double>::infinity();
    double distance_step = std::numeric_limits<double>::infinity();
};

struct SelectionBoxHit {
    double distance = 0.0;
    math::Coord3i face_normal{};
};

[[nodiscard]] AxisTraversal traversal_axis(double local, double direction) noexcept {
    if (direction > 0.0) {
        return {1, (1.0 - local) / direction, 1.0 / direction};
    }
    if (direction < 0.0) {
        return {-1, local / -direction, 1.0 / -direction};
    }
    return {};
}

[[nodiscard]] std::optional<SelectionBoxHit>
raycast_selection_box(math::Vec3d origin, math::Vec3d direction, math::Bounds3d bounds,
                      double maximum_distance) noexcept {
    double entry = 0.0;
    double exit = maximum_distance;
    math::Coord3i entry_normal{};
    const auto clip_axis = [&](double axis_origin, double axis_direction, double minimum,
                               double maximum, math::Coord3i minimum_normal,
                               math::Coord3i maximum_normal) {
        constexpr double parallel_epsilon = 1.0e-12;
        if (std::abs(axis_direction) <= parallel_epsilon) {
            return axis_origin >= minimum && axis_origin <= maximum;
        }
        auto first = (minimum - axis_origin) / axis_direction;
        auto second = (maximum - axis_origin) / axis_direction;
        auto first_normal = minimum_normal;
        if (first > second) {
            std::swap(first, second);
            first_normal = maximum_normal;
        }
        if (first > entry) {
            entry = first;
            entry_normal = first_normal;
        }
        exit = std::min(exit, second);
        return entry <= exit;
    };
    if (!clip_axis(origin.x, direction.x, bounds.min.x, bounds.max.x, {-1, 0, 0}, {1, 0, 0}) ||
        !clip_axis(origin.y, direction.y, bounds.min.y, bounds.max.y, {0, -1, 0}, {0, 1, 0}) ||
        !clip_axis(origin.z, direction.z, bounds.min.z, bounds.max.z, {0, 0, -1}, {0, 0, 1}) ||
        entry < 0.0 || entry > maximum_distance) {
        return std::nullopt;
    }
    return SelectionBoxHit{entry, entry_normal};
}

} // namespace

core::Result<VoxelRaycastResult> raycast_voxels(const world::ChunkDatabase& chunks,
                                                const VoxelRay& ray,
                                                const world::VoxelPalette* palette) {
    if (!ray.origin.is_valid() || !ray.direction.is_finite() ||
        !std::isfinite(ray.maximum_distance) || ray.maximum_distance <= 0.0) {
        return core::Result<VoxelRaycastResult>::failure("voxel_raycast.invalid_ray",
                                                         "voxel ray must have valid finite inputs");
    }
    const auto direction_length = math::length(ray.direction);
    if (!std::isfinite(direction_length) || direction_length <= 1.0e-12) {
        return core::Result<VoxelRaycastResult>::failure("voxel_raycast.zero_direction",
                                                         "voxel ray direction must be non-zero");
    }
    const auto direction = ray.direction / direction_length;
    auto x = traversal_axis(ray.origin.local_offset.x, direction.x);
    auto y = traversal_axis(ray.origin.local_offset.y, direction.y);
    auto z = traversal_axis(ray.origin.local_offset.z, direction.z);

    auto current = ray.origin.anchor;
    auto previous = current;
    double distance = 0.0;
    for (;;) {
        const auto address = world::block_to_chunk_local(current);
        const auto* chunk = chunks.find(address.chunk);
        if (chunk == nullptr) {
            return core::Result<VoxelRaycastResult>::success({std::nullopt, true});
        }
        auto cell = chunk->get(address.local);
        if (!cell) {
            return core::Result<VoxelRaycastResult>::failure(cell.error().code,
                                                             cell.error().message);
        }
        if (!cell.value().is_air()) {
            constexpr math::Bounds3f full_block{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
            const auto* definition =
                palette == nullptr ? nullptr : palette->find_by_type(cell.value().type);
            const auto selection_bounds =
                definition == nullptr
                    ? std::span<const math::Bounds3f>(&full_block, 1)
                    : std::span<const math::Bounds3f>(definition->selection_bounds);
            std::optional<SelectionBoxHit> nearest;
            const auto translation = math::Vec3d{
                static_cast<double>(static_cast<long double>(current.x) -
                                    static_cast<long double>(ray.origin.anchor.x)),
                static_cast<double>(static_cast<long double>(current.y) -
                                    static_cast<long double>(ray.origin.anchor.y)),
                static_cast<double>(static_cast<long double>(current.z) -
                                    static_cast<long double>(ray.origin.anchor.z)),
            };
            for (const auto& source : selection_bounds) {
                const math::Bounds3d bounds{
                    translation + math::Vec3d{static_cast<double>(source.min.x),
                                              static_cast<double>(source.min.y),
                                              static_cast<double>(source.min.z)},
                    translation + math::Vec3d{static_cast<double>(source.max.x),
                                              static_cast<double>(source.max.y),
                                              static_cast<double>(source.max.z)},
                };
                auto hit = raycast_selection_box(ray.origin.local_offset, direction, bounds,
                                                 ray.maximum_distance);
                if (hit && (!nearest || hit->distance < nearest->distance)) {
                    nearest = hit;
                }
            }
            if (nearest) {
                const world::BlockCoord face_offset{nearest->face_normal.x, nearest->face_normal.y,
                                                    nearest->face_normal.z};
                auto adjacent = world::checked_block_coord_offset(current, face_offset);
                if (!adjacent) {
                    return core::Result<VoxelRaycastResult>::failure(adjacent.error().code,
                                                                     adjacent.error().message);
                }
                VoxelRaycastResult result;
                result.hit = VoxelRaycastHit{
                    current,
                    nearest->face_normal == math::Coord3i{} ? previous : adjacent.value(),
                    nearest->face_normal,
                    cell.value(),
                    nearest->distance,
                };
                return core::Result<VoxelRaycastResult>::success(std::move(result));
            }
        }

        previous = current;
        world::BlockCoord step{};
        if (x.next_distance <= y.next_distance && x.next_distance <= z.next_distance) {
            distance = x.next_distance;
            x.next_distance += x.distance_step;
            step.x = x.step;
        } else if (y.next_distance <= z.next_distance) {
            distance = y.next_distance;
            y.next_distance += y.distance_step;
            step.y = y.step;
        } else {
            distance = z.next_distance;
            z.next_distance += z.distance_step;
            step.z = z.step;
        }
        if (distance > ray.maximum_distance) {
            return core::Result<VoxelRaycastResult>::success({});
        }
        auto next = world::checked_block_coord_offset(current, step);
        if (!next) {
            return core::Result<VoxelRaycastResult>::failure(next.error().code,
                                                             next.error().message);
        }
        current = next.value();
    }
}

} // namespace heartstead::game::interaction
