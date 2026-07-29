#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/renderer/assets/render_asset_handles.hpp"
#include "engine/renderer/assets/texture_manager.hpp"
#include "engine/renderer/materials/block_render_table.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer {

enum class MaterialRuntimeDomain : std::uint8_t {
    surface,
    voxel,
};

struct MaterialRuntimeDesc {
    core::PrototypeId id;
    MaterialRuntimeDomain domain = MaterialRuntimeDomain::voxel;
    std::uint16_t voxel_type = 0;
    std::uint32_t side_texture = 0;
    std::uint32_t top_texture = 0;
    std::uint32_t bottom_texture = 0;
    VoxelMaterialFlags flags = VoxelMaterialFlags::none;
    std::array<float, 4> base_color{1.0F, 1.0F, 1.0F, 1.0F};
    float emissive_strength = 0.0F;
    float roughness = 1.0F;
    float animation_frame_time = 0.0F;

    friend bool operator==(const MaterialRuntimeDesc&, const MaterialRuntimeDesc&) = default;
};

struct MaterialRuntimeView {
    MaterialRuntimeHandle handle;
    core::PrototypeId id;
    MaterialRuntimeDomain domain = MaterialRuntimeDomain::voxel;
    std::uint16_t voxel_type = 0;
    std::uint32_t material_index = 0;
    std::uint64_t revision = 0;
};

struct MaterialRuntimeCacheStats {
    std::size_t resident_material_count = 0;
    std::uint64_t table_revision = 0;
    GpuMaterialTableStats gpu_table{};
};

class MaterialRuntimeCache {
  public:
    explicit MaterialRuntimeCache(rhi::IRenderDevice& device);
    ~MaterialRuntimeCache();

    MaterialRuntimeCache(const MaterialRuntimeCache&) = delete;
    MaterialRuntimeCache& operator=(const MaterialRuntimeCache&) = delete;

    [[nodiscard]] core::Result<MaterialRuntimeHandle> upsert(MaterialRuntimeDesc desc);
    [[nodiscard]] core::Status synchronize_gpu();
    [[nodiscard]] core::Status
    write_gpu_table_descriptor(const core::PrototypeId& pipeline_material,
                               std::string binding_name);
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] const MaterialRuntimeView* find(MaterialRuntimeHandle handle) const noexcept;
    [[nodiscard]] const MaterialRuntimeView* find(const core::PrototypeId& id) const noexcept;
    [[nodiscard]] const BlockRenderTable& block_render_table() const noexcept;
    [[nodiscard]] rhi::RenderResourceHandle gpu_table_buffer() const noexcept;
    [[nodiscard]] const MaterialRuntimeCacheStats& stats() const noexcept;

  private:
    struct Record;

    [[nodiscard]] Record* find_record(MaterialRuntimeHandle handle) noexcept;
    [[nodiscard]] const Record* find_record(MaterialRuntimeHandle handle) const noexcept;
    void update_stats() noexcept;

    rhi::IRenderDevice& device_;
    BlockRenderTable table_;
    GpuMaterialTable gpu_table_;
    std::vector<Record> records_;
    MaterialRuntimeCacheStats stats_{};
};

class TerrainTextureArrayBuilder {
  public:
    TerrainTextureArrayBuilder(std::uint32_t layer_width, std::uint32_t layer_height);

    [[nodiscard]] core::Result<std::uint32_t> add_layer(std::string id,
                                                        std::span<const std::byte> rgba8);
    [[nodiscard]] core::Result<TextureUploadDesc>
    build(std::string texture_id, TextureColorSpace color_space = TextureColorSpace::srgb,
          bool generate_mipmaps = true) const;
    [[nodiscard]] std::size_t layer_count() const noexcept;
    [[nodiscard]] const std::string* layer_id(std::uint32_t layer) const noexcept;

  private:
    struct Layer {
        std::string id;
        std::vector<std::byte> rgba8;
    };

    std::uint32_t layer_width_ = 0;
    std::uint32_t layer_height_ = 0;
    std::vector<Layer> layers_;
};

[[nodiscard]] core::Status validate_material_runtime_desc(const MaterialRuntimeDesc& desc);
[[nodiscard]] GpuVoxelMaterial gpu_voxel_material(const MaterialRuntimeDesc& desc) noexcept;

} // namespace heartstead::renderer
