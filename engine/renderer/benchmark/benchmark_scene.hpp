#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"
#include "engine/world/world_state.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace heartstead::world {
class ChunkFluidSystem;
}

namespace heartstead::renderer::benchmark {

enum class BenchmarkSceneKind {
    flat_terrain,
    mountainous_terrain,
    dense_caves,
    checkerboard_geometry,
    forest_cross_planes,
    rapid_voxel_edits,
    high_speed_flythrough,
    chunk_load_unload_churn,
    large_coordinates,
    resize_minimize_stress,
    active_water,
    particle_stress,
    light_heavy_settlement,
    terrain_material_preview,
};

[[nodiscard]] std::string_view benchmark_scene_name(BenchmarkSceneKind kind) noexcept;
[[nodiscard]] std::optional<BenchmarkSceneKind>
parse_benchmark_scene(std::string_view name) noexcept;

struct BenchmarkSceneConfig {
    BenchmarkSceneKind kind = BenchmarkSceneKind::flat_terrain;
    std::uint64_t seed = 0x485354454144ULL;
    std::uint32_t chunk_radius = 2;
    rhi::RenderExtent initial_extent{1280, 720};

    [[nodiscard]] core::Status validate() const;
};

struct BenchmarkSceneStep {
    std::optional<rhi::RenderExtent> requested_extent;
    bool skip_render = false;
};

class BenchmarkScene {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<BenchmarkScene>>
    create(BenchmarkSceneConfig config);

    [[nodiscard]] world::WorldState& world() noexcept;
    [[nodiscard]] const world::WorldState& world() const noexcept;
    [[nodiscard]] world::VoxelPalette& palette() noexcept;
    [[nodiscard]] const world::VoxelPalette& palette() const noexcept;
    [[nodiscard]] RenderCamera& camera() noexcept;
    [[nodiscard]] const RenderCamera& camera() const noexcept;
    [[nodiscard]] BenchmarkSceneKind kind() const noexcept;
    [[nodiscard]] std::uint64_t seed() const noexcept;

    [[nodiscard]] core::Result<BenchmarkSceneStep> advance(std::uint64_t frame_index);
    void activate_fluid_work(world::ChunkFluidSystem& fluids) const;

  private:
    explicit BenchmarkScene(BenchmarkSceneConfig config);

    [[nodiscard]] core::Status initialize();
    [[nodiscard]] core::Status initialize_palette();
    [[nodiscard]] core::Status populate_initial_chunks();
    [[nodiscard]] core::Status insert_generated_chunk(world::ChunkCoord coordinate);
    [[nodiscard]] std::vector<world::VoxelCell> generate_cells(world::ChunkCoord coordinate) const;
    [[nodiscard]] core::Status apply_rapid_edits(std::uint64_t frame_index);
    [[nodiscard]] core::Status apply_chunk_churn(std::uint64_t frame_index);
    void update_flythrough_camera(std::uint64_t frame_index) noexcept;

    BenchmarkSceneConfig config_{};
    world::ChunkCoord center_chunk_{};
    world::WorldState world_{};
    world::VoxelPalette palette_{};
    RenderCamera camera_{};
    std::vector<world::ChunkCoord> managed_chunks_;
    std::size_t churn_cursor_ = 0;
};

} // namespace heartstead::renderer::benchmark
