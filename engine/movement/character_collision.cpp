#include "engine/movement/character_collision.hpp"

#include "engine/world/fluids/fluid_volume_query.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace heartstead::movement {

namespace {

constexpr double collision_epsilon = 1.0 / 1'048'576.0;

[[nodiscard]] bool ranges_overlap(double a_min, double a_max, double b_min, double b_max) noexcept {
    return a_max > b_min + collision_epsilon && a_min < b_max - collision_epsilon;
}

[[nodiscard]] bool bounds_overlap(const math::Bounds3d& lhs, const math::Bounds3d& rhs) noexcept {
    return ranges_overlap(lhs.min.x, lhs.max.x, rhs.min.x, rhs.max.x) &&
           ranges_overlap(lhs.min.y, lhs.max.y, rhs.min.y, rhs.max.y) &&
           ranges_overlap(lhs.min.z, lhs.max.z, rhs.min.z, rhs.max.z);
}

[[nodiscard]] math::Bounds3d translated(math::Bounds3d bounds, math::Vec3d delta) noexcept {
    bounds.min += delta;
    bounds.max += delta;
    return bounds;
}

[[nodiscard]] math::Bounds3d swept_bounds(math::Bounds3d bounds, math::Vec3d delta) noexcept {
    return bounds.merged_with(translated(bounds, delta)).expanded(collision_epsilon);
}

[[nodiscard]] core::Result<std::int64_t> checked_floor(double local, std::int64_t origin_axis) {
    if (!std::isfinite(local)) {
        return core::Result<std::int64_t>::failure("character_collision.non_finite_bounds",
                                                   "collision bounds must be finite");
    }
    const auto floor_value = std::floor(local);
    if (floor_value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        floor_value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return core::Result<std::int64_t>::failure("character_collision.bounds_out_of_range",
                                                   "collision bounds exceed the world range");
    }
    const auto offset = static_cast<std::int64_t>(floor_value);
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if ((offset > 0 && origin_axis > maximum - offset) ||
        (offset < 0 && origin_axis < minimum - offset)) {
        return core::Result<std::int64_t>::failure("character_collision.bounds_out_of_range",
                                                   "collision bounds exceed the world range");
    }
    return core::Result<std::int64_t>::success(origin_axis + offset);
}

[[nodiscard]] bool has_tag(const world::VoxelDefinition& voxel, std::string_view tag) noexcept {
    return std::ranges::find(voxel.tags, tag) != voxel.tags.end();
}

} // namespace

core::Status CharacterShape::validate() const {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0 ||
        width > 16.0 || height > 16.0) {
        return core::Status::failure("character_collision.invalid_shape",
                                     "character shape dimensions must be finite and in (0, 16]");
    }
    return core::Status::ok();
}

math::Bounds3d character_local_bounds(const world::WorldPosition& position,
                                      world::BlockCoord origin,
                                      const CharacterShape& shape) noexcept {
    const auto feet = position.relative_to(origin);
    const auto half_width = shape.width * 0.5;
    return {{feet.x - half_width, feet.y, feet.z - half_width},
            {feet.x + half_width, feet.y + shape.height, feet.z + half_width}};
}

VoxelCharacterCollisionWorld::VoxelCharacterCollisionWorld(
    const world::ChunkDatabase& chunks, const world::VoxelPalette& palette) noexcept
    : chunks_(&chunks), palette_(&palette) {}

core::Result<std::vector<CharacterCollisionBox>>
VoxelCharacterCollisionWorld::collision_boxes(world::BlockCoord origin,
                                              math::Bounds3d local_bounds) const {
    return voxel_boxes(origin, local_bounds, false);
}

