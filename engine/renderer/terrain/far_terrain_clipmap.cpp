#include "engine/renderer/terrain/far_terrain_clipmap.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <ranges>
#include <tuple>

namespace heartstead::renderer {

namespace {

[[nodiscard]] std::int64_t floor_to_i64(double value) noexcept {
    if (value <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if (value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(std::floor(value));
}

[[nodiscard]] double patch_size(const FarTerrainClipmapConfig& config,
                                std::uint32_t level) noexcept {
    return config.base_cell_size * std::exp2(static_cast<double>(level)) *
           static_cast<double>(config.patch_resolution);
}

[[nodiscard]] bool fully_inside(const math::Bounds3d& candidate,
                                const math::Bounds3d& coverage) noexcept {
    return candidate.min.x >= coverage.min.x && candidate.max.x <= coverage.max.x &&
           candidate.min.z >= coverage.min.z && candidate.max.z <= coverage.max.z;
}

[[nodiscard]] math::Bounds3d level_coverage(math::Vec3d camera,
                                            const FarTerrainClipmapConfig& config,
                                            std::uint32_t level) noexcept {
    const auto size = patch_size(config, level);
    const auto center_x = floor_to_i64(camera.x / size);
    const auto center_z = floor_to_i64(camera.z / size);
    const auto radius = static_cast<std::int64_t>(config.patches_per_axis / 2U);
    return {{static_cast<double>(center_x - radius) * size,
             -std::numeric_limits<double>::max(),
             static_cast<double>(center_z - radius) * size},
            {static_cast<double>(center_x + radius + 1) * size,
             std::numeric_limits<double>::max(),
             static_cast<double>(center_z + radius + 1) * size}};
}

[[nodiscard]] math::Vec3f normalized(math::Vec3f value) noexcept {
    const auto length = static_cast<float>(math::length(value));
    return length > 0.0F ? value / length : math::Vec3f{0.0F, 1.0F, 0.0F};
}

} // namespace

core::Result<FarTerrainClipmap> FarTerrainClipmap::create(FarTerrainClipmapConfig config) {
    if (config.level_count == 0 || config.level_count > 16 || config.patches_per_axis < 5 ||
        config.patches_per_axis % 2U == 0 || config.patch_resolution < 2 ||
        config.patch_resolution > 256 || !std::isfinite(config.base_cell_size) ||
        config.base_cell_size <= 0.0 || !std::isfinite(config.maximum_distance) ||
        config.maximum_distance <= 0.0 ||
        !std::isfinite(config.maximum_geometric_error_pixels) ||
        config.maximum_geometric_error_pixels <= 0.0F ||
        !std::isfinite(config.inner_exclusion_radius) ||
        config.inner_exclusion_radius < 0.0) {
        return core::Result<FarTerrainClipmap>::failure(
            "renderer.invalid_far_terrain_clipmap",
            "far-terrain clipmap requires odd patch coverage, finite distances, and valid levels");
    }
    return core::Result<FarTerrainClipmap>::success(FarTerrainClipmap(config));
}

FarTerrainPlan FarTerrainClipmap::plan(math::Vec3d camera_world) const {
    FarTerrainPlan result;
    result.camera_world = camera_world;
    const auto radius = static_cast<std::int64_t>(config_.patches_per_axis / 2U);

    for (std::uint32_t level = 0; level < config_.level_count; ++level) {
        const auto size = patch_size(config_, level);
        const auto cell_size = config_.base_cell_size * std::exp2(static_cast<double>(level));
        const auto center_x = floor_to_i64(camera_world.x / size);
        const auto center_z = floor_to_i64(camera_world.z / size);
        const auto finer_coverage = level == 0 ? math::Bounds3d{}
                                              : level_coverage(camera_world, config_, level - 1U);

        for (std::int64_t z = center_z - radius; z <= center_z + radius; ++z) {
            for (std::int64_t x = center_x - radius; x <= center_x + radius; ++x) {
                math::Bounds3d bounds{{static_cast<double>(x) * size, 0.0,
                                       static_cast<double>(z) * size},
                                      {static_cast<double>(x + 1) * size, 0.0,
                                       static_cast<double>(z + 1) * size}};
                // Keep an overlap band around the finer level. It is consumed by a stable
                // dither/morph transition and prevents hard LOD boundaries from exposing gaps.
                if (level > 0 && fully_inside(bounds, finer_coverage)) {
                    continue;
                }
                const auto patch_center = math::Vec3d{(bounds.min.x + bounds.max.x) * 0.5, 0.0,
                                                       (bounds.min.z + bounds.max.z) * 0.5};
                const auto distance = math::length(patch_center - camera_world);
                const auto nearest_distance = std::max(
                    0.0, distance - size * std::numbers::sqrt2_v<double> * 0.5);
                if (nearest_distance < config_.inner_exclusion_radius) {
                    continue;
                }
                if (distance - size * std::numbers::sqrt2_v<double> * 0.5 >
                    config_.maximum_distance) {
                    continue;
                }
                const auto transition_start = level == 0 ? 0.0F : 0.15F;
                result.patches.push_back(
                    {{level, x, z, config_.domain}, bounds, cell_size,
                     config_.patch_resolution, static_cast<float>(cell_size * 0.5),
                     transition_start, level == 0 ? 0.0F : 0.85F,
                     1.0F / static_cast<float>(1.0 + distance)});
                result.patches.back().finer_coverage = finer_coverage;
                result.covered_radius = std::max(result.covered_radius, distance + size * 0.5);
            }
        }
    }

    std::ranges::sort(result.patches, [](const FarTerrainPatch& left,
                                        const FarTerrainPatch& right) {
        return std::tuple{left.key.level, left.key.z, left.key.x} <
               std::tuple{right.key.level, right.key.z, right.key.x};
    });
    return result;
}

core::Result<FarTerrainPatchMesh> FarTerrainClipmap::build_patch_mesh(
    const FarTerrainPatch& patch, const FarTerrainSurfaceSampler& sampler) const {
    if (!sampler || patch.resolution < 2 || patch.cell_size <= 0.0 ||
        !patch.horizontal_bounds.is_valid()) {
        return core::Result<FarTerrainPatchMesh>::failure(
            "renderer.invalid_far_terrain_patch",
            "far-terrain patch mesh requires valid bounds, resolution, and surface sampler");
    }

    FarTerrainPatchMesh result;
    result.key = patch.key;
    auto origin_height = 0.0;
    bool found_origin_height = false;
    for (std::uint32_t z = 0; z <= patch.resolution && !found_origin_height; ++z) {
        for (std::uint32_t x = 0; x <= patch.resolution; ++x) {
            const auto sample = sampler(
                patch.horizontal_bounds.min.x + static_cast<double>(x) * patch.cell_size,
                patch.horizontal_bounds.min.z + static_cast<double>(z) * patch.cell_size,
                patch.key.domain);
            if (sample.valid) {
                origin_height = sample.height;
                found_origin_height = true;
                break;
            }
        }
    }
    result.world_origin = {patch.horizontal_bounds.min.x, origin_height,
                           patch.horizontal_bounds.min.z};
    const auto row = patch.resolution + 1U;
    result.vertices.reserve(static_cast<std::size_t>(row) * row);
    result.indices.reserve(static_cast<std::size_t>(patch.resolution) * patch.resolution * 6U);
    std::vector<bool> valid_vertices;
    valid_vertices.reserve(static_cast<std::size_t>(row) * row);

    bool has_bounds = false;
    for (std::uint32_t z = 0; z <= patch.resolution; ++z) {
        for (std::uint32_t x = 0; x <= patch.resolution; ++x) {
            const auto world_x = patch.horizontal_bounds.min.x +
                                 static_cast<double>(x) * patch.cell_size;
            const auto world_z = patch.horizontal_bounds.min.z +
                                 static_cast<double>(z) * patch.cell_size;
            const auto sample = sampler(world_x, world_z, patch.key.domain);
            const auto neighbor_height = [&sampler, &sample, domain = patch.key.domain](
                                             double neighbor_x, double neighbor_z) {
                const auto neighbor = sampler(neighbor_x, neighbor_z, domain);
                return neighbor.valid ? neighbor.height : sample.height;
            };
            const auto left = neighbor_height(world_x - patch.cell_size, world_z);
            const auto right = neighbor_height(world_x + patch.cell_size, world_z);
            const auto back = neighbor_height(world_x, world_z - patch.cell_size);
            const auto front = neighbor_height(world_x, world_z + patch.cell_size);
            const auto position = math::Vec3f{
                static_cast<float>(world_x - result.world_origin.x),
                static_cast<float>((sample.valid ? sample.height : origin_height) -
                                   result.world_origin.y),
                static_cast<float>(world_z - result.world_origin.z)};
            float transition = 1.0F;
            if (patch.key.level > 0U) {
                const auto inside = world_x >= patch.finer_coverage.min.x &&
                                    world_x <= patch.finer_coverage.max.x &&
                                    world_z >= patch.finer_coverage.min.z &&
                                    world_z <= patch.finer_coverage.max.z;
                double signed_distance = 0.0;
                if (inside) {
                    signed_distance =
                        -std::min({world_x - patch.finer_coverage.min.x,
                                   patch.finer_coverage.max.x - world_x,
                                   world_z - patch.finer_coverage.min.z,
                                   patch.finer_coverage.max.z - world_z});
                } else {
                    const auto delta_x =
                        std::max({patch.finer_coverage.min.x - world_x, 0.0,
                                  world_x - patch.finer_coverage.max.x});
                    const auto delta_z =
                        std::max({patch.finer_coverage.min.z - world_z, 0.0,
                                  world_z - patch.finer_coverage.max.z});
                    signed_distance = std::hypot(delta_x, delta_z);
                }
                const auto transition_band = std::max(patch.cell_size * 2.0, 1.0);
                transition = static_cast<float>(
                    std::clamp(0.5 + signed_distance / (transition_band * 2.0), 0.0, 1.0));
            }
            result.vertices.push_back(
                {position,
                 normalized({static_cast<float>(left - right),
                             static_cast<float>(2.0 * patch.cell_size),
                             static_cast<float>(back - front)}),
                 {position.x * 0.0625F, position.z * 0.0625F},
                 sample.material, transition});
            valid_vertices.push_back(sample.valid);
            if (sample.valid) {
                const math::Bounds3f point_bounds{position, position};
                result.local_bounds = has_bounds
                                          ? math::Bounds3f{
                                                math::component_min(result.local_bounds.min,
                                                                    position),
                                                math::component_max(result.local_bounds.max,
                                                                    position)}
                                          : point_bounds;
                has_bounds = true;
            }
        }
    }

    for (std::uint32_t z = 0; z < patch.resolution; ++z) {
        for (std::uint32_t x = 0; x < patch.resolution; ++x) {
            const auto top_left = z * row + x;
            const auto top_right = top_left + 1U;
            const auto bottom_left = top_left + row;
            const auto bottom_right = bottom_left + 1U;
            if (!valid_vertices[top_left] || !valid_vertices[top_right] ||
                !valid_vertices[bottom_left] || !valid_vertices[bottom_right]) {
                continue;
            }
            result.indices.insert(result.indices.end(),
                                  {top_left, bottom_left, top_right,
                                   top_right, bottom_left, bottom_right});
        }
    }
    return core::Result<FarTerrainPatchMesh>::success(std::move(result));
}

const FarTerrainClipmapConfig& FarTerrainClipmap::config() const noexcept {
    return config_;
}

} // namespace heartstead::renderer
