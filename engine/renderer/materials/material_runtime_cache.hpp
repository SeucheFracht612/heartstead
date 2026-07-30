#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/renderer/assets/render_asset_handles.hpp"
#include "engine/renderer/assets/texture_manager.hpp"
#include "engine/renderer/materials/block_render_table.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer {

enum class MaterialRuntimeDomain : std::uint8_t {
    surface,
    voxel,
};

struct RuntimeSurfaceTextureBinding {
    std::uint32_t texture = 0;
    std::uint32_t sampler_state = 0;
    std::uint32_t texcoord = 0;
    std::array<float, 2> offset{};
    std::array<float, 2> scale{1.0F, 1.0F};
    float rotation = 0.0F;

    friend bool operator==(const RuntimeSurfaceTextureBinding&,
                           const RuntimeSurfaceTextureBinding&) = default;
};

struct RuntimeTerrainSurfaceLayer {
    std::array<float, 4> tint{1.0F, 1.0F, 1.0F, 1.0F};
    float strength = 0.0F;
    float roughness = 1.0F;
    float metallic = 0.0F;
    float emissive_strength = 0.0F;
    std::uint32_t texture_layer = std::numeric_limits<std::uint32_t>::max();

    friend bool operator==(const RuntimeTerrainSurfaceLayer&,
                           const RuntimeTerrainSurfaceLayer&) = default;
};

struct MaterialRuntimeDesc {
    core::PrototypeId id;
    MaterialRuntimeDomain domain = MaterialRuntimeDomain::voxel;
    std::uint16_t voxel_type = 0;
    std::array<std::uint32_t, voxel_material_face_count> face_texture_starts{};
    std::array<std::uint32_t, voxel_material_face_count> face_texture_counts{1, 1, 1, 1, 1, 1};
    std::uint32_t surface_texture = 0;
    RuntimeSurfaceTextureBinding base_color_texture;
    RuntimeSurfaceTextureBinding metallic_roughness_texture;
    RuntimeSurfaceTextureBinding normal_texture;
    RuntimeSurfaceTextureBinding occlusion_texture;
    RuntimeSurfaceTextureBinding emissive_texture;
    VoxelMaterialFlags flags = VoxelMaterialFlags::none;
    std::array<float, 4> base_color{1.0F, 1.0F, 1.0F, 1.0F};
    float alpha_cutoff = 0.5F;
    float emissive_strength = 0.0F;
    float roughness = 1.0F;
    float animation_frame_time = 0.0F;
    std::array<float, 3> emissive_color{};
    float metallic = 1.0F;
    float normal_scale = 1.0F;
    float occlusion_strength = 1.0F;
    float texel_density = 1.0F;
    float biome_tint_strength = 0.0F;
    float macro_color_strength = 0.08F;
    float macro_roughness_strength = 0.08F;
    float transition_width = 0.0F;
    float transition_contrast = 1.0F;
    float transition_noise_scale = 1.0F;
    std::array<float, 4> biome_tint{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<RuntimeTerrainSurfaceLayer, 9> terrain_surface_layers{};

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
    GpuMaterialTableStats surface_gpu_table{};
};

struct GpuSurfaceTextureBinding {
    std::uint32_t metadata[4]{};
    float transform[4]{0.0F, 0.0F, 1.0F, 1.0F};
};

static_assert(sizeof(GpuSurfaceTextureBinding) == 32);

struct GpuSurfaceMaterial {
    GpuSurfaceTextureBinding textures[5]{};
    float base_color[4]{1.0F, 1.0F, 1.0F, 1.0F};
    float emissive_metallic[4]{};
    float roughness_normal_occlusion_alpha[4]{1.0F, 1.0F, 1.0F, 0.5F};
    std::uint32_t flags = 0;
    std::uint32_t padding[3]{};
};

static_assert(sizeof(GpuSurfaceMaterial) == 224);
static_assert(offsetof(GpuSurfaceMaterial, textures) == 0);
static_assert(offsetof(GpuSurfaceMaterial, base_color) == 160);
static_assert(offsetof(GpuSurfaceMaterial, flags) == 208);

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
    [[nodiscard]] core::Status
    write_gpu_surface_table_descriptor(const core::PrototypeId& pipeline_material,
                                       std::string binding_name);
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] const MaterialRuntimeView* find(MaterialRuntimeHandle handle) const noexcept;
    [[nodiscard]] const MaterialRuntimeView* find(const core::PrototypeId& id) const noexcept;
    [[nodiscard]] std::optional<MaterialRuntimeDesc>
    describe(MaterialRuntimeHandle handle) const noexcept;
    [[nodiscard]] const BlockRenderTable& block_render_table() const noexcept;
    [[nodiscard]] rhi::RenderResourceHandle gpu_table_buffer() const noexcept;
    [[nodiscard]] rhi::RenderResourceHandle surface_gpu_table_buffer() const noexcept;
    [[nodiscard]] const MaterialRuntimeCacheStats& stats() const noexcept;

  private:
    struct Record;

    [[nodiscard]] Record* find_record(MaterialRuntimeHandle handle) noexcept;
    [[nodiscard]] const Record* find_record(MaterialRuntimeHandle handle) const noexcept;
    void update_stats() noexcept;

    rhi::IRenderDevice& device_;
    BlockRenderTable table_;
    GpuMaterialTable gpu_table_;
    std::vector<GpuSurfaceMaterial> surface_table_;
    std::uint64_t surface_table_revision_ = 1;
    rhi::RenderResourceHandle surface_gpu_table_buffer_;
    std::size_t surface_gpu_table_capacity_ = 0;
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
[[nodiscard]] GpuSurfaceMaterial gpu_surface_material(const MaterialRuntimeDesc& desc) noexcept;

} // namespace heartstead::renderer
