#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"

#include <compare>
#include <cstdint>
#include <functional>
#include <vector>

namespace heartstead::renderer {

enum class FarTerrainDomain : std::uint8_t {
    surface,
    underground,
    aerial,
    ocean,
};

struct FarTerrainClipmapConfig {
    std::uint32_t level_count = 6;
    std::uint32_t patches_per_axis = 7;
    std::uint32_t patch_resolution = 16;
    double base_cell_size = 2.0;
    double maximum_distance = 8'192.0;
    float maximum_geometric_error_pixels = 3.0F;
    FarTerrainDomain domain = FarTerrainDomain::surface;
    // The editable high-detail chunk renderer owns the near field. Keeping the clipmap outside
    // that radius prevents z-fighting while adjacent clipmap rings overlap to hide LOD seams.
    double inner_exclusion_radius = 256.0;
};

struct FarTerrainPatchKey {
    std::uint32_t level = 0;
    std::int64_t x = 0;
    std::int64_t z = 0;
    FarTerrainDomain domain = FarTerrainDomain::surface;

    friend auto operator<=>(const FarTerrainPatchKey&, const FarTerrainPatchKey&) = default;
};

struct FarTerrainPatch {
    FarTerrainPatchKey key;
    math::Bounds3d horizontal_bounds{};
    double cell_size = 0.0;
    std::uint32_t resolution = 0;
    float geometric_error = 0.0F;
    float transition_start = 0.0F;
    float transition_end = 1.0F;
    float streaming_priority = 0.0F;
    math::Bounds3d finer_coverage{};
};

struct FarTerrainPlan {
    math::Vec3d camera_world{};
    std::vector<FarTerrainPatch> patches;
    double covered_radius = 0.0;
};

struct FarTerrainSurfaceSample {
    double height = 0.0;
    std::uint16_t material = 0;
    bool valid = true;
};

using FarTerrainSurfaceSampler =
    std::function<FarTerrainSurfaceSample(double world_x, double world_z,
                                          FarTerrainDomain domain)>;

struct FarTerrainVertex {
    math::Vec3f local_position{};
    math::Vec3f normal{0.0F, 1.0F, 0.0F};
    math::Vec2f uv{};
    std::uint16_t material = 0;
    float transition = 0.0F;
};

struct FarTerrainPatchMesh {
    FarTerrainPatchKey key;
    std::vector<FarTerrainVertex> vertices;
    std::vector<std::uint32_t> indices;
    math::Vec3d world_origin{};
    math::Bounds3f local_bounds{};
};

class FarTerrainClipmap {
  public:
    [[nodiscard]] static core::Result<FarTerrainClipmap>
    create(FarTerrainClipmapConfig config);

    [[nodiscard]] FarTerrainPlan plan(math::Vec3d camera_world) const;
    [[nodiscard]] core::Result<FarTerrainPatchMesh>
    build_patch_mesh(const FarTerrainPatch& patch,
                     const FarTerrainSurfaceSampler& sampler) const;
    [[nodiscard]] const FarTerrainClipmapConfig& config() const noexcept;

  private:
    explicit FarTerrainClipmap(FarTerrainClipmapConfig config) : config_(config) {}

    FarTerrainClipmapConfig config_;
};

} // namespace heartstead::renderer
