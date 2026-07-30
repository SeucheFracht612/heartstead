#pragma once

#include "engine/assets/cooked_asset_store.hpp"
#include "engine/assets/image_asset.hpp"
#include "engine/core/result.hpp"
#include "engine/renderer/materials/material_definition.hpp"
#include "engine/renderer/materials/voxel_material_faces.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

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

struct TerrainVoxelMaterialAsset {
    std::uint16_t voxel_type = world::VoxelDefinition::air_type;
    std::array<std::vector<std::uint32_t>, voxel_material_face_count> face_textures;
    std::array<float, 4> base_color{1.0F, 1.0F, 1.0F, 1.0F};
    float roughness = 1.0F;
    MaterialBlendMode blend_mode = MaterialBlendMode::opaque;
    bool double_sided = false;

    [[nodiscard]] const std::vector<std::uint32_t>&
    textures_for(VoxelMaterialFace face) const noexcept {
        return face_textures[voxel_material_face_index(face)];
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
