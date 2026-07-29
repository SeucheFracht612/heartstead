#include "engine/renderer/materials/terrain_material_assets.hpp"

#include "engine/assets/asset_catalog.hpp"
#include "engine/assets/asset_cooker.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace heartstead::renderer::materials {

namespace {

[[nodiscard]] core::Result<std::uint32_t>
parse_texture_dimension(const assets::CookedAssetPayload& payload, std::string_view field) {
    const auto found = payload.metadata.find(std::string(field));
    if (found == payload.metadata.end() || found->second.empty()) {
        return core::Result<std::uint32_t>::failure(
            "terrain_material_assets.missing_texture_metadata",
            payload.logical_id + ": cooked texture is missing " + std::string(field));
    }
    std::uint32_t parsed = 0;
    const auto* begin = found->second.data();
    const auto* end = begin + found->second.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end || parsed == 0) {
        return core::Result<std::uint32_t>::failure(
            "terrain_material_assets.invalid_texture_metadata",
            payload.logical_id + ": cooked texture has invalid " + std::string(field));
    }
    return core::Result<std::uint32_t>::success(parsed);
}

[[nodiscard]] bool metadata_equals(const assets::CookedAssetPayload& payload,
                                   std::string_view field, std::string_view expected) {
    const auto found = payload.metadata.find(std::string(field));
    return found != payload.metadata.end() && found->second == expected;
}

[[nodiscard]] core::Result<assets::ImageAsset>
image_from_cooked_texture(assets::CookedAssetPayload payload) {
    if (payload.kind != assets::AssetKind::texture || payload.profile != "production" ||
        payload.backend != assets::asset_cook_pipeline_name(
                               assets::AssetKind::texture,
                               assets::AssetCookBackend::production_converters)) {
        return core::Result<assets::ImageAsset>::failure(
            "terrain_material_assets.unsupported_cooked_texture",
            payload.logical_id + ": terrain textures require a production-cooked texture");
    }
    if (!metadata_equals(payload, "texture.runtime_format", "heartstead.texture.rgba8.v1") ||
        !metadata_equals(payload, "texture.container", "rgba8") ||
        !metadata_equals(payload, "texture.channels", "4")) {
        return core::Result<assets::ImageAsset>::failure(
            "terrain_material_assets.unsupported_cooked_texture",
            payload.logical_id + ": terrain textures require a cooked RGBA8 image");
    }

    auto width = parse_texture_dimension(payload, "texture.width");
    if (!width) {
        return core::Result<assets::ImageAsset>::failure(width.error().code,
                                                        width.error().message);
    }
    auto height = parse_texture_dimension(payload, "texture.height");
    if (!height) {
        return core::Result<assets::ImageAsset>::failure(height.error().code,
                                                        height.error().message);
    }
    const auto pixel_count = static_cast<std::uint64_t>(width.value()) * height.value();
    if (pixel_count > std::numeric_limits<std::size_t>::max() / 4U) {
        return core::Result<assets::ImageAsset>::failure(
            "terrain_material_assets.texture_size_mismatch",
            payload.logical_id + ": cooked RGBA8 dimensions exceed the runtime size limit");
    }
    const auto expected_size = static_cast<std::size_t>(pixel_count) * 4U;
    if (payload.bytes.size() != expected_size) {
        return core::Result<assets::ImageAsset>::failure(
            "terrain_material_assets.texture_size_mismatch",
            payload.logical_id + ": cooked RGBA8 byte count does not match its dimensions");
    }

    assets::ImageAsset image;
    image.width = width.value();
    image.height = height.value();
    image.rgba8 = std::move(payload.bytes);
    return core::Result<assets::ImageAsset>::success(std::move(image));
}

[[nodiscard]] std::string terrain_material_id(const world::VoxelDefinition& voxel) {
    return std::string(voxel.prototype_id.namespace_id()) + ":materials/" +
           voxel.terrain_material;
}

[[nodiscard]] const MaterialScalarParameter*
find_scalar(const MaterialDefinition& material, std::string_view name) {
    const auto found =
        std::ranges::find_if(material.scalars, [name](const MaterialScalarParameter& parameter) {
            return parameter.name == name;
        });
    return found == material.scalars.end() ? nullptr : &*found;
}

[[nodiscard]] const MaterialColorParameter*
find_color(const MaterialDefinition& material, std::string_view name) {
    const auto found =
        std::ranges::find_if(material.colors, [name](const MaterialColorParameter& parameter) {
            return parameter.name == name;
        });
    return found == material.colors.end() ? nullptr : &*found;
}

} // namespace

const TerrainVoxelMaterialAsset*
TerrainMaterialAssetSet::find(std::uint16_t voxel_type) const noexcept {
    const auto found =
        std::ranges::find_if(materials, [voxel_type](const TerrainVoxelMaterialAsset& material) {
            return material.voxel_type == voxel_type;
        });
    return found == materials.end() ? nullptr : &*found;
}

