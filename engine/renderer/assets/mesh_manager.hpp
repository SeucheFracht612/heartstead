#pragma once

#include "engine/assets/model_asset.hpp"
#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/renderer/assets/render_asset_handles.hpp"
#include "engine/renderer/memory/gpu_buffer_arena.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer {

struct GpuStaticMeshVertex {
    float position[3]{};
    std::int16_t normal[4]{0, 32'767, 0, 0};
    std::int16_t tangent[4]{32'767, 0, 0, 32'767};
    float uv0[2]{};
    float uv1[2]{};
    std::uint16_t joints[4]{};
    std::uint16_t weights[4]{65'535, 0, 0, 0};
    std::uint8_t color[4]{255, 255, 255, 255};

    constexpr GpuStaticMeshVertex() = default;
    constexpr GpuStaticMeshVertex(std::array<float, 3> source_position,
                                  std::array<float, 3> source_normal,
                                  std::array<float, 2> source_uv) {
        for (std::size_t index = 0; index < 3; ++index) {
            position[index] = source_position[index];
            const auto value = source_normal[index] < -1.0F  ? -1.0F
                               : source_normal[index] > 1.0F ? 1.0F
                                                             : source_normal[index];
            normal[index] =
                static_cast<std::int16_t>(value * 32'767.0F + (value >= 0.0F ? 0.5F : -0.5F));
        }
        uv0[0] = source_uv[0];
        uv0[1] = source_uv[1];
    }
};

static_assert(sizeof(GpuStaticMeshVertex) == 64);
static_assert(offsetof(GpuStaticMeshVertex, position) == 0);
static_assert(offsetof(GpuStaticMeshVertex, normal) == 12);
static_assert(offsetof(GpuStaticMeshVertex, tangent) == 20);
static_assert(offsetof(GpuStaticMeshVertex, uv0) == 28);
static_assert(offsetof(GpuStaticMeshVertex, uv1) == 36);
static_assert(offsetof(GpuStaticMeshVertex, joints) == 44);
static_assert(offsetof(GpuStaticMeshVertex, weights) == 52);
static_assert(offsetof(GpuStaticMeshVertex, color) == 60);

inline constexpr rhi::RenderVertexAttributeDesc gpu_static_mesh_vertex_attributes[]{
    {0, offsetof(GpuStaticMeshVertex, position), rhi::RenderVertexAttributeFormat::float3},
    {1, offsetof(GpuStaticMeshVertex, normal), rhi::RenderVertexAttributeFormat::snorm16x4},
    {2, offsetof(GpuStaticMeshVertex, tangent), rhi::RenderVertexAttributeFormat::snorm16x4},
    {3, offsetof(GpuStaticMeshVertex, uv0), rhi::RenderVertexAttributeFormat::float2},
    {4, offsetof(GpuStaticMeshVertex, uv1), rhi::RenderVertexAttributeFormat::float2},
    {5, offsetof(GpuStaticMeshVertex, joints), rhi::RenderVertexAttributeFormat::uint16x4},
    {6, offsetof(GpuStaticMeshVertex, weights), rhi::RenderVertexAttributeFormat::unorm16x4},
    {7, offsetof(GpuStaticMeshVertex, color), rhi::RenderVertexAttributeFormat::unorm8x4},
};

struct GpuMorphDelta {
    float position[4]{};
    float normal[4]{};
    float tangent[4]{};
};

static_assert(sizeof(GpuMorphDelta) == 48);

struct StaticMeshUploadDesc {
    std::string id;
    std::span<const GpuStaticMeshVertex> vertices;
    std::span<const std::uint32_t> indices;
    math::Bounds3f local_bounds{};
};

struct RenderMeshView {
    RenderMeshHandle handle;
    std::string_view id;
    GpuAllocation vertices;
    GpuAllocation indices;
    GpuAllocation morph_deltas;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    std::uint32_t skin_joint_count = 0;
    std::uint32_t morph_target_count = 0;
    std::uint32_t reference_count = 0;
    rhi::RenderIndexType index_type = rhi::RenderIndexType::uint32;
    math::Bounds3f local_bounds{};
    bool fallback = false;
};

struct MeshManagerConfig {
    std::uint64_t vertex_initial_bytes = 4U * 1024U * 1024U;
    std::uint64_t vertex_maximum_bytes = 128U * 1024U * 1024U;
    std::uint64_t index_initial_bytes = 2U * 1024U * 1024U;
    std::uint64_t index_maximum_bytes = 64U * 1024U * 1024U;
    std::uint64_t morph_bytes = 64U * 1024U * 1024U;

    [[nodiscard]] core::Status validate() const;
};

struct MeshManagerStats {
    std::size_t resident_mesh_count = 0;
    std::uint64_t resident_mesh_bytes = 0;
    std::uint64_t uploaded_mesh_count = 0;
    std::uint64_t uploaded_bytes = 0;
    std::uint64_t cache_hit_count = 0;
    std::uint64_t fallback_resolution_count = 0;
    GpuBufferArenaStats vertex_arena;
    GpuBufferArenaStats index_arena;
    GpuBufferArenaStats morph_arena;
};

class MeshManager {
  public:
    explicit MeshManager(rhi::IRenderDevice& device);
    ~MeshManager();

    MeshManager(const MeshManager&) = delete;
    MeshManager& operator=(const MeshManager&) = delete;

    [[nodiscard]] core::Status initialize(MeshManagerConfig config = {});
    [[nodiscard]] core::Result<RenderMeshHandle> create_mesh(const StaticMeshUploadDesc& desc);
    [[nodiscard]] core::Result<RenderMeshHandle>
    create_model_primitive(std::string id, const assets::ModelAsset& model,
                           std::uint32_t primitive_index);
    [[nodiscard]] core::Status release(RenderMeshHandle handle);
    [[nodiscard]] core::Status shutdown();

    // Missing/stale handles resolve visibly. Use find_exact() for ownership/lifetime checks.
    [[nodiscard]] const RenderMeshView* find(RenderMeshHandle handle) noexcept;
    [[nodiscard]] const RenderMeshView* find_exact(RenderMeshHandle handle) const noexcept;
    [[nodiscard]] const RenderMeshView* find(std::string_view id) const noexcept;
    [[nodiscard]] RenderMeshHandle fallback_mesh() const noexcept;
    [[nodiscard]] MeshManagerStats stats() noexcept;
    [[nodiscard]] rhi::RenderResourceHandle morph_delta_buffer() const noexcept;

  private:
    struct Record;

    [[nodiscard]] core::Result<RenderMeshHandle>
    upload_mesh(const StaticMeshUploadDesc& desc, bool fallback, std::uint32_t skin_joint_count = 0,
                std::span<const GpuMorphDelta> morph_deltas = {},
                std::uint32_t morph_target_count = 0);
    [[nodiscard]] Record* find_record(RenderMeshHandle handle) noexcept;
    [[nodiscard]] const Record* find_record(RenderMeshHandle handle) const noexcept;
    [[nodiscard]] Record* find_record(std::string_view id) noexcept;
    [[nodiscard]] core::Result<RenderMeshHandle> retain_record(Record& record);
    [[nodiscard]] core::Status retire_record(Record& record);
    void collect() noexcept;
    void refresh_stats() noexcept;

    rhi::IRenderDevice* device_ = nullptr;
    std::unique_ptr<GpuBufferArena> vertex_arena_;
    std::unique_ptr<GpuBufferArena> index_arena_;
    std::unique_ptr<GpuBufferArena> morph_arena_;
    GpuAllocation morph_sentinel_;
    // RenderMeshView pointers and id views remain valid while their record is resident.
    std::deque<Record> records_;
    RenderMeshHandle fallback_mesh_;
    MeshManagerStats stats_{};
};

[[nodiscard]] core::Status validate_static_mesh_upload(const StaticMeshUploadDesc& desc);

} // namespace heartstead::renderer
