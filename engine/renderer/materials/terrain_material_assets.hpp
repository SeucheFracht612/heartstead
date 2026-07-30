#pragma once

#include "engine/assets/cooked_asset_store.hpp"
#include "engine/assets/image_asset.hpp"
#include "engine/core/result.hpp"
#include "engine/renderer/materials/material_definition.hpp"
#include "engine/renderer/materials/voxel_material_faces.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "engine/world/voxels/voxel_surface_state.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace heartstead::renderer::materials {

inline constexpr std::uint32_t no_terrain_texture_asset = std::numeric_limits<std::uint32_t>::max();

struct TerrainTextureAsset {
    std::string logical_id;
    assets::ImageAsset image;
};

struct TerrainSurfaceLayerAsset {
    std::uint32_t texture = no_terrain_texture_asset;
    std::array<float, 4> tint{1.0F, 1.0F, 1.0F, 1.0F};
    float strength = 0.0F;
    float roughness = 1.0F;
    float metallic = 0.0F;
    float emissive_strength = 0.0F;

    friend bool operator==(const TerrainSurfaceLayerAsset&,
                           const TerrainSurfaceLayerAsset&) = default;
};

struct TerrainVoxelMaterialAsset {
    std::uint16_t voxel_type = world::VoxelDefinition::air_type;
    std::array<std::vector<std::uint32_t>, voxel_material_face_count> face_textures;
    // Optional auxiliary maps are aligned with the base-color variants. A single auxiliary
    // texture may be shared by every variant; otherwise its count must match the base-color set.
    std::array<std::vector<std::uint32_t>, voxel_material_face_count> face_normal_textures;
    // R=ambient occlusion, G=roughness, B=metallic, A=height/transition mask.
    std::array<std::vector<std::uint32_t>, voxel_material_face_count> face_surface_textures;
    std::array<float, 4> base_color{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> biome_tint{1.0F, 1.0F, 1.0F, 1.0F};
    float roughness = 1.0F;
    float metallic = 0.0F;
    float ambient_occlusion = 1.0F;
    float emissive_strength = 0.0F;
    float normal_scale = 1.0F;
    float texel_density = 1.0F;
    float biome_tint_strength = 0.0F;
    float macro_color_strength = 0.08F;
    float macro_roughness_strength = 0.08F;
    float transition_width = 0.0F;
    float transition_contrast = 1.0F;
    float transition_noise_scale = 1.0F;
    std::array<TerrainSurfaceLayerAsset, world::voxel_surface_state_count> surface_layers{};
    MaterialBlendMode blend_mode = MaterialBlendMode::opaque;
    bool double_sided = false;
    bool unlit = false;
    bool stable_rotations = true;
    bool stable_mirroring = false;

    [[nodiscard]] const std::vector<std::uint32_t>&
    textures_for(VoxelMaterialFace face) const noexcept {
        return face_textures[voxel_material_face_index(face)];
    }

    [[nodiscard]] const std::vector<std::uint32_t>&
    normal_textures_for(VoxelMaterialFace face) const noexcept {
        return face_normal_textures[voxel_material_face_index(face)];
    }

    [[nodiscard]] const std::vector<std::uint32_t>&
    surface_textures_for(VoxelMaterialFace face) const noexcept {
        return face_surface_textures[voxel_material_face_index(face)];
    }
};

struct TerrainMaterialAssetSet {
    std::vector<TerrainTextureAsset> textures;
    std::vector<TerrainVoxelMaterialAsset> materials;

    [[nodiscard]] const TerrainVoxelMaterialAsset* find(std::uint16_t voxel_type) const noexcept;
    [[nodiscard]] core::Status validate() const;
};

[[nodiscard]] core::Result<TerrainMaterialAssetSet>
load_terrain_material_assets(const world::VoxelPalette& voxel_palette,
                             const MaterialRegistry& material_registry,
                             const assets::CookedAssetStore& cooked_assets);

} // namespace heartstead::renderer::materials