core::Status TerrainMaterialAssetSet::validate() const {
    std::unordered_set<std::string> texture_ids;
    for (const auto& texture : textures) {
        if (texture.logical_id.empty() || !texture_ids.insert(texture.logical_id).second) {
            return core::Status::failure(
                "terrain_material_assets.invalid_texture",
                "terrain texture assets require unique, non-empty logical ids");
        }
        const auto pixel_count =
            static_cast<std::uint64_t>(texture.image.width) * texture.image.height;
        if (texture.image.width == 0 || texture.image.height == 0 ||
            pixel_count > std::numeric_limits<std::size_t>::max() / 4U ||
            texture.image.rgba8.size() != static_cast<std::size_t>(pixel_count) * 4U) {
            return core::Status::failure(
                "terrain_material_assets.invalid_texture",
                texture.logical_id + ": terrain texture must contain a complete RGBA8 image");
        }
    }

    std::unordered_set<std::uint16_t> voxel_types;
    const auto valid_texture = [this](std::uint32_t texture) {
        return texture == no_terrain_texture_asset || texture < textures.size();
    };
    for (const auto& material : materials) {
        if (material.voxel_type == world::VoxelDefinition::air_type ||
            !voxel_types.insert(material.voxel_type).second) {
            return core::Status::failure(
                "terrain_material_assets.invalid_material",
                "terrain material assets require unique, non-air voxel types");
        }
        if (!valid_texture(material.side_texture) || !valid_texture(material.top_texture) ||
            !valid_texture(material.bottom_texture)) {
            return core::Status::failure(
                "terrain_material_assets.invalid_texture_reference",
                "terrain material references a texture outside the asset set");
        }
        if (!std::isfinite(material.roughness) || material.roughness < 0.0F ||
            material.roughness > 1.0F ||
            !std::ranges::all_of(material.base_color, [](float value) {
                return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
            })) {
            return core::Status::failure(
                "terrain_material_assets.invalid_parameters",
                "terrain material tint and roughness values must be between zero and one");
        }
    }
    return core::Status::ok();
}

core::Result<TerrainMaterialAssetSet>
load_terrain_material_assets(const world::VoxelPalette& voxel_palette,
                             const MaterialRegistry& material_registry,
                             const assets::CookedAssetStore& cooked_assets) {
    TerrainMaterialAssetSet result;
    std::unordered_map<std::string, std::uint32_t> texture_indices;

    const auto load_texture =
        [&](const MaterialTextureBinding& binding) -> core::Result<std::uint32_t> {
        const auto logical_id = assets::asset_logical_id(binding.texture);
        const auto existing = texture_indices.find(logical_id);
        if (existing != texture_indices.end()) {
            return core::Result<std::uint32_t>::success(existing->second);
        }
        auto payload = cooked_assets.load_payload(logical_id);
        if (!payload) {
            return core::Result<std::uint32_t>::failure(
                payload.error().code,
                logical_id + ": could not load terrain texture: " + payload.error().message);
        }
        auto image = image_from_cooked_texture(std::move(payload).value());
        if (!image) {
            return core::Result<std::uint32_t>::failure(image.error().code,
                                                        image.error().message);
        }
        if (result.textures.size() >= no_terrain_texture_asset) {
            return core::Result<std::uint32_t>::failure(
                "terrain_material_assets.too_many_textures",
                "terrain texture asset index overflow");
        }
        const auto index = static_cast<std::uint32_t>(result.textures.size());
        result.textures.push_back({logical_id, std::move(image).value()});
        texture_indices.emplace(logical_id, index);
        return core::Result<std::uint32_t>::success(index);
    };

    for (const auto* voxel : voxel_palette.definitions()) {
        const auto* definition = material_registry.find(terrain_material_id(*voxel));
        if (definition == nullptr) {
            continue;
        }
        if (definition->domain != MaterialDomain::terrain) {
            return core::Result<TerrainMaterialAssetSet>::failure(
                "terrain_material_assets.wrong_domain",
                definition->id.value() + ": voxel terrain material must use the terrain domain");
        }

        std::optional<std::uint32_t> albedo;
        std::optional<std::uint32_t> side;
        std::optional<std::uint32_t> top;
        std::optional<std::uint32_t> bottom;
        for (const auto& texture : definition->textures) {
            std::optional<std::uint32_t>* destination = nullptr;
            if (texture.name == "albedo") {
                destination = &albedo;
            } else if (texture.name == "side") {
                destination = &side;
            } else if (texture.name == "top") {
                destination = &top;
            } else if (texture.name == "bottom") {
                destination = &bottom;
            } else {
                continue;
            }

            auto loaded = load_texture(texture);
            if (!loaded) {
                if (!texture.required) {
                    continue;
                }
                return core::Result<TerrainMaterialAssetSet>::failure(
                    loaded.error().code,
                    definition->id.value() + ": " + loaded.error().message);
            }
            *destination = loaded.value();
        }

        TerrainVoxelMaterialAsset material;
        material.voxel_type = voxel->type;
        material.side_texture = side.value_or(albedo.value_or(no_terrain_texture_asset));
        material.top_texture = top.value_or(albedo.value_or(no_terrain_texture_asset));
        material.bottom_texture = bottom.value_or(albedo.value_or(no_terrain_texture_asset));
        material.blend_mode = definition->blend_mode;
        material.double_sided = definition->double_sided;
        if (const auto* tint = find_color(*definition, "tint")) {
            material.base_color = {tint->value.red, tint->value.green, tint->value.blue,
                                   tint->value.alpha};
        }
        if (const auto* roughness = find_scalar(*definition, "roughness")) {
            material.roughness = roughness->value;
        }
        result.materials.push_back(std::move(material));
    }

    auto validation = result.validate();
    if (!validation) {
        return core::Result<TerrainMaterialAssetSet>::failure(validation.error().code,
                                                              validation.error().message);
    }
    return core::Result<TerrainMaterialAssetSet>::success(std::move(result));
}

} // namespace heartstead::renderer::materials
