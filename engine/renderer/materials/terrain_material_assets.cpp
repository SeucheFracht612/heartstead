#include "engine/renderer/materials/terrain_material_assets.hpp"

#include "engine/assets/asset_catalog.hpp"
#include "engine/assets/asset_cooker.hpp"
#include "engine/assets/texture_asset.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace heartstead::renderer::materials {

namespace {

[[nodiscard]] bool metadata_equals(const assets::CookedAssetPayload& payload,
                                   std::string_view field, std::string_view expected) {
    const auto found = payload.metadata.find(std::string(field));
    return found != payload.metadata.end() && found->second == expected;
}

[[nodiscard]] core::Result<assets::ImageAsset>
image_from_cooked_texture(assets::CookedAssetPayload payload) {
    if (payload.kind != assets::AssetKind::texture || payload.profile != "production" ||
        payload.backend !=
            assets::asset_cook_pipeline_name(assets::AssetKind::texture,
                                             assets::AssetCookBackend::production_converters)) {
        return core::Result<assets::ImageAsset>::failure(
            "terrain_material_assets.unsupported_cooked_texture",
            payload.logical_id + ": terrain textures require a production-cooked texture");
    }
    if (!metadata_equals(payload, "texture.runtime_format", "heartstead.texture.v2")) {
        return core::Result<assets::ImageAsset>::failure(
            "terrain_material_assets.unsupported_cooked_texture",
            payload.logical_id + ": terrain textures require a versioned cooked texture");
    }
    auto texture = assets::decode_texture_asset(payload.bytes);
    if (!texture) {
        return core::Result<assets::ImageAsset>::failure(
            texture.error().code, payload.logical_id + ": " + texture.error().message);
    }
    if (texture.value().format != assets::TextureAssetFormat::rgba8) {
        return core::Result<assets::ImageAsset>::failure(
            "terrain_material_assets.compressed_array_source",
            payload.logical_id +
                ": voxel array sources must use compression = rgba8 because the runtime "
                "resamples and repacks their layers");
    }

    assets::ImageAsset image;
    image.width = texture.value().width;
    image.height = texture.value().height;
    image.rgba8 = std::move(texture).value().mips.front().bytes;
    return core::Result<assets::ImageAsset>::success(std::move(image));
}

[[nodiscard]] std::string terrain_material_id(const world::VoxelDefinition& voxel) {
    return std::string(voxel.prototype_id.namespace_id()) + ":materials/" + voxel.terrain_material;
}

[[nodiscard]] const MaterialScalarParameter* find_scalar(const MaterialDefinition& material,
                                                         std::string_view name) {
    const auto found =
        std::ranges::find_if(material.scalars, [name](const MaterialScalarParameter& parameter) {
            return parameter.name == name;
        });
    return found == material.scalars.end() ? nullptr : &*found;
}

[[nodiscard]] const MaterialColorParameter* find_color(const MaterialDefinition& material,
                                                       std::string_view name) {
    const auto found =
        std::ranges::find_if(material.colors, [name](const MaterialColorParameter& parameter) {
            return parameter.name == name;
        });
    return found == material.colors.end() ? nullptr : &*found;
}

enum class TextureFamily : std::uint8_t {
    albedo,
    side,
    west,
    east,
    bottom,
    top,
    north,
    south,
};

inline constexpr std::size_t texture_family_count = 8;

enum class TerrainTextureRole : std::uint8_t {
    base_color,
    normal,
    surface,
};

inline constexpr std::size_t terrain_texture_role_count = 3;

struct ParsedTextureBindingName {
    TerrainTextureRole role = TerrainTextureRole::base_color;
    TextureFamily family = TextureFamily::albedo;
    std::uint32_t variant = 0;
    std::optional<world::VoxelSurfaceState> overlay;
};

struct TextureBindingSet {
    const MaterialTextureBinding* primary = nullptr;
    std::map<std::uint32_t, const MaterialTextureBinding*> variants;
};

[[nodiscard]] constexpr std::size_t texture_family_index(TextureFamily family) noexcept {
    return static_cast<std::size_t>(family);
}

[[nodiscard]] constexpr std::size_t texture_role_index(TerrainTextureRole role) noexcept {
    return static_cast<std::size_t>(role);
}

[[nodiscard]] constexpr std::string_view texture_family_name(TextureFamily family) noexcept {
    switch (family) {
    case TextureFamily::albedo:
        return "albedo";
    case TextureFamily::side:
        return "side";
    case TextureFamily::west:
        return "west";
    case TextureFamily::east:
        return "east";
    case TextureFamily::bottom:
        return "bottom";
    case TextureFamily::top:
        return "top";
    case TextureFamily::north:
        return "north";
    case TextureFamily::south:
        return "south";
    }
    return "unknown";
}

[[nodiscard]] std::optional<TextureFamily> parse_texture_family(std::string_view name) noexcept {
    constexpr std::array families{
        TextureFamily::albedo, TextureFamily::side, TextureFamily::west,  TextureFamily::east,
        TextureFamily::bottom, TextureFamily::top,  TextureFamily::north, TextureFamily::south,
    };
    const auto found = std::ranges::find_if(
        families, [name](TextureFamily family) { return texture_family_name(family) == name; });
    return found == families.end() ? std::nullopt : std::optional<TextureFamily>{*found};
}

[[nodiscard]] core::Result<ParsedTextureBindingName>
parse_texture_binding_name(std::string_view name, std::string_view material_id) {
    constexpr std::string_view overlay_prefix = "overlay.";
    if (name.starts_with(overlay_prefix)) {
        const auto state_name = name.substr(overlay_prefix.size());
        for (const auto state : world::voxel_surface_states()) {
            if (world::voxel_surface_state_name(state) == state_name) {
                ParsedTextureBindingName result;
                result.overlay = state;
                return core::Result<ParsedTextureBindingName>::success(result);
            }
        }
        return core::Result<ParsedTextureBindingName>::failure(
            "terrain_material_assets.invalid_surface_overlay",
            std::string(material_id) + ": unknown terrain surface overlay " +
                std::string(state_name));
    }

    TerrainTextureRole role = TerrainTextureRole::base_color;
    constexpr std::string_view normal_prefix = "normal.";
    constexpr std::string_view surface_prefix = "surface.";
    if (name.starts_with(normal_prefix)) {
        role = TerrainTextureRole::normal;
        name.remove_prefix(normal_prefix.size());
    } else if (name.starts_with(surface_prefix)) {
        role = TerrainTextureRole::surface;
        name.remove_prefix(surface_prefix.size());
    }

    if (const auto family = parse_texture_family(name)) {
        return core::Result<ParsedTextureBindingName>::success({role, *family, 0, std::nullopt});
    }

    constexpr std::string_view variant_marker = ".variant.";
    const auto marker = name.find(variant_marker);
    if (marker == std::string_view::npos) {
        return core::Result<ParsedTextureBindingName>::failure(
            "terrain_material_assets.unsupported_texture_binding",
            std::string(material_id) + ": unsupported terrain texture binding " +
                std::string(name));
    }
    const auto family = parse_texture_family(name.substr(0, marker));
    const auto variant_text = name.substr(marker + variant_marker.size());
    std::uint32_t variant = 0;
    const auto [ptr, error] =
        std::from_chars(variant_text.data(), variant_text.data() + variant_text.size(), variant);
    if (!family || variant_text.empty() || error != std::errc{} ||
        ptr != variant_text.data() + variant_text.size() || variant == 0) {
        return core::Result<ParsedTextureBindingName>::failure(
            "terrain_material_assets.invalid_texture_variant",
            std::string(material_id) + ": terrain texture variants must use " +
                "<face>.variant.<positive integer>");
    }
    return core::Result<ParsedTextureBindingName>::success({role, *family, variant, std::nullopt});
}

[[nodiscard]] constexpr TextureFamily texture_family_for_face(VoxelMaterialFace face) noexcept {
    switch (face) {
    case VoxelMaterialFace::west:
        return TextureFamily::west;
    case VoxelMaterialFace::east:
        return TextureFamily::east;
    case VoxelMaterialFace::bottom:
        return TextureFamily::bottom;
    case VoxelMaterialFace::top:
        return TextureFamily::top;
    case VoxelMaterialFace::north:
        return TextureFamily::north;
    case VoxelMaterialFace::south:
        return TextureFamily::south;
    }
    return TextureFamily::albedo;
}

[[nodiscard]] constexpr bool is_lateral_face(VoxelMaterialFace face) noexcept {
    return face == VoxelMaterialFace::west || face == VoxelMaterialFace::east ||
           face == VoxelMaterialFace::north || face == VoxelMaterialFace::south;
}

[[nodiscard]] TerrainSurfaceLayerAsset
default_surface_layer(world::VoxelSurfaceState state) noexcept {
    TerrainSurfaceLayerAsset layer;
    layer.strength = 1.0F;
    switch (state) {
    case world::VoxelSurfaceState::wetness:
        layer.tint = {0.48F, 0.54F, 0.60F, 0.72F};
        layer.roughness = 0.12F;
        break;
    case world::VoxelSurfaceState::snow:
        layer.tint = {0.92F, 0.96F, 1.0F, 0.95F};
        layer.roughness = 0.78F;
        break;
    case world::VoxelSurfaceState::frost:
        layer.tint = {0.76F, 0.90F, 1.0F, 0.72F};
        layer.roughness = 0.58F;
        break;
    case world::VoxelSurfaceState::mud:
        layer.tint = {0.24F, 0.14F, 0.075F, 0.76F};
        layer.roughness = 0.86F;
        break;
    case world::VoxelSurfaceState::moss:
        layer.tint = {0.20F, 0.38F, 0.12F, 0.68F};
        layer.roughness = 0.92F;
        break;
    case world::VoxelSurfaceState::soot:
        layer.tint = {0.055F, 0.045F, 0.04F, 0.86F};
        layer.roughness = 0.96F;
        break;
    case world::VoxelSurfaceState::heat:
        layer.tint = {1.0F, 0.18F, 0.025F, 0.58F};
        layer.roughness = 0.42F;
        layer.emissive_strength = 2.5F;
        break;
    case world::VoxelSurfaceState::corruption:
        layer.tint = {0.30F, 0.035F, 0.45F, 0.72F};
        layer.roughness = 0.48F;
        layer.emissive_strength = 0.35F;
        break;
    case world::VoxelSurfaceState::magical_residue:
        layer.tint = {0.03F, 0.68F, 0.92F, 0.64F};
        layer.roughness = 0.32F;
        layer.emissive_strength = 1.2F;
        break;
    }
    return layer;
}

[[nodiscard]] std::string surface_parameter_name(world::VoxelSurfaceState state,
                                                 std::string_view parameter) {
    return "surface." + std::string(world::voxel_surface_state_name(state)) + "." +
           std::string(parameter);
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
    const auto valid_texture = [this](std::uint32_t texture) { return texture < textures.size(); };
    for (const auto& material : materials) {
        if (material.voxel_type == world::VoxelDefinition::air_type ||
            !voxel_types.insert(material.voxel_type).second) {
            return core::Status::failure(
                "terrain_material_assets.invalid_material",
                "terrain material assets require unique, non-air voxel types");
        }
        const auto valid_face_texture_set = [&valid_texture](const auto& faces) {
            return std::ranges::all_of(faces, [&valid_texture](const auto& face) {
                return std::ranges::all_of(face, valid_texture);
            });
        };
        if (!valid_face_texture_set(material.face_textures) ||
            !valid_face_texture_set(material.face_normal_textures) ||
            !valid_face_texture_set(material.face_surface_textures)) {
            return core::Status::failure(
                "terrain_material_assets.invalid_texture_reference",
                "terrain material references a texture outside the asset set");
        }
        for (std::size_t face = 0; face < material.face_textures.size(); ++face) {
            const auto variants = material.face_textures[face].size();
            const auto aligned = [variants](const auto& auxiliary) {
                return auxiliary.empty() || auxiliary.size() == 1U || auxiliary.size() == variants;
            };
            if (!aligned(material.face_normal_textures[face]) ||
                !aligned(material.face_surface_textures[face])) {
                return core::Status::failure(
                    "terrain_material_assets.unaligned_auxiliary_variants",
                    "terrain normal and surface maps must have one entry or match the base-color "
                    "variant count");
            }
        }
        if (!std::isfinite(material.roughness) || material.roughness < 0.0F ||
            material.roughness > 1.0F || !std::isfinite(material.metallic) ||
            material.metallic < 0.0F || material.metallic > 1.0F ||
            !std::isfinite(material.ambient_occlusion) || material.ambient_occlusion < 0.0F ||
            material.ambient_occlusion > 1.0F || !std::isfinite(material.emissive_strength) ||
            material.emissive_strength < 0.0F || !std::isfinite(material.normal_scale) ||
            material.normal_scale < 0.0F || !std::isfinite(material.texel_density) ||
            material.texel_density <= 0.0F || !std::isfinite(material.biome_tint_strength) ||
            material.biome_tint_strength < 0.0F || material.biome_tint_strength > 1.0F ||
            !std::isfinite(material.macro_color_strength) || material.macro_color_strength < 0.0F ||
            material.macro_color_strength > 1.0F ||
            !std::isfinite(material.macro_roughness_strength) ||
            material.macro_roughness_strength < 0.0F || material.macro_roughness_strength > 1.0F ||
            !std::isfinite(material.transition_width) || material.transition_width < 0.0F ||
            material.transition_width > 0.5F || !std::isfinite(material.transition_contrast) ||
            material.transition_contrast <= 0.0F ||
            !std::isfinite(material.transition_noise_scale) ||
            material.transition_noise_scale <= 0.0F ||
            !std::ranges::all_of(material.base_color,
                                 [](float value) {
                                     return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
                                 }) ||
            !std::ranges::all_of(material.biome_tint, [](float value) {
                return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
            })) {
            return core::Status::failure(
                "terrain_material_assets.invalid_parameters",
                "terrain material mapping, tint, and PBR parameters are outside valid ranges");
        }
        for (const auto& layer : material.surface_layers) {
            if ((layer.texture != no_terrain_texture_asset && !valid_texture(layer.texture)) ||
                !std::isfinite(layer.strength) || layer.strength < 0.0F || layer.strength > 1.0F ||
                !std::isfinite(layer.roughness) || layer.roughness < 0.0F ||
                layer.roughness > 1.0F || !std::isfinite(layer.metallic) || layer.metallic < 0.0F ||
                layer.metallic > 1.0F || !std::isfinite(layer.emissive_strength) ||
                layer.emissive_strength < 0.0F || !std::ranges::all_of(layer.tint, [](float value) {
                    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
                })) {
                return core::Status::failure(
                    "terrain_material_assets.invalid_surface_layer",
                    "terrain surface-layer parameters are outside valid ranges");
            }
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
            return core::Result<std::uint32_t>::failure(image.error().code, image.error().message);
        }
        if (result.textures.size() >= no_terrain_texture_asset) {
            return core::Result<std::uint32_t>::failure("terrain_material_assets.too_many_textures",
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

        std::array<std::array<TextureBindingSet, texture_family_count>, terrain_texture_role_count>
            binding_sets;
        std::array<const MaterialTextureBinding*, world::voxel_surface_state_count>
            overlay_bindings{};
        for (const auto& texture : definition->textures) {
            auto parsed = parse_texture_binding_name(texture.name, definition->id.value());
            if (!parsed) {
                return core::Result<TerrainMaterialAssetSet>::failure(parsed.error().code,
                                                                      parsed.error().message);
            }
            if (parsed.value().overlay.has_value()) {
                overlay_bindings[static_cast<std::size_t>(*parsed.value().overlay)] = &texture;
                continue;
            }
            auto& set = binding_sets[texture_role_index(parsed.value().role)]
                                    [texture_family_index(parsed.value().family)];
            if (parsed.value().variant == 0) {
                set.primary = &texture;
            } else {
                set.variants.emplace(parsed.value().variant, &texture);
            }
        }

        for (const auto& role_sets : binding_sets) {
            for (std::size_t index = 0; index < role_sets.size(); ++index) {
                const auto& set = role_sets[index];
                if (!set.variants.empty() && set.primary == nullptr) {
                    return core::Result<TerrainMaterialAssetSet>::failure(
                        "terrain_material_assets.variant_without_primary",
                        definition->id.value() + ": " +
                            std::string(texture_family_name(static_cast<TextureFamily>(index))) +
                            " variants require a primary texture binding");
                }
                std::uint32_t expected_variant = 1;
                for (const auto& [variant, _] : set.variants) {
                    if (variant != expected_variant) {
                        return core::Result<TerrainMaterialAssetSet>::failure(
                            "terrain_material_assets.non_contiguous_variants",
                            definition->id.value() + ": " +
                                std::string(
                                    texture_family_name(static_cast<TextureFamily>(index))) +
                                " texture variants must be contiguous from 1");
                    }
                    ++expected_variant;
                }
            }
        }

        std::array<std::array<std::vector<std::uint32_t>, texture_family_count>,
                   terrain_texture_role_count>
            loaded_sets;
        for (std::size_t role = 0; role < binding_sets.size(); ++role) {
            for (std::size_t index = 0; index < binding_sets[role].size(); ++index) {
                const auto& binding_set = binding_sets[role][index];
                if (binding_set.primary == nullptr) {
                    continue;
                }
                auto primary = load_texture(*binding_set.primary);
                if (!primary) {
                    if (!binding_set.primary->required) {
                        continue;
                    }
                    return core::Result<TerrainMaterialAssetSet>::failure(
                        primary.error().code,
                        definition->id.value() + ": " + primary.error().message);
                }
                auto& loaded_set = loaded_sets[role][index];
                loaded_set.push_back(primary.value());
                for (const auto& [_, variant] : binding_set.variants) {
                    auto loaded = load_texture(*variant);
                    if (!loaded) {
                        if (!variant->required) {
                            continue;
                        }
                        return core::Result<TerrainMaterialAssetSet>::failure(
                            loaded.error().code,
                            definition->id.value() + ": " + loaded.error().message);
                    }
                    loaded_set.push_back(loaded.value());
                }
            }
        }

        TerrainVoxelMaterialAsset material;
        material.voxel_type = voxel->type;
        for (const auto state : world::voxel_surface_states()) {
            material.surface_layers[static_cast<std::size_t>(state)] = default_surface_layer(state);
        }
        constexpr std::array faces{
            VoxelMaterialFace::west, VoxelMaterialFace::east,  VoxelMaterialFace::bottom,
            VoxelMaterialFace::top,  VoxelMaterialFace::north, VoxelMaterialFace::south,
        };
        const auto resolve_faces = [&loaded_sets, &faces](TerrainTextureRole role,
                                                          auto& destinations) {
            const auto& role_sets = loaded_sets[texture_role_index(role)];
            for (const auto face : faces) {
                const auto& specific =
                    role_sets[texture_family_index(texture_family_for_face(face))];
                const auto& side = role_sets[texture_family_index(TextureFamily::side)];
                const auto& albedo = role_sets[texture_family_index(TextureFamily::albedo)];
                auto& destination = destinations[voxel_material_face_index(face)];
                if (!specific.empty()) {
                    destination = specific;
                } else if (is_lateral_face(face) && !side.empty()) {
                    destination = side;
                } else {
                    destination = albedo;
                }
            }
        };
        resolve_faces(TerrainTextureRole::base_color, material.face_textures);
        resolve_faces(TerrainTextureRole::normal, material.face_normal_textures);
        resolve_faces(TerrainTextureRole::surface, material.face_surface_textures);
        for (const auto state : world::voxel_surface_states()) {
            const auto index = static_cast<std::size_t>(state);
            if (overlay_bindings[index] != nullptr) {
                auto loaded = load_texture(*overlay_bindings[index]);
                if (!loaded) {
                    if (!overlay_bindings[index]->required) {
                        continue;
                    }
                    return core::Result<TerrainMaterialAssetSet>::failure(
                        loaded.error().code,
                        definition->id.value() + ": " + loaded.error().message);
                }
                material.surface_layers[index].texture = loaded.value();
            }
        }
        material.blend_mode = definition->blend_mode;
        material.double_sided = definition->double_sided;
        if (const auto* tint = find_color(*definition, "tint")) {
            material.base_color = {tint->value.red, tint->value.green, tint->value.blue,
                                   tint->value.alpha};
        }
        if (const auto* tint = find_color(*definition, "biome_tint")) {
            material.biome_tint = {tint->value.red, tint->value.green, tint->value.blue,
                                   tint->value.alpha};
        }
        if (const auto* roughness = find_scalar(*definition, "roughness")) {
            material.roughness = roughness->value;
        }
        if (const auto* metallic = find_scalar(*definition, "metallic")) {
            material.metallic = metallic->value;
        }
        if (const auto* ambient_occlusion = find_scalar(*definition, "ambient_occlusion")) {
            material.ambient_occlusion = ambient_occlusion->value;
        }
        if (const auto* emissive = find_scalar(*definition, "emissive_strength")) {
            material.emissive_strength = emissive->value;
        }
        if (const auto* unlit = find_scalar(*definition, "unlit")) {
            material.unlit = unlit->value >= 0.5F;
        }
        if (const auto* value = find_scalar(*definition, "normal_scale")) {
            material.normal_scale = value->value;
        }
        if (const auto* value = find_scalar(*definition, "texel_density")) {
            material.texel_density = value->value;
        }
        if (const auto* value = find_scalar(*definition, "biome_tint_strength")) {
            material.biome_tint_strength = value->value;
        }
        if (const auto* value = find_scalar(*definition, "macro_color_strength")) {
            material.macro_color_strength = value->value;
        }
        if (const auto* value = find_scalar(*definition, "macro_roughness_strength")) {
            material.macro_roughness_strength = value->value;
        }
        if (const auto* value = find_scalar(*definition, "transition_width")) {
            material.transition_width = value->value;
        }
        if (const auto* value = find_scalar(*definition, "transition_contrast")) {
            material.transition_contrast = value->value;
        }
        if (const auto* value = find_scalar(*definition, "transition_noise_scale")) {
            material.transition_noise_scale = value->value;
        }
        if (const auto* value = find_scalar(*definition, "stable_rotations")) {
            material.stable_rotations = value->value >= 0.5F;
        }
        if (const auto* value = find_scalar(*definition, "stable_mirroring")) {
            material.stable_mirroring = value->value >= 0.5F;
        }
        for (const auto state : world::voxel_surface_states()) {
            auto& layer = material.surface_layers[static_cast<std::size_t>(state)];
            if (const auto* color =
                    find_color(*definition, surface_parameter_name(state, "tint"))) {
                layer.tint = {color->value.red, color->value.green, color->value.blue,
                              color->value.alpha};
            }
            if (const auto* value =
                    find_scalar(*definition, surface_parameter_name(state, "strength"))) {
                layer.strength = value->value;
            }
            if (const auto* value =
                    find_scalar(*definition, surface_parameter_name(state, "roughness"))) {
                layer.roughness = value->value;
            }
            if (const auto* value =
                    find_scalar(*definition, surface_parameter_name(state, "metallic"))) {
                layer.metallic = value->value;
            }
            if (const auto* value =
                    find_scalar(*definition, surface_parameter_name(state, "emissive_strength"))) {
                layer.emissive_strength = value->value;
            }
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
