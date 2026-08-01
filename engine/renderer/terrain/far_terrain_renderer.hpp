#pragma once

#include "engine/renderer/memory/gpu_buffer_arena.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"
#include "engine/renderer/terrain/far_terrain_clipmap.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace heartstead::renderer {

struct FarTerrainGpuVertex {
    math::Vec3f position{};
    math::Vec3f normal{0.0F, 1.0F, 0.0F};
    math::Vec2f uv{};
    std::uint16_t material = 0;
    std::uint16_t padding = 0;
    float transition = 0.0F;
    float transition_padding = 0.0F;
};

static_assert(sizeof(FarTerrainGpuVertex) == 44U);

struct FarTerrainDrawData {
    math::Vec3f camera_relative_origin{};
    std::uint32_t coordinate_key = 0;
};

static_assert(sizeof(FarTerrainDrawData) == 16U);

inline constexpr std::array<rhi::RenderVertexAttributeDesc, 5> far_terrain_vertex_attributes{
    rhi::RenderVertexAttributeDesc{0, 0, rhi::RenderVertexAttributeFormat::float3},
    rhi::RenderVertexAttributeDesc{1, 12, rhi::RenderVertexAttributeFormat::float3},
    rhi::RenderVertexAttributeDesc{2, 24, rhi::RenderVertexAttributeFormat::float2},
    rhi::RenderVertexAttributeDesc{3, 32, rhi::RenderVertexAttributeFormat::uint16},
    rhi::RenderVertexAttributeDesc{4, 36, rhi::RenderVertexAttributeFormat::float2},
};

struct FarTerrainRendererConfig {
    FarTerrainClipmapConfig clipmap{};
    std::uint32_t maximum_patch_builds_per_frame = 4;
    std::size_t maximum_upload_bytes_per_frame = 8U * 1024U * 1024U;
    std::size_t maximum_resident_bytes = 256U * 1024U * 1024U;
};

struct FarTerrainRendererStats {
    std::size_t planned_patches = 0;
    std::size_t resident_patches = 0;
    std::size_t visible_patches = 0;
    std::size_t pending_patches = 0;
    std::size_t built_patches = 0;
    std::size_t evicted_patches = 0;
    std::size_t draw_count = 0;
    std::size_t visible_triangle_count = 0;
    std::size_t resident_bytes = 0;
    std::size_t uploaded_bytes = 0;
};

class FarTerrainRenderer {
  public:
    explicit FarTerrainRenderer(rhi::IRenderDevice& device) noexcept;
    ~FarTerrainRenderer();

    [[nodiscard]] core::Status initialize(FarTerrainRendererConfig config,
                                          rhi::RenderResourceHandle pipeline);
    [[nodiscard]] core::Status set_pipeline(rhi::RenderResourceHandle pipeline) noexcept;
    [[nodiscard]] core::Status update(math::Vec3d camera_world,
                                      const FarTerrainSurfaceSampler& sampler,
                                      std::uint64_t surface_revision = 0,
                                      std::span<const math::Bounds3d> invalidated_regions = {});
    [[nodiscard]] std::vector<rhi::RenderDrawCommand>
    build_draws(const RenderCamera& camera, std::vector<rhi::RenderDrawCommand> reusable = {});
    [[nodiscard]] core::Status clear();
    [[nodiscard]] core::Status shutdown();
    [[nodiscard]] const FarTerrainRendererStats& stats() const noexcept;

  private:
    struct ResidentPatch {
        FarTerrainPatch patch;
        math::Vec3d world_origin{};
        math::Bounds3f local_bounds{};
        GpuAllocation vertex_allocation;
        GpuAllocation index_allocation;
        std::uint32_t index_count = 0;
        std::size_t resident_bytes = 0;
    };

    [[nodiscard]] core::Status upload_patch(const FarTerrainPatch& patch,
                                            const FarTerrainSurfaceSampler& sampler);
    void release_patch(std::map<FarTerrainPatchKey, ResidentPatch>::iterator iterator);
    void enforce_resident_budget();
    void refresh_resident_stats() noexcept;

    rhi::IRenderDevice* device_ = nullptr;
    FarTerrainRendererConfig config_{};
    std::optional<FarTerrainClipmap> clipmap_;
    rhi::RenderResourceHandle pipeline_;
    std::unique_ptr<GpuBufferArena> vertex_arena_;
    std::unique_ptr<GpuBufferArena> index_arena_;
    std::array<rhi::RenderResourceHandle, 4> indirect_buffers_{};
    std::array<rhi::RenderResourceHandle, 4> draw_data_buffers_{};
    std::size_t maximum_draw_count_ = 0;
    std::size_t frame_buffer_index_ = 0;
    std::map<FarTerrainPatchKey, ResidentPatch> resident_;
    FarTerrainPlan plan_;
    FarTerrainRendererStats stats_{};
    std::uint64_t surface_revision_ = 0;
};

} // namespace heartstead::renderer
