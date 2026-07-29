#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/rhi/render_device.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace heartstead::renderer {

enum class VoxelMaterialFlags : std::uint32_t {
    none = 0,
    alpha_tested = 1U << 0U,
    translucent = 1U << 1U,
    emissive = 1U << 2U,
    two_sided = 1U << 3U,
    animated = 1U << 4U,
    unlit = 1U << 5U,
};

[[nodiscard]] constexpr VoxelMaterialFlags operator|(VoxelMaterialFlags left,
                                                     VoxelMaterialFlags right) noexcept {
    return static_cast<VoxelMaterialFlags>(static_cast<std::uint32_t>(left) |
                                           static_cast<std::uint32_t>(right));
}

struct GpuVoxelMaterial {
    std::uint32_t side_texture = 0;
    std::uint32_t top_texture = 0;
    std::uint32_t bottom_texture = 0;
    std::uint32_t flags = 0;
    float base_color[4]{1.0F, 1.0F, 1.0F, 1.0F};
    float emissive_strength = 0.0F;
    float roughness = 1.0F;
    float animation_frame_time = 0.0F;
    float padding = 0.0F;

    friend bool operator==(const GpuVoxelMaterial&, const GpuVoxelMaterial&) = default;
};

static_assert(sizeof(GpuVoxelMaterial) == 48);
static_assert(offsetof(GpuVoxelMaterial, side_texture) == 0);
static_assert(offsetof(GpuVoxelMaterial, flags) == 12);
static_assert(offsetof(GpuVoxelMaterial, base_color) == 16);
static_assert(offsetof(GpuVoxelMaterial, emissive_strength) == 32);
static_assert(offsetof(GpuVoxelMaterial, roughness) == 36);

class BlockRenderTable {
  public:
    BlockRenderTable();

    [[nodiscard]] core::Status set(std::uint16_t voxel_type, GpuVoxelMaterial material);
    [[nodiscard]] const GpuVoxelMaterial& get(std::uint16_t voxel_type) const noexcept;
    [[nodiscard]] std::span<const GpuVoxelMaterial> materials() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;

  private:
    std::vector<GpuVoxelMaterial> materials_;
    std::uint64_t revision_ = 1;
};

struct GpuMaterialTableStats {
    std::size_t material_count = 0;
    std::size_t resident_bytes = 0;
    std::size_t capacity_bytes = 0;
    std::uint64_t resident_revision = 0;
    std::uint64_t upload_count = 0;
    std::uint64_t uploaded_bytes = 0;
};

class GpuMaterialTable {
  public:
    explicit GpuMaterialTable(rhi::IRenderDevice& device);
    ~GpuMaterialTable();

    GpuMaterialTable(const GpuMaterialTable&) = delete;
    GpuMaterialTable& operator=(const GpuMaterialTable&) = delete;

    [[nodiscard]] core::Status synchronize(const BlockRenderTable& table);
    [[nodiscard]] core::Status shutdown();
    [[nodiscard]] rhi::RenderResourceHandle buffer() const noexcept;
    [[nodiscard]] const GpuMaterialTableStats& stats() const noexcept;

  private:
    rhi::IRenderDevice& device_;
    rhi::RenderResourceHandle buffer_;
    std::size_t capacity_bytes_ = 0;
    GpuMaterialTableStats stats_{};
};

} // namespace heartstead::renderer