core::Result<std::vector<CharacterCollisionBox>>
VoxelCharacterCollisionWorld::voxel_boxes(world::BlockCoord origin, math::Bounds3d local_bounds,
                                          bool include_non_colliding) const {
    if (!local_bounds.is_valid()) {
        return core::Result<std::vector<CharacterCollisionBox>>::failure(
            "character_collision.invalid_bounds", "character collision query bounds are invalid");
    }
    auto min_x = checked_floor(local_bounds.min.x, origin.x);
    auto min_y = checked_floor(local_bounds.min.y, origin.y);
    auto min_z = checked_floor(local_bounds.min.z, origin.z);
    auto max_x = checked_floor(local_bounds.max.x - collision_epsilon, origin.x);
    auto max_y = checked_floor(local_bounds.max.y - collision_epsilon, origin.y);
    auto max_z = checked_floor(local_bounds.max.z - collision_epsilon, origin.z);
    if (!min_x || !min_y || !min_z || !max_x || !max_y || !max_z) {
        const auto* error = !min_x   ? &min_x.error()
                            : !min_y ? &min_y.error()
                            : !min_z ? &min_z.error()
                            : !max_x ? &max_x.error()
                            : !max_y ? &max_y.error()
                            : !max_z ? &max_z.error()
                                     : nullptr;
        return core::Result<std::vector<CharacterCollisionBox>>::failure(error->code,
                                                                         error->message);
    }
    constexpr std::int64_t maximum_axis_span = 64;
    if (max_x.value() - min_x.value() > maximum_axis_span ||
        max_y.value() - min_y.value() > maximum_axis_span ||
        max_z.value() - min_z.value() > maximum_axis_span) {
        return core::Result<std::vector<CharacterCollisionBox>>::failure(
            "character_collision.query_too_large", "character collision query is too large");
    }

    std::vector<CharacterCollisionBox> result;
    for (auto x = min_x.value();;) {
        for (auto y = min_y.value();;) {
            for (auto z = min_z.value();;) {
                const world::BlockCoord block{x, y, z};
                const auto chunk_local = world::block_to_chunk_local(block);
                const auto* chunk = chunks_->find(chunk_local.chunk);
                const auto local_translation =
                    math::Vec3d{static_cast<double>(static_cast<long double>(x) - origin.x),
                                static_cast<double>(static_cast<long double>(y) - origin.y),
                                static_cast<double>(static_cast<long double>(z) - origin.z)};
                if (chunk == nullptr) {
                    if (!include_non_colliding) {
                        result.push_back({{local_translation, local_translation + math::splat(1.0)},
                                          block,
                                          nullptr,
                                          true});
                    }
                    if (z == max_z.value()) {
                        break;
                    }
                    ++z;
                    continue;
                }
                auto cell = chunk->get(chunk_local.local);
                if (!cell || cell.value().is_air()) {
                    if (z == max_z.value()) {
                        break;
                    }
                    ++z;
                    continue;
                }
                const auto* voxel = palette_->find_by_type(cell.value().type);
                if (voxel == nullptr) {
                    result.push_back({{local_translation, local_translation + math::splat(1.0)},
                                      block,
                                      nullptr,
                                      false});
                    if (z == max_z.value()) {
                        break;
                    }
                    ++z;
                    continue;
                }
                if (include_non_colliding) {
                    result.push_back({{local_translation, local_translation + math::splat(1.0)},
                                      block,
                                      voxel,
                                      false});
                    if (z == max_z.value()) {
                        break;
                    }
                    ++z;
                    continue;
                }
                for (const auto& source : voxel->collision_bounds) {
                    math::Bounds3d bounds{
                        {static_cast<double>(source.min.x), static_cast<double>(source.min.y),
                         static_cast<double>(source.min.z)},
                        {static_cast<double>(source.max.x), static_cast<double>(source.max.y),
                         static_cast<double>(source.max.z)}};
                    result.push_back({translated(bounds, local_translation), block, voxel, false});
                }
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
    return core::Result<std::vector<CharacterCollisionBox>>::success(std::move(result));
}

core::Result<CharacterMoveResult>
VoxelCharacterCollisionWorld::move(const world::WorldPosition& position,
                                   const CharacterShape& shape, math::Vec3d desired_delta,
                                   double step_height, bool prevent_edge_drop) {
    auto shape_status = shape.validate();
    if (!shape_status || !position.is_valid() || !desired_delta.is_finite() ||
        !std::isfinite(step_height) || step_height < 0.0 || step_height > shape.height) {
        return core::Result<CharacterMoveResult>::failure(
            "character_collision.invalid_move", "character collision move input is invalid");
    }
    auto recovered = depenetrate(position, shape);
    if (!recovered) {
        return core::Result<CharacterMoveResult>::failure(recovered.error().code,
                                                          recovered.error().message);
    }
    if (recovered.value() != position) {
        auto moved = move(recovered.value(), shape, desired_delta, step_height, prevent_edge_drop);
        if (!moved) {
            return moved;
        }
        const auto recovery_delta =
            recovered.value().relative_to(position.anchor) - position.local_offset;
        moved.value().depenetration_delta += recovery_delta;
        moved.value().applied_delta += recovery_delta;
        return moved;
    }
    const auto origin = position.anchor;
    const auto start = character_local_bounds(position, origin, shape);
    auto boxes = collision_boxes(origin, swept_bounds(start, desired_delta).expanded(step_height));
    if (!boxes) {
        return core::Result<CharacterMoveResult>::failure(boxes.error().code,
                                                          boxes.error().message);
    }

    const auto resolve = [&boxes](math::Bounds3d bounds, math::Vec3d requested,
                                  CharacterMoveResult& output) {
        math::Vec3d applied{};
        const auto resolve_axis = [&](math::Axis3 axis, double amount) {
            if (amount == 0.0) {
                return 0.0;
            }
            auto allowed = amount;
            for (const auto& obstacle : boxes.value()) {
                const auto& other = obstacle.bounds;
                bool cross_overlap = false;
                double near_face = 0.0;
                double far_face = 0.0;
                switch (axis) {
                case math::Axis3::x:
                    cross_overlap =
                        ranges_overlap(bounds.min.y, bounds.max.y, other.min.y, other.max.y) &&
                        ranges_overlap(bounds.min.z, bounds.max.z, other.min.z, other.max.z);
                    near_face =
                        amount > 0.0 ? other.min.x - bounds.max.x : other.max.x - bounds.min.x;
                    far_face =
                        amount > 0.0 ? other.max.x - bounds.min.x : other.min.x - bounds.max.x;
                    break;
                case math::Axis3::y:
                    cross_overlap =
                        ranges_overlap(bounds.min.x, bounds.max.x, other.min.x, other.max.x) &&
                        ranges_overlap(bounds.min.z, bounds.max.z, other.min.z, other.max.z);
                    near_face =
                        amount > 0.0 ? other.min.y - bounds.max.y : other.max.y - bounds.min.y;
                    far_face =
                        amount > 0.0 ? other.max.y - bounds.min.y : other.min.y - bounds.max.y;
                    break;
                case math::Axis3::z:
                    cross_overlap =
                        ranges_overlap(bounds.min.x, bounds.max.x, other.min.x, other.max.x) &&
                        ranges_overlap(bounds.min.y, bounds.max.y, other.min.y, other.max.y);
                    near_face =
                        amount > 0.0 ? other.min.z - bounds.max.z : other.max.z - bounds.min.z;
                    far_face =
                        amount > 0.0 ? other.max.z - bounds.min.z : other.min.z - bounds.max.z;
                    break;
                }
                if (!cross_overlap) {
                    continue;
                }
                const auto crosses =
                    amount > 0.0
                        ? near_face >= -collision_epsilon && near_face < allowed && far_face > 0.0
                        : near_face <= collision_epsilon && near_face > allowed && far_face < 0.0;
                if (crosses) {
                    allowed = near_face;
                    output.blocked_by_unloaded_chunk |= obstacle.unloaded;
                }
            }
            return allowed;
        };

        applied.x = resolve_axis(math::Axis3::x, requested.x);
        bounds = translated(bounds, {applied.x, 0.0, 0.0});
        applied.z = resolve_axis(math::Axis3::z, requested.z);
        bounds = translated(bounds, {0.0, 0.0, applied.z});
        applied.y = resolve_axis(math::Axis3::y, requested.y);
        return applied;
    };

    CharacterMoveResult result;
    result.position = position;
    result.requested_delta = desired_delta;
    auto applied = resolve(start, desired_delta, result);
    const auto horizontal_blocked = std::abs(applied.x - desired_delta.x) > collision_epsilon ||
                                    std::abs(applied.z - desired_delta.z) > collision_epsilon;

    if (horizontal_blocked && step_height > 0.0 && desired_delta.y <= 0.0) {
        CharacterMoveResult stepped_result;
        auto raised = resolve(start, {0.0, step_height, 0.0}, stepped_result);
        if (raised.y >= step_height - collision_epsilon) {
            auto raised_bounds = translated(start, raised);
            auto horizontal =
                resolve(raised_bounds, {desired_delta.x, 0.0, desired_delta.z}, stepped_result);
            raised_bounds = translated(raised_bounds, horizontal);
            auto lowered = resolve(raised_bounds, {0.0, -step_height - 0.05, 0.0}, stepped_result);
            const auto stepped_horizontal = std::hypot(horizontal.x, horizontal.z);
            if (stepped_horizontal > std::hypot(applied.x, applied.z) + collision_epsilon &&
                lowered.y > -step_height - 0.05 + collision_epsilon) {
                applied = {horizontal.x, raised.y + lowered.y, horizontal.z};
                result.blocked_by_unloaded_chunk |= stepped_result.blocked_by_unloaded_chunk;
                result.stepped = true;
            }
        }
    }

    bool snapped_to_ground = false;
    if (desired_delta.y <= 0.0 && std::abs(applied.y - desired_delta.y) <= collision_epsilon) {
        CharacterMoveResult snap_result;
        const auto moved_bounds = translated(start, applied);
        const auto snap = resolve(moved_bounds, {0.0, -0.05, 0.0}, snap_result);
        if (snap.y > -0.05 + collision_epsilon) {
            applied.y += snap.y;
            result.blocked_by_unloaded_chunk |= snap_result.blocked_by_unloaded_chunk;
            snapped_to_ground = true;
        }
    }

    auto next = world::WorldPosition::from_anchor(position.anchor, position.local_offset + applied);
    if (!next) {
        return core::Result<CharacterMoveResult>::failure(next.error().code, next.error().message);
    }
    bool edge_drop_prevented = false;
    if (prevent_edge_drop && (desired_delta.x != 0.0 || desired_delta.z != 0.0)) {
        auto supported = has_support(next.value(), shape, 0.08);
        if (!supported) {
            return core::Result<CharacterMoveResult>::failure(supported.error().code,
                                                              supported.error().message);
        }
        if (!supported.value()) {
            applied.x = 0.0;
            applied.z = 0.0;
            next =
                world::WorldPosition::from_anchor(position.anchor, position.local_offset + applied);
            edge_drop_prevented = true;
        }
    }

    result.position = next.value();
    result.applied_delta = applied;
    result.hit_x = std::abs(applied.x - desired_delta.x) > collision_epsilon;
    result.hit_y = std::abs(applied.y - desired_delta.y) > collision_epsilon;
    result.hit_z = std::abs(applied.z - desired_delta.z) > collision_epsilon;
    result.grounded =
        desired_delta.y <= 0.0 && (result.hit_y || snapped_to_ground || edge_drop_prevented);
    result.hit_ceiling = desired_delta.y > 0.0 && result.hit_y;
    return core::Result<CharacterMoveResult>::success(result);
}

core::Result<bool> VoxelCharacterCollisionWorld::overlaps(const world::WorldPosition& position,
                                                          const CharacterShape& shape) {
    if (!position.is_valid() || !shape.validate()) {
        return core::Result<bool>::failure("character_collision.invalid_overlap",
                                           "character overlap input is invalid");
    }
    const auto bounds = character_local_bounds(position, position.anchor, shape);
    auto boxes = collision_boxes(position.anchor, bounds);
    if (!boxes) {
        return core::Result<bool>::failure(boxes.error().code, boxes.error().message);
    }
    return core::Result<bool>::success(std::ranges::any_of(
        boxes.value(), [&bounds](const auto& box) { return bounds_overlap(bounds, box.bounds); }));
}

core::Result<world::WorldPosition>
VoxelCharacterCollisionWorld::depenetrate(const world::WorldPosition& position,
                                          const CharacterShape& shape,
                                          std::uint32_t maximum_iterations) {
    if (!position.is_valid() || !shape.validate() || maximum_iterations == 0 ||
        maximum_iterations > 64) {
        return core::Result<world::WorldPosition>::failure(
            "character_collision.invalid_depenetration",
            "character depenetration input is invalid");
    }
    const auto origin = position.anchor;
    auto bounds = character_local_bounds(position, origin, shape);
    auto boxes = collision_boxes(origin, bounds.expanded(shape.height + 0.1));
    if (!boxes) {
        return core::Result<world::WorldPosition>::failure(boxes.error().code,
                                                           boxes.error().message);
    }
    math::Vec3d total{};
    for (std::uint32_t iteration = 0; iteration < maximum_iterations; ++iteration) {
        const CharacterCollisionBox* overlap = nullptr;
        for (const auto& box : boxes.value()) {
            if (bounds_overlap(bounds, box.bounds)) {
                overlap = &box;
                break;
            }
        }
        if (overlap == nullptr) {
            auto result = world::WorldPosition::from_anchor(origin, position.local_offset + total);
            if (!result) {
                return core::Result<world::WorldPosition>::failure(result.error().code,
                                                                   result.error().message);
            }
            return result;
        }

        const std::array pushes{
            math::Vec3d{overlap->bounds.min.x - bounds.max.x - collision_epsilon, 0.0, 0.0},
            math::Vec3d{overlap->bounds.max.x - bounds.min.x + collision_epsilon, 0.0, 0.0},
            math::Vec3d{0.0, overlap->bounds.min.y - bounds.max.y - collision_epsilon, 0.0},
            math::Vec3d{0.0, overlap->bounds.max.y - bounds.min.y + collision_epsilon, 0.0},
            math::Vec3d{0.0, 0.0, overlap->bounds.min.z - bounds.max.z - collision_epsilon},
            math::Vec3d{0.0, 0.0, overlap->bounds.max.z - bounds.min.z + collision_epsilon},
        };
        const auto magnitude = [](math::Vec3d value) {
            return std::abs(value.x) + std::abs(value.y) + std::abs(value.z);
        };
        const auto best = std::ranges::min_element(pushes, [&](math::Vec3d lhs, math::Vec3d rhs) {
            return magnitude(lhs) < magnitude(rhs);
        });
        bounds = translated(bounds, *best);
        total += *best;
    }
    return core::Result<world::WorldPosition>::failure(
        "character_collision.depenetration_failed",
        "character could not be separated from collision within the iteration limit");
}

core::Result<bool> VoxelCharacterCollisionWorld::has_support(const world::WorldPosition& position,
                                                             const CharacterShape& shape,
                                                             double probe_distance) {
    auto support = supporting_voxel(position, shape, probe_distance);
    if (!support) {
        return core::Result<bool>::failure(support.error().code, support.error().message);
    }
    return core::Result<bool>::success(support.value().has_value());
}

core::Result<std::optional<CharacterCollisionBox>>
VoxelCharacterCollisionWorld::supporting_voxel(const world::WorldPosition& position,
                                               const CharacterShape& shape,
                                               double probe_distance) const {
    if (!std::isfinite(probe_distance) || probe_distance <= 0.0 || probe_distance > 1.0) {
        return core::Result<std::optional<CharacterCollisionBox>>::failure(
            "character_collision.invalid_support_probe",
            "support probe distance must be in (0, 1]");
    }
    auto shape_status = shape.validate();
    if (!position.is_valid() || !shape_status) {
        return core::Result<std::optional<CharacterCollisionBox>>::failure(
            "character_collision.invalid_support_query",
            "support query requires a valid position and character shape");
    }
    const auto bounds = character_local_bounds(position, position.anchor, shape);
    const auto inset = std::min(0.02, shape.width * 0.1);
    const math::Bounds3d probe{
        {bounds.min.x + inset, bounds.min.y - probe_distance, bounds.min.z + inset},
        {bounds.max.x - inset, bounds.min.y + collision_epsilon, bounds.max.z - inset}};
    auto boxes = collision_boxes(position.anchor, probe);
    if (!boxes) {
        return core::Result<std::optional<CharacterCollisionBox>>::failure(boxes.error().code,
                                                                           boxes.error().message);
    }
    const CharacterCollisionBox* best = nullptr;
    double best_top = -std::numeric_limits<double>::infinity();
    double best_overlap_area = 0.0;
    for (const auto& box : boxes.value()) {
        if (!bounds_overlap(probe, box.bounds)) {
            continue;
        }
        const auto overlap_x = std::max(0.0, std::min(probe.max.x, box.bounds.max.x) -
                                                 std::max(probe.min.x, box.bounds.min.x));
        const auto overlap_z = std::max(0.0, std::min(probe.max.z, box.bounds.max.z) -
                                                 std::max(probe.min.z, box.bounds.min.z));
        const auto overlap_area = overlap_x * overlap_z;
        const auto top = box.bounds.max.y;
        if (best == nullptr || top > best_top + collision_epsilon ||
            (std::abs(top - best_top) <= collision_epsilon && overlap_area > best_overlap_area)) {
            best = &box;
            best_top = top;
            best_overlap_area = overlap_area;
        }
    }
    return core::Result<std::optional<CharacterCollisionBox>>::success(
        best == nullptr ? std::optional<CharacterCollisionBox>{}
                        : std::optional<CharacterCollisionBox>{*best});
}

core::Result<std::optional<LedgeProbeResult>>
VoxelCharacterCollisionWorld::probe_ledge(const world::WorldPosition& position,
                                          const CharacterShape& shape, math::Vec3d forward,
                                          double maximum_height, double reach) {
    if (!position.is_valid() || !shape.validate() || !forward.is_finite() ||
        !std::isfinite(maximum_height) || maximum_height <= 0.0 || maximum_height > 3.0 ||
        !std::isfinite(reach) || reach <= 0.0 || reach > 2.0) {
        return core::Result<std::optional<LedgeProbeResult>>::failure(
            "character_collision.invalid_ledge_probe", "ledge probe input is invalid");
    }
    forward.y = 0.0;
    const auto length = std::hypot(forward.x, forward.z);
    if (length < collision_epsilon) {
        return core::Result<std::optional<LedgeProbeResult>>::success(std::nullopt);
    }
    forward.x /= length;
    forward.z /= length;

    const auto origin = position.anchor;
    const auto feet = position.relative_to(origin);
    const auto half = shape.width * 0.45;
    const math::Bounds3d query{{feet.x + std::min(0.0, forward.x * reach) - half, feet.y,
                                feet.z + std::min(0.0, forward.z * reach) - half},
                               {feet.x + std::max(0.0, forward.x * reach) + half,
                                feet.y + maximum_height,
                                feet.z + std::max(0.0, forward.z * reach) + half}};
    auto boxes = collision_boxes(origin, query);
    if (!boxes) {
        return core::Result<std::optional<LedgeProbeResult>>::failure(boxes.error().code,
                                                                      boxes.error().message);
    }

    const auto probe_x = feet.x + forward.x * reach;
    const auto probe_z = feet.z + forward.z * reach;
    const CharacterCollisionBox* best = nullptr;
    double best_top = feet.y;
    for (const auto& box : boxes.value()) {
        const auto top = box.bounds.max.y;
        if (top <= feet.y + 0.2 || top > feet.y + maximum_height + collision_epsilon ||
            probe_x < box.bounds.min.x - half || probe_x > box.bounds.max.x + half ||
            probe_z < box.bounds.min.z - half || probe_z > box.bounds.max.z + half) {
            continue;
        }
        if (top > best_top) {
            best = &box;
            best_top = top;
        }
    }
    if (best == nullptr) {
        return core::Result<std::optional<LedgeProbeResult>>::success(std::nullopt);
    }

    auto target = world::WorldPosition::from_anchor(
        origin, {probe_x + forward.x * shape.width * 0.55, best_top,
                 probe_z + forward.z * shape.width * 0.55});
    if (!target) {
        return core::Result<std::optional<LedgeProbeResult>>::failure(target.error().code,
                                                                      target.error().message);
    }
    auto occupied = overlaps(target.value(), shape);
    if (!occupied) {
        return core::Result<std::optional<LedgeProbeResult>>::failure(occupied.error().code,
                                                                      occupied.error().message);
    }
    if (occupied.value()) {
        return core::Result<std::optional<LedgeProbeResult>>::success(std::nullopt);
    }
    return core::Result<std::optional<LedgeProbeResult>>::success(
        LedgeProbeResult{target.value(), best_top - feet.y, best->block});
}

core::Result<bool>
VoxelCharacterCollisionWorld::touches_occupancy(const world::WorldPosition& position,
                                                const CharacterShape& shape,
                                                world::BlockLogicalOccupancy occupancy) {
    const auto bounds = character_local_bounds(position, position.anchor, shape);
    auto boxes = voxel_boxes(position.anchor, bounds, true);
    if (!boxes) {
        return core::Result<bool>::failure(boxes.error().code, boxes.error().message);
    }
    return core::Result<bool>::success(std::ranges::any_of(boxes.value(), [&](const auto& box) {
        return box.voxel != nullptr && box.voxel->logical_occupancy == occupancy &&
               bounds_overlap(bounds, box.bounds);
    }));
}

core::Result<double>
VoxelCharacterCollisionWorld::fluid_submersion(const world::WorldPosition& position,
                                               const CharacterShape& shape) {
    auto shape_status = shape.validate();
    if (!shape_status || !position.is_valid()) {
        return core::Result<double>::failure(
            "character_collision.invalid_fluid_query",
            "fluid submersion requires a valid character position and shape");
    }
    const auto bounds = character_local_bounds(position, position.anchor, shape);
    return world::query_fluid_submersion(*chunks_, *palette_, position.anchor, bounds);
}

core::Result<bool> VoxelCharacterCollisionWorld::touches_tag(const world::WorldPosition& position,
                                                             const CharacterShape& shape,
                                                             std::string_view tag) {
    if (tag.empty()) {
        return core::Result<bool>::failure("character_collision.invalid_tag",
                                           "character collision tag must not be empty");
    }
    const auto bounds = character_local_bounds(position, position.anchor, shape);
    auto boxes = voxel_boxes(position.anchor, bounds.expanded(0.05), true);
    if (!boxes) {
        return core::Result<bool>::failure(boxes.error().code, boxes.error().message);
    }
    return core::Result<bool>::success(std::ranges::any_of(boxes.value(), [&](const auto& box) {
        return box.voxel != nullptr && has_tag(*box.voxel, tag) &&
               bounds_overlap(bounds.expanded(0.05), box.bounds);
    }));
}

} // namespace heartstead::movement
