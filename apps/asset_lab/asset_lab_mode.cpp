#include "apps/asset_lab/asset_lab_mode.hpp"

#include "engine/animation/skeletal_animation.hpp"
#include "engine/assets/model_asset.hpp"
#include "engine/assets/texture_asset.hpp"
#include "engine/core/file_io.hpp"
#include "engine/renderer/materials/material_definition.hpp"
#include "engine/renderer/particles/particle_system.hpp"
#include "engine/world/voxels/voxel_surface_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <sstream>
#include <utility>

namespace heartstead::asset_lab {

namespace {

using renderer::LightingDebugView;

[[nodiscard]] bool model_preview(PreviewKind preview) noexcept {
    return preview == PreviewKind::static_model || preview == PreviewKind::animated_model ||
           preview == PreviewKind::character || preview == PreviewKind::equipment ||
           preview == PreviewKind::vegetation || preview == PreviewKind::lod ||
           preview == PreviewKind::visual_prefab;
}

[[nodiscard]] std::string default_selection(PreviewKind preview) {
    if (preview == PreviewKind::terrain_material || preview == PreviewKind::material) {
        return "base:materials/grass";
    }
    if (preview == PreviewKind::texture) {
        return "base:textures/voxels/grass.png";
    }
    if (preview == PreviewKind::particle) {
        return "base:particles/fire_ember";
    }
    return "base:visuals/player";
}

[[nodiscard]] core::Result<assets::ModelAsset> load_model(const assets::CookedAssetStore& store,
                                                          std::string_view logical_id) {
    auto payload = store.load_payload(logical_id);
    if (!payload) {
        return core::Result<assets::ModelAsset>::failure(payload.error().code,
                                                         payload.error().message);
    }
    if (payload.value().kind != assets::AssetKind::model) {
        return core::Result<assets::ModelAsset>::failure("asset_lab.not_model",
                                                         "selected cooked asset is not a model: " +
                                                             std::string(logical_id));
    }
    return assets::decode_model_asset(payload.value().bytes);
}

[[nodiscard]] std::string count_detail(std::string_view name, std::size_t count) {
    return std::string(name) + "=" + std::to_string(count);
}

[[nodiscard]] std::string resolved_prefab_model(const entities::EntityVisualDefinition& definition,
                                                std::span<const entities::VisualStateValue> states,
                                                std::optional<std::uint32_t> forced_lod) {
    if (const auto* state = definition.resolve_state_rule(states);
        state != nullptr && !state->model_asset.empty()) {
        return state->model_asset;
    }
    if (forced_lod.has_value() && *forced_lod < definition.lods.size()) {
        return definition.lods[*forced_lod].model_asset;
    }
    return definition.model_asset;
}

[[nodiscard]] core::Result<AssetLabInspection>
inspect_model_asset(const AssetLabModeConfig& config, const assets::CookedAssetStore& store,
                    std::string logical_id, std::string selection_kind) {
    auto model = load_model(store, logical_id);
    if (!model) {
        return core::Result<AssetLabInspection>::failure(model.error().code, model.error().message);
    }
    const auto* record = store.manifest().find_active(logical_id);
    AssetLabInspection inspection;
    inspection.selection_id = config.selection_id;
    inspection.selection_kind = std::move(selection_kind);
    inspection.source_path =
        record == nullptr ? "<missing>" : record->source_virtual_path.to_string();
    inspection.cooked_path = record == nullptr
                                 ? "<missing>"
                                 : (store.root() / record->cooked_relative_path).generic_string();
    inspection.format = "heartstead.model.v5";
    inspection.runtime_bytes = model.value().vertices.size() * sizeof(assets::ModelVertex) +
                               model.value().indices.size() * sizeof(std::uint32_t);
    inspection.details = {
        count_detail("vertices", model.value().vertices.size()),
        count_detail("indices", model.value().indices.size()),
        count_detail("primitives", model.value().primitives.size()),
        count_detail("materials", model.value().materials.size()),
        count_detail("images", model.value().images.size()),
        count_detail("nodes", model.value().nodes.size()),
        count_detail("skins", model.value().skins.size()),
        count_detail("animations", model.value().animations.size()),
        count_detail("morph_targets",
                     [&] {
                         std::size_t count = 0;
                         for (const auto& primitive : model.value().primitives) {
                             count += primitive.morph_targets.size();
                         }
                         return count;
                     }()),
        count_detail("sockets", model.value().sockets.size()),
        count_detail("lods", model.value().lods.size()),
        count_detail("collision_shapes", model.value().collision_shapes.size()),
        count_detail("cameras", model.value().cameras.size()),
        count_detail("lights", model.value().lights.size()),
    };
    if (!config.equipment_socket.empty()) {
        const auto socket =
            std::ranges::find_if(model.value().sockets, [&](const assets::ModelSocket& candidate) {
                return candidate.name == config.equipment_socket;
            });
        if (socket == model.value().sockets.end()) {
            return core::Result<AssetLabInspection>::failure(
                "asset_lab.missing_socket",
                "selected model has no named equipment socket: " + config.equipment_socket);
        }
        inspection.details.push_back("equipment_socket=" + config.equipment_socket);
    }
    if (!config.equipment_asset.empty()) {
        auto equipment = load_model(store, config.equipment_asset);
        if (!equipment) {
            return core::Result<AssetLabInspection>::failure(
                equipment.error().code,
                "equipment model could not be inspected: " + equipment.error().message);
        }
        inspection.details.push_back("equipment_model=" + config.equipment_asset);
        inspection.details.push_back(
            count_detail("equipment_primitives", equipment.value().primitives.size()));
    }
    return core::Result<AssetLabInspection>::success(std::move(inspection));
}

[[nodiscard]] const renderer::ParticlePrototype*
find_particle(const content::ContentValidationReport& content, const core::PrototypeId& id) {
    const auto found = std::ranges::find_if(
        content.particle_prototypes,
        [&](const renderer::ParticlePrototype& prototype) { return prototype.id == id; });
    return found == content.particle_prototypes.end() ? nullptr : &*found;
}

[[nodiscard]] core::Result<assets::ImageAsset>
load_source_image(const content::ContentValidationReport& content, std::string_view logical_id) {
    const auto* record = content.asset_catalog.find_active(logical_id);
    if (record == nullptr || record->kind != assets::AssetKind::texture) {
        return core::Result<assets::ImageAsset>::failure(
            "asset_lab.missing_texture_source",
            "texture preview source is absent from the active asset catalog: " +
                std::string(logical_id));
    }
    auto bytes = core::read_binary_file(
        record->source_path, {.maximum_bytes = assets::default_maximum_asset_source_bytes});
    if (!bytes) {
        return core::Result<assets::ImageAsset>::failure(bytes.error().code, bytes.error().message);
    }
    if (record->source_path.extension() == ".ktx2") {
        return assets::decode_ktx2(bytes.value());
    }
    return assets::decode_png_or_jpeg(bytes.value());
}

[[nodiscard]] const renderer::materials::MaterialScalarParameter*
material_scalar(const renderer::materials::MaterialDefinition& material, std::string_view name) {
    const auto found = std::ranges::find_if(
        material.scalars, [name](const auto& value) { return value.name == name; });
    return found == material.scalars.end() ? nullptr : &*found;
}

[[nodiscard]] const renderer::materials::MaterialColorParameter*
material_color(const renderer::materials::MaterialDefinition& material, std::string_view name) {
    const auto found = std::ranges::find_if(
        material.colors, [name](const auto& value) { return value.name == name; });
    return found == material.colors.end() ? nullptr : &*found;
}

[[nodiscard]] core::Result<math::Transform3f> transform_from_matrix(const math::Mat4f& matrix) {
    const auto column_length = [&](std::size_t column) {
        return std::sqrt(matrix.at(0, column) * matrix.at(0, column) +
                         matrix.at(1, column) * matrix.at(1, column) +
                         matrix.at(2, column) * matrix.at(2, column));
    };
    math::Transform3f transform;
    transform.position = {matrix.at(0, 3), matrix.at(1, 3), matrix.at(2, 3)};
    transform.scale = {column_length(0U), column_length(1U), column_length(2U)};
    if (!transform.has_non_zero_scale()) {
        return core::Result<math::Transform3f>::failure(
            "asset_lab.invalid_socket_transform",
            "equipment socket transform contains a zero scale axis");
    }
    const auto r00 = matrix.at(0, 0) / transform.scale.x;
    const auto r10 = matrix.at(1, 0) / transform.scale.x;
    const auto r20 = matrix.at(2, 0) / transform.scale.x;
    const auto r21 = matrix.at(2, 1) / transform.scale.y;
    const auto r22 = matrix.at(2, 2) / transform.scale.z;
    const auto rotation_y = std::asin(std::clamp(-r20, -1.0F, 1.0F));
    const auto cosine_y = std::cos(rotation_y);
    float rotation_x = 0.0F;
    float rotation_z = 0.0F;
    if (std::abs(cosine_y) > 0.00001F) {
        rotation_x = std::atan2(r21, r22);
        rotation_z = std::atan2(r10, r00);
    } else {
        rotation_x = std::atan2(-matrix.at(1, 2), matrix.at(1, 1));
    }
    constexpr auto radians_to_degrees = 180.0F / std::numbers::pi_v<float>;
    transform.rotation_degrees = {rotation_x * radians_to_degrees, rotation_y * radians_to_degrees,
                                  rotation_z * radians_to_degrees};
    if (!transform.is_finite()) {
        return core::Result<math::Transform3f>::failure("asset_lab.invalid_socket_transform",
                                                        "equipment socket transform is not finite");
    }
    return core::Result<math::Transform3f>::success(transform);
}

} // namespace

std::string AssetLabInspection::summary() const {
    std::ostringstream output;
    output << "Asset Lab: " << selection_kind << ' ' << selection_id << " [" << format << "] "
           << runtime_bytes << " runtime bytes";
    for (const auto& detail : details) {
        output << ' ' << detail;
    }
    return output.str();
}

std::optional<PreviewKind> parse_preview_kind(std::string_view value) noexcept {
    constexpr std::array values{
        std::pair{"static-model", PreviewKind::static_model},
        std::pair{"animated-model", PreviewKind::animated_model},
        std::pair{"character", PreviewKind::character},
        std::pair{"equipment", PreviewKind::equipment},
        std::pair{"terrain-material", PreviewKind::terrain_material},
        std::pair{"vegetation", PreviewKind::vegetation},
        std::pair{"particle", PreviewKind::particle},
        std::pair{"material", PreviewKind::material},
        std::pair{"texture", PreviewKind::texture},
        std::pair{"lod", PreviewKind::lod},
        std::pair{"visual-prefab", PreviewKind::visual_prefab},
    };
    const auto found =
        std::ranges::find_if(values, [value](const auto& entry) { return entry.first == value; });
    return found == values.end() ? std::nullopt : std::optional{found->second};
}

std::string_view preview_kind_name(PreviewKind value) noexcept {
    switch (value) {
    case PreviewKind::static_model:
        return "static-model";
    case PreviewKind::animated_model:
        return "animated-model";
    case PreviewKind::character:
        return "character";
    case PreviewKind::equipment:
        return "equipment";
    case PreviewKind::terrain_material:
        return "terrain-material";
    case PreviewKind::vegetation:
        return "vegetation";
    case PreviewKind::particle:
        return "particle";
    case PreviewKind::material:
        return "material";
    case PreviewKind::texture:
        return "texture";
    case PreviewKind::lod:
        return "lod";
    case PreviewKind::visual_prefab:
        return "visual-prefab";
    }
    return "visual-prefab";
}

std::optional<LightingPreset> parse_lighting_preset(std::string_view value) noexcept {
    constexpr std::array values{
        std::pair{"studio", LightingPreset::studio},
        std::pair{"overcast", LightingPreset::overcast},
        std::pair{"noon", LightingPreset::noon},
        std::pair{"sunset", LightingPreset::sunset},
        std::pair{"night", LightingPreset::night},
        std::pair{"fire-lit-interior", LightingPreset::fire_lit_interior},
        std::pair{"cave", LightingPreset::cave},
        std::pair{"forest-shade", LightingPreset::forest_shade},
        std::pair{"rain-wetness", LightingPreset::rain_wetness},
        std::pair{"snow-frost", LightingPreset::snow_frost},
        std::pair{"underwater", LightingPreset::underwater},
    };
    const auto found =
        std::ranges::find_if(values, [value](const auto& entry) { return entry.first == value; });
    return found == values.end() ? std::nullopt : std::optional{found->second};
}

std::string_view lighting_preset_name(LightingPreset value) noexcept {
    switch (value) {
    case LightingPreset::studio:
        return "studio";
    case LightingPreset::overcast:
        return "overcast";
    case LightingPreset::noon:
        return "noon";
    case LightingPreset::sunset:
        return "sunset";
    case LightingPreset::night:
        return "night";
    case LightingPreset::fire_lit_interior:
        return "fire-lit-interior";
    case LightingPreset::cave:
        return "cave";
    case LightingPreset::forest_shade:
        return "forest-shade";
    case LightingPreset::rain_wetness:
        return "rain-wetness";
    case LightingPreset::snow_frost:
        return "snow-frost";
    case LightingPreset::underwater:
        return "underwater";
    }
    return "studio";
}

std::optional<LightingDebugView> parse_asset_lab_debug_view(std::string_view value) noexcept {
    constexpr std::array values{
        std::pair{"none", LightingDebugView::none},
        std::pair{"base-color", LightingDebugView::base_color},
        std::pair{"normals", LightingDebugView::normal},
        std::pair{"roughness", LightingDebugView::roughness},
        std::pair{"metallic", LightingDebugView::metallic},
        std::pair{"ambient-occlusion", LightingDebugView::ambient_occlusion},
        std::pair{"emissive", LightingDebugView::emissive},
        std::pair{"shadow-cascades", LightingDebugView::shadow_cascades},
        std::pair{"local-light-tiles", LightingDebugView::local_light_tiles},
        std::pair{"uv0", LightingDebugView::uv0},
        std::pair{"uv1", LightingDebugView::uv1},
        std::pair{"tangents", LightingDebugView::tangents},
        std::pair{"vertex-colors", LightingDebugView::vertex_colors},
        std::pair{"mip-level", LightingDebugView::mip_level},
        std::pair{"texel-density", LightingDebugView::texel_density},
        std::pair{"texture-residency", LightingDebugView::texture_residency},
        std::pair{"lod", LightingDebugView::lod},
        std::pair{"bounds", LightingDebugView::bounds},
        std::pair{"skeletons", LightingDebugView::skeletons},
        std::pair{"skin-weights", LightingDebugView::skin_weights},
        std::pair{"overdraw", LightingDebugView::overdraw},
    };
    const auto found =
        std::ranges::find_if(values, [value](const auto& entry) { return entry.first == value; });
    return found == values.end() ? std::nullopt : std::optional{found->second};
}

std::string_view asset_lab_debug_view_name(LightingDebugView value) noexcept {
    constexpr std::array names{
        std::string_view{"none"},
        std::string_view{"base-color"},
        std::string_view{"normals"},
        std::string_view{"roughness"},
        std::string_view{"metallic"},
        std::string_view{"ambient-occlusion"},
        std::string_view{"emissive"},
        std::string_view{"shadow-cascades"},
        std::string_view{"local-light-tiles"},
        std::string_view{"uv0"},
        std::string_view{"uv1"},
        std::string_view{"tangents"},
        std::string_view{"vertex-colors"},
        std::string_view{"mip-level"},
        std::string_view{"texel-density"},
        std::string_view{"texture-residency"},
        std::string_view{"lod"},
        std::string_view{"bounds"},
        std::string_view{"skeletons"},
        std::string_view{"skin-weights"},
        std::string_view{"overdraw"},
    };
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : std::string_view{"none"};
}

renderer::rhi::RenderEnvironmentData
asset_lab_lighting_environment(LightingPreset preset) noexcept {
    renderer::rhi::RenderEnvironmentData environment;
    environment.fog_start = 1'000.0F;
    environment.fog_end = 2'000.0F;
    switch (preset) {
    case LightingPreset::studio:
        environment.sun_direction = {-0.42F, 0.78F, 0.46F};
        environment.sun_intensity = 2.4F;
        environment.ambient_color = {0.55F, 0.58F, 0.65F};
        environment.sky_diffuse_intensity = 0.9F;
        environment.environment_specular_intensity = 1.2F;
        break;
    case LightingPreset::overcast:
        environment.sun_intensity = 0.35F;
        environment.ambient_color = {0.62F, 0.68F, 0.76F};
        environment.sky_diffuse_intensity = 1.35F;
        environment.environment_specular_intensity = 0.65F;
        break;
    case LightingPreset::noon:
        environment.sun_direction = {0.1F, 0.98F, 0.05F};
        environment.sun_intensity = 4.0F;
        environment.ambient_color = {0.7F, 0.78F, 0.9F};
        environment.sky_diffuse_intensity = 1.1F;
        break;
    case LightingPreset::sunset:
        environment.sun_direction = {-0.8F, 0.18F, 0.2F};
        environment.sun_intensity = 2.2F;
        environment.ambient_color = {0.42F, 0.3F, 0.5F};
        environment.fog_color = {0.38F, 0.12F, 0.08F};
        environment.environment_rotation_radians = 1.2F;
        break;
    case LightingPreset::night:
        environment.sun_intensity = 0.04F;
        environment.ambient_color = {0.05F, 0.08F, 0.18F};
        environment.sky_diffuse_intensity = 0.18F;
        environment.environment_specular_intensity = 0.35F;
        break;
    case LightingPreset::fire_lit_interior:
        environment.sun_intensity = 0.0F;
        environment.ambient_color = {0.18F, 0.055F, 0.018F};
        environment.sky_diffuse_intensity = 0.08F;
        environment.environment_specular_intensity = 0.2F;
        break;
    case LightingPreset::cave:
        environment.sun_intensity = 0.0F;
        environment.ambient_color = {0.018F, 0.024F, 0.032F};
        environment.sky_diffuse_intensity = 0.015F;
        environment.environment_specular_intensity = 0.04F;
        break;
    case LightingPreset::forest_shade:
        environment.sun_intensity = 0.55F;
        environment.ambient_color = {0.2F, 0.34F, 0.22F};
        environment.sky_diffuse_intensity = 0.55F;
        break;
    case LightingPreset::rain_wetness:
        environment.sun_intensity = 0.18F;
        environment.ambient_color = {0.36F, 0.43F, 0.5F};
        environment.sky_diffuse_intensity = 0.9F;
        environment.environment_specular_intensity = 1.5F;
        break;
    case LightingPreset::snow_frost:
        environment.sun_intensity = 1.8F;
        environment.ambient_color = {0.78F, 0.84F, 0.95F};
        environment.sky_diffuse_intensity = 1.4F;
        environment.environment_specular_intensity = 1.1F;
        break;
    case LightingPreset::underwater:
        environment.sun_intensity = 0.2F;
        environment.ambient_color = {0.04F, 0.28F, 0.34F};
        environment.fog_color = {0.015F, 0.16F, 0.2F};
        environment.fog_start = 8.0F;
        environment.fog_end = 38.0F;
        environment.sky_diffuse_intensity = 0.35F;
        environment.environment_specular_intensity = 0.25F;
        break;
    }
    return environment;
}

core::Result<AssetLabInspection>
inspect_asset_lab_selection(const AssetLabModeConfig& config,
                            const assets::CookedAssetStore& cooked_assets) {
    if (config.content == nullptr) {
        return core::Result<AssetLabInspection>::failure(
            "asset_lab.missing_content", "Asset Lab requires validated project content");
    }
    if (model_preview(config.preview)) {
        const auto visual_id = core::PrototypeId::parse(config.selection_id);
        if (visual_id) {
            if (const auto* definition = config.content->visual_definitions.find(*visual_id);
                definition != nullptr) {
                if (config.forced_lod.has_value() &&
                    *config.forced_lod >= definition->lods.size()) {
                    return core::Result<AssetLabInspection>::failure(
                        "asset_lab.invalid_lod", "forced LOD is outside the visual prefab chain");
                }
                auto inspected = inspect_model_asset(
                    config, cooked_assets,
                    resolved_prefab_model(*definition, config.visual_states, config.forced_lod),
                    "visual-prefab");
                if (inspected) {
                    inspected.value().details.push_back(
                        count_detail("prefab_states", definition->state_rules.size()));
                    inspected.value().details.push_back(
                        count_detail("prefab_anchors", definition->anchors.size()));
                    inspected.value().details.push_back(
                        count_detail("prefab_lods", definition->lods.size()));
                }
                return inspected;
            }
        }
        return inspect_model_asset(config, cooked_assets, config.selection_id, "model");
    }
    if (config.preview == PreviewKind::texture) {
        auto payload = cooked_assets.load_payload(config.selection_id);
        if (!payload) {
            return core::Result<AssetLabInspection>::failure(payload.error().code,
                                                             payload.error().message);
        }
        if (payload.value().kind != assets::AssetKind::texture) {
            return core::Result<AssetLabInspection>::failure(
                "asset_lab.not_texture", "texture preview requires a cooked texture asset");
        }
        auto texture = assets::decode_texture_asset(payload.value().bytes);
        if (!texture) {
            return core::Result<AssetLabInspection>::failure(texture.error().code,
                                                             texture.error().message);
        }
        const auto* record = cooked_assets.manifest().find_active(config.selection_id);
        AssetLabInspection inspection;
        inspection.selection_id = config.selection_id;
        inspection.selection_kind = "texture";
        inspection.source_path =
            record == nullptr ? "<missing>" : record->source_virtual_path.to_string();
        inspection.cooked_path =
            record == nullptr
                ? "<missing>"
                : (cooked_assets.root() / record->cooked_relative_path).generic_string();
        inspection.format = "heartstead.texture.v2/" +
                            std::string(assets::texture_asset_format_name(texture.value().format));
        inspection.runtime_bytes = texture.value().gpu_memory_bytes();
        inspection.details = {
            "dimensions=" + std::to_string(texture.value().width) + "x" +
                std::to_string(texture.value().height),
            count_detail("mips", texture.value().mips.size()),
            "role=" + std::string(assets::texture_role_name(texture.value().role)),
            "color_space=" +
                std::string(assets::texture_color_space_name(texture.value().color_space)),
            std::string("alpha_coverage=") +
                (texture.value().alpha_coverage_preserved ? "preserved" : "none"),
        };
        return core::Result<AssetLabInspection>::success(std::move(inspection));
    }
    const auto prototype_id = core::PrototypeId::parse(config.selection_id);
    if (!prototype_id) {
        return core::Result<AssetLabInspection>::failure(
            "asset_lab.invalid_prototype", "preview selection must be a prototype id");
    }
    const auto* prototype = config.content->registry.find(*prototype_id);
    if (prototype == nullptr) {
        return core::Result<AssetLabInspection>::failure("asset_lab.missing_prototype",
                                                         "preview prototype does not exist");
    }
    const auto expected_kind = config.preview == PreviewKind::particle
                                   ? modding::PrototypeKinds::particle
                                   : modding::PrototypeKinds::material;
    if (prototype->kind != expected_kind) {
        return core::Result<AssetLabInspection>::failure("asset_lab.prototype_kind_mismatch",
                                                         "preview selection has kind '" +
                                                             prototype->kind + "', expected '" +
                                                             std::string(expected_kind) + "'");
    }
    AssetLabInspection inspection;
    inspection.selection_id = config.selection_id;
    inspection.selection_kind = prototype->kind;
    inspection.source_path = prototype->source.generic_string();
    inspection.cooked_path = "<prototype-data>";
    inspection.format = "heartstead.prototype";
    inspection.details.push_back(count_detail("fields", prototype->fields.size()));
    return core::Result<AssetLabInspection>::success(std::move(inspection));
}

AssetLabMode::AssetLabMode(AssetLabModeConfig config) : config_(std::move(config)) {
    if (config_.selection_id.empty()) {
        config_.selection_id = default_selection(config_.preview);
    }
}

core::Status AssetLabMode::initialize(game::GameApplicationServices& services) {
    auto store = assets::CookedAssetStore::load(config_.cooked_asset_root);
    if (!store) {
        return core::Status::failure(store.error().code, store.error().message);
    }
    cooked_assets_.emplace(std::move(store).value());
    if (config_.use_prefab_preview_settings) {
        if (const auto visual_id = core::PrototypeId::parse(config_.selection_id)) {
            if (const auto* definition = config_.content->visual_definitions.find(*visual_id);
                definition != nullptr) {
                auto lighting_name = definition->preview.lighting_preset;
                std::ranges::replace(lighting_name, '_', '-');
                if (const auto lighting = parse_lighting_preset(lighting_name)) {
                    config_.lighting = *lighting;
                }
                preview_camera_distance_ = definition->preview.camera_distance;
                if (config_.visual_states.empty()) {
                    for (const auto& [channel, value] : definition->preview.states) {
                        config_.visual_states.push_back({channel, value});
                    }
                    std::ranges::sort(config_.visual_states,
                                      [](const auto& left, const auto& right) {
                                          return left.channel < right.channel;
                                      });
                }
            }
        }
    }
    auto inspected = inspect_asset_lab_selection(config_, *cooked_assets_);
    if (!inspected) {
        return core::Status::failure(inspected.error().code, inspected.error().message);
    }
    inspection_.emplace(std::move(inspected).value());
    if (services.headless()) {
        return core::Status::ok();
    }
    auto* active_renderer = services.renderer();
    if (active_renderer == nullptr) {
        return core::Status::failure("asset_lab.renderer_missing",
                                     "native Asset Lab requires the game renderer");
    }
    auto status =
        active_renderer->set_environment(asset_lab_lighting_environment(config_.lighting));
    if (!status) {
        return status;
    }
    status = active_renderer->set_lighting_debug_view(config_.debug_view);
    if (!status) {
        return status;
    }
    camera_.local_position = {0.0F, 1.2F, 0.0F};
    camera_.yaw_radians = 0.0F;
    camera_.pitch_radians = -0.0872665F;
    status = camera_.set_aspect_ratio(16.0F / 9.0F);
    if (!status) {
        return status;
    }
    if (config_.preview == PreviewKind::particle) {
        return initialize_particle_preview(*active_renderer);
    }
    if (config_.preview == PreviewKind::texture) {
        return initialize_texture_preview(*active_renderer);
    }
    if (config_.preview == PreviewKind::terrain_material) {
        return initialize_terrain_preview(*active_renderer);
    }
    if (config_.preview == PreviewKind::material) {
        return initialize_material_preview(*active_renderer);
    }
    return initialize_model_preview(*active_renderer);
}

core::Status AssetLabMode::initialize_model_preview(renderer::Renderer& active_renderer) {
    const auto fallback_id = core::PrototypeId::parse("base:visuals/fallback");
    const auto* fallback =
        fallback_id ? config_.content->visual_definitions.find(*fallback_id) : nullptr;
    if (fallback == nullptr) {
        return core::Status::failure("asset_lab.missing_fallback",
                                     "Asset Lab requires the fallback visual prefab");
    }
    auto status = preview_visuals_.add(*fallback);
    if (!status) {
        return status;
    }
    const entities::EntityVisualDefinition* selected = nullptr;
    if (!model_preview(config_.preview)) {
        const auto showcase = core::PrototypeId::parse("base:visuals/foundation_material_showcase");
        selected = showcase ? config_.content->visual_definitions.find(*showcase) : nullptr;
    } else if (const auto selection_id = core::PrototypeId::parse(config_.selection_id)) {
        selected = config_.content->visual_definitions.find(*selection_id);
    }
    entities::EntityVisualDefinition generated;
    if (selected == nullptr) {
        generated = *fallback;
        generated.id = *core::PrototypeId::parse("asset_lab:visuals/preview");
        generated.entity_prototype = *core::PrototypeId::parse("asset_lab:entities/preview");
        generated.model_asset = config_.selection_id;
        generated.lods = {{0U, config_.selection_id, 0.0F, 0.0F}};
        generated.animation_clips.clear();
        generated.sound_events.clear();
        generated.state_rules.clear();
        generated.anchors.clear();
        generated.socket_aliases.clear();
        selected = &generated;
    }
    if (config_.forced_lod.has_value() && selected->lods.size() > 1U) {
        generated = *selected;
        const auto selected_model =
            resolved_prefab_model(generated, config_.visual_states, config_.forced_lod);
        generated.model_asset = selected_model;
        generated.lods = {{0U, selected_model, 0.0F, 0.0F}};
        selected = &generated;
    }
    const auto inspected_model_id =
        resolved_prefab_model(*selected, config_.visual_states, std::nullopt);
    auto inspected_model = load_model(*cooked_assets_, inspected_model_id);
    if (!inspected_model) {
        return core::Status::failure(inspected_model.error().code, inspected_model.error().message);
    }
    inspected_model_.emplace(std::move(inspected_model).value());
    auto bind_pose = animation::bind_node_pose(*inspected_model_);
    auto node_matrices = animation::evaluate_model_node_matrices(*inspected_model_, bind_pose);
    if (!node_matrices) {
        return core::Status::failure(node_matrices.error().code, node_matrices.error().message);
    }
    inspected_node_matrices_ = std::move(node_matrices).value();
    if (selected->id != fallback->id) {
        status = preview_visuals_.add(*selected);
        if (!status) {
            return status;
        }
    }
    if (!config_.equipment_asset.empty()) {
        const auto socket = std::ranges::find_if(
            inspected_model_->sockets, [&](const assets::ModelSocket& candidate) {
                return candidate.name == config_.equipment_socket;
            });
        if (socket == inspected_model_->sockets.end()) {
            return core::Status::failure("asset_lab.missing_socket",
                                         "equipment preview socket disappeared after inspection");
        }
        const auto socket_matrix = math::scale_matrix({selected->model_scale, selected->model_scale,
                                                       selected->model_scale}) *
                                   inspected_node_matrices_[socket->node];
        auto socket_transform = transform_from_matrix(socket_matrix);
        if (!socket_transform) {
            return core::Status::failure(socket_transform.error().code,
                                         socket_transform.error().message);
        }
        equipment_socket_transform_ = socket_transform.value();
        auto equipment_visual = *fallback;
        equipment_visual.id = *core::PrototypeId::parse("asset_lab:visuals/equipment");
        equipment_visual.entity_prototype =
            *core::PrototypeId::parse("asset_lab:entities/equipment");
        equipment_visual.model_asset = config_.equipment_asset;
        equipment_visual.lods = {{0U, config_.equipment_asset, 0.0F, 0.0F}};
        equipment_visual.animation_clips.clear();
        equipment_visual.sound_events.clear();
        equipment_visual.state_rules.clear();
        equipment_visual.anchors.clear();
        equipment_visual.socket_aliases.clear();
        status = preview_visuals_.add(equipment_visual);
        if (!status) {
            return status;
        }
        equipment_entity_ = equipment_visual.entity_prototype;
    }
    preview_entity_ = selected->entity_prototype;
    game::ModelPresentationSystemConfig model_presentation_config;
    model_presentation_config.material_registry = &config_.content->material_registry;
    status = models_.initialize(active_renderer, preview_visuals_, config_.cooked_asset_root,
                                model_presentation_config);
    if (!status) {
        return status;
    }
    models_initialized_ = true;

    game::RenderObjectSnapshot object;
    object.id = game::PresentationObjectId::from_parts(1U, 1U);
    object.source_net_id = core::NetId::from_value(1U);
    object.visual_prototype = preview_entity_;
    object.current_transform.position =
        world::WorldPosition{0.0, 0.0, -static_cast<double>(preview_camera_distance_)};
    object.previous_transform = object.current_transform;
    object.visual_states = config_.visual_states;
    object.source_revision = 1U;
    snapshot_.objects.push_back(std::move(object));
    if (equipment_entity_.is_valid()) {
        game::RenderObjectSnapshot equipment;
        equipment.id = game::PresentationObjectId::from_parts(2U, 1U);
        equipment.source_net_id = core::NetId::from_value(2U);
        equipment.visual_prototype = equipment_entity_;
        equipment.current_transform.position =
            world::WorldPosition{static_cast<double>(equipment_socket_transform_.position.x),
                                 static_cast<double>(equipment_socket_transform_.position.y),
                                 -static_cast<double>(preview_camera_distance_) +
                                     static_cast<double>(equipment_socket_transform_.position.z)};
        equipment.current_transform.rotation_degrees = {
            static_cast<double>(equipment_socket_transform_.rotation_degrees.x),
            static_cast<double>(equipment_socket_transform_.rotation_degrees.y),
            static_cast<double>(equipment_socket_transform_.rotation_degrees.z)};
        equipment.current_transform.scale = {
            static_cast<double>(equipment_socket_transform_.scale.x),
            static_cast<double>(equipment_socket_transform_.scale.y),
            static_cast<double>(equipment_socket_transform_.scale.z)};
        equipment.previous_transform = equipment.current_transform;
        equipment.source_revision = 1U;
        snapshot_.objects.push_back(std::move(equipment));
    }
    snapshot_.simulation_tick = 1U;
    auto synchronized = models_.synchronize(active_renderer, snapshot_);
    if (!synchronized) {
        return core::Status::failure(synchronized.error().code, synchronized.error().message);
    }
    return core::Status::ok();
}

core::Status AssetLabMode::initialize_image_quad(renderer::Renderer& active_renderer,
                                                 const assets::ImageAsset& image,
                                                 renderer::MaterialRuntimeDesc material) {
    auto texture = active_renderer.create_surface_texture("asset_lab:" + config_.selection_id,
                                                          image.width, image.height, image.rgba8);
    if (!texture) {
        return core::Status::failure(texture.error().code, texture.error().message);
    }
    material.domain = renderer::MaterialRuntimeDomain::surface;
    material.surface_texture = texture.value();
    material.base_color_texture.texture = texture.value();
    material.metallic_roughness_texture.texture = 1U;
    material.normal_texture.texture = 2U;
    material.occlusion_texture.texture = 1U;
    material.emissive_texture.texture = 3U;
    auto runtime_material = active_renderer.create_surface_material(std::move(material));
    if (!runtime_material) {
        return core::Status::failure(runtime_material.error().code,
                                     runtime_material.error().message);
    }

    constexpr std::array<renderer::GpuStaticMeshVertex, 4> vertices{{
        {{-1.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F}},
        {{1.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 1.0F}},
        {{1.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F}},
        {{-1.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F}},
    }};
    constexpr std::array<std::uint32_t, 6> indices{0U, 1U, 2U, 0U, 2U, 3U};
    auto mesh = active_renderer.create_static_mesh(
        {"asset_lab:preview_quad", vertices, indices, {{-1.0F, -1.0F, 0.0F}, {1.0F, 1.0F, 0.0F}}});
    if (!mesh) {
        return core::Status::failure(mesh.error().code, mesh.error().message);
    }
    direct_preview_mesh_ = mesh.value();
    renderer::RenderObjectProxy object;
    object.anchor = world::WorldPosition{0.0, 0.0, 0.0};
    object.previous_transform.position = {0.0F, 1.2F, -preview_camera_distance_};
    object.current_transform = object.previous_transform;
    const auto aspect = static_cast<float>(image.width) / static_cast<float>(image.height);
    object.previous_transform.scale.x = std::clamp(aspect, 0.25F, 4.0F);
    object.current_transform.scale = object.previous_transform.scale;
    object.mesh = direct_preview_mesh_;
    object.material = runtime_material.value();
    object.local_bounds = {{-1.0F, -1.0F, 0.0F}, {1.0F, 1.0F, 0.0F}};
    object.flags = renderer::RenderObjectFlags::two_sided;
    auto created = active_renderer.create_object(std::move(object));
    if (!created) {
        return core::Status::failure(created.error().code, created.error().message);
    }
    direct_preview_object_ = created.value();
    return core::Status::ok();
}

core::Status AssetLabMode::initialize_texture_preview(renderer::Renderer& active_renderer) {
    auto image = load_source_image(*config_.content, config_.selection_id);
    if (!image) {
        return core::Status::failure(image.error().code, image.error().message);
    }
    renderer::MaterialRuntimeDesc material;
    material.id = *core::PrototypeId::parse("asset_lab:materials/texture_preview");
    material.flags = renderer::VoxelMaterialFlags::unlit | renderer::VoxelMaterialFlags::two_sided;
    material.roughness = 1.0F;
    material.metallic = 0.0F;
    return initialize_image_quad(active_renderer, image.value(), std::move(material));
}

core::Status AssetLabMode::initialize_material_preview(renderer::Renderer& active_renderer) {
    const auto* definition = config_.content->material_registry.find(config_.selection_id);
    if (definition == nullptr) {
        return core::Status::failure("asset_lab.missing_material",
                                     "selected material is absent from the registry");
    }
    if (definition->domain == renderer::materials::MaterialDomain::terrain ||
        definition->domain == renderer::materials::MaterialDomain::foliage ||
        definition->domain == renderer::materials::MaterialDomain::water) {
        return initialize_terrain_preview(active_renderer);
    }
    assets::ImageAsset image;
    image.width = 1U;
    image.height = 1U;
    image.rgba8 = {255U, 255U, 255U, 255U};
    if (const auto albedo = std::ranges::find_if(definition->textures,
                                                 [](const auto& binding) {
                                                     return binding.name == "albedo" ||
                                                            binding.name == "base_color";
                                                 });
        albedo != definition->textures.end()) {
        auto loaded = load_source_image(*config_.content, albedo->texture.to_string());
        if (!loaded) {
            return core::Status::failure(loaded.error().code, loaded.error().message);
        }
        image = std::move(loaded).value();
    }
    renderer::MaterialRuntimeDesc material;
    material.id = *core::PrototypeId::parse("asset_lab:materials/material_preview");
    if (const auto* tint = material_color(*definition, "tint"); tint != nullptr) {
        material.base_color = {tint->value.red, tint->value.green, tint->value.blue,
                               tint->value.alpha};
    }
    if (const auto* value = material_scalar(*definition, "roughness"); value != nullptr) {
        material.roughness = value->value;
    }
    if (const auto* value = material_scalar(*definition, "metallic"); value != nullptr) {
        material.metallic = value->value;
    }
    if (const auto* value = material_scalar(*definition, "normal_scale"); value != nullptr) {
        material.normal_scale = value->value;
    }
    if (definition->double_sided) {
        material.flags = material.flags | renderer::VoxelMaterialFlags::two_sided;
    }
    return initialize_image_quad(active_renderer, image, std::move(material));
}

core::Status AssetLabMode::initialize_terrain_preview(renderer::Renderer& active_renderer) {
    const auto slash = config_.selection_id.find_last_of('/');
    const auto token =
        slash == std::string::npos ? config_.selection_id : config_.selection_id.substr(slash + 1U);
    const auto definitions = config_.content->voxel_palette.definitions();
    const auto voxel =
        std::ranges::find_if(definitions, [&](const world::VoxelDefinition* candidate) {
            return candidate != nullptr && candidate->terrain_material == token;
        });
    if (voxel == definitions.end()) {
        return core::Status::failure(
            "asset_lab.material_has_no_voxel",
            "terrain material preview requires a voxel using material token '" + token + "'");
    }
    terrain_world_.emplace();
    world::VoxelChunk chunk({0, 0, 0});
    std::vector<world::VoxelCell> cells(world::VoxelChunk::total_cells, world::VoxelCell::air());
    constexpr auto edge = static_cast<std::size_t>(world::VoxelChunk::edge_length);
    for (std::uint16_t z = 0; z < world::VoxelChunk::edge_length; ++z) {
        for (std::uint16_t y = 0; y < 5U; ++y) {
            for (std::uint16_t x = 0; x < world::VoxelChunk::edge_length; ++x) {
                const auto index = static_cast<std::size_t>(z) * edge * edge +
                                   static_cast<std::size_t>(y) * edge + x;
                std::uint16_t state_bits = 0U;
                if (y == 4U) {
                    const auto state_index =
                        static_cast<std::uint8_t>(((x / 8U) + (z / 8U) * 4U) % 9U);
                    state_bits = world::encode_voxel_surface_states(
                        {static_cast<std::uint16_t>(1U << state_index), 7U});
                }
                cells[index] = {(*voxel)->type, 255U, state_bits, 0U};
            }
        }
    }
    auto status = chunk.load_generated_cells(std::move(cells));
    if (!status) {
        return status;
    }
    status = terrain_world_->chunks().insert_generated(std::move(chunk),
                                                       terrain_world_->dirty_regions());
    if (!status) {
        return status;
    }
    camera_.floating_origin.block = {16, 0, 48};
    camera_.local_position = {0.0F, 13.0F, 0.0F};
    camera_.pitch_radians = -0.32F;
    status = camera_.update_matrices();
    if (!status) {
        return status;
    }
    return active_renderer.synchronize_chunks(*terrain_world_, camera_);
}

core::Status AssetLabMode::initialize_particle_preview(renderer::Renderer& active_renderer) {
    const auto particle_id = core::PrototypeId::parse(config_.selection_id);
    if (!particle_id || find_particle(*config_.content, *particle_id) == nullptr) {
        return core::Status::failure("asset_lab.missing_particle",
                                     "Asset Lab particle preview requires a particle prototype");
    }
    renderer::ParticleSystemConfig particle_config;
    particle_config.maximum_particles = 4'096;
    particle_config.maximum_emitters = 8;
    auto created =
        renderer::CpuParticleSystem::create(particle_config, config_.content->particle_prototypes);
    if (!created) {
        return core::Status::failure(created.error().code, created.error().message);
    }
    particles_.emplace(std::move(created).value());
    auto emitter = particles_->create_emitter({*particle_id,
                                               world::WorldPosition{0.0, 1.0, -4.0},
                                               {0.0F, 1.0F, 0.0F},
                                               {},
                                               3'600.0F,
                                               24.0F,
                                               16U,
                                               0x41535345544c4142ULL});
    if (!emitter) {
        return core::Status::failure(emitter.error().code, emitter.error().message);
    }
    auto status =
        particle_presentation_.initialize(active_renderer, {.maximum_presented_particles = 4'096});
    if (!status) {
        return status;
    }
    particles_initialized_ = true;
    return core::Status::ok();
}

core::Result<game::GameApplicationFrameOutput>
AssetLabMode::update(game::GameApplicationServices& services,
                     const game::GameApplicationFrame& frame) {
    if (services.headless()) {
        return core::Result<game::GameApplicationFrameOutput>::success({});
    }
    auto* active_renderer = services.renderer();
    if (active_renderer == nullptr) {
        return core::Result<game::GameApplicationFrameOutput>::failure(
            "asset_lab.renderer_missing", "Asset Lab renderer disappeared");
    }
    if (frame.extent.height != 0U) {
        auto status = camera_.set_aspect_ratio(static_cast<float>(frame.extent.width) /
                                               static_cast<float>(frame.extent.height));
        if (!status) {
            return core::Result<game::GameApplicationFrameOutput>::failure(status.error().code,
                                                                           status.error().message);
        }
    }
    if (models_initialized_) {
        ++snapshot_.simulation_tick;
        auto synchronized = models_.synchronize(*active_renderer, snapshot_);
        if (!synchronized) {
            return core::Result<game::GameApplicationFrameOutput>::failure(
                synchronized.error().code, synchronized.error().message);
        }
    }
    if (terrain_world_.has_value()) {
        auto status = active_renderer->synchronize_chunks(*terrain_world_, camera_);
        if (!status) {
            return core::Result<game::GameApplicationFrameOutput>::failure(status.error().code,
                                                                           status.error().message);
        }
    }
    if (particles_initialized_) {
        auto status = particles_->update(frame.delta_seconds());
        if (!status) {
            return core::Result<game::GameApplicationFrameOutput>::failure(status.error().code,
                                                                           status.error().message);
        }
        auto synchronized =
            particle_presentation_.synchronize(*active_renderer, *particles_, camera_);
        if (!synchronized) {
            return core::Result<game::GameApplicationFrameOutput>::failure(
                synchronized.error().code, synchronized.error().message);
        }
    }
    if ((config_.show_bounds || config_.debug_view == LightingDebugView::bounds) &&
        active_renderer->debug_renderer() != nullptr && inspected_model_.has_value()) {
        auto status = active_renderer->debug_renderer()->submit_aabb(
            world::WorldPosition{0.0, 0.0, -static_cast<double>(preview_camera_distance_)},
            inspected_model_->bounds, {0.1F, 0.9F, 1.0F, 1.0F}, 0.05F);
        if (!status) {
            return core::Result<game::GameApplicationFrameOutput>::failure(status.error().code,
                                                                           status.error().message);
        }
    }
    if ((config_.show_skeleton || config_.debug_view == LightingDebugView::skeletons) &&
        active_renderer->debug_renderer() != nullptr && inspected_model_.has_value()) {
        for (std::size_t index = 0; index < inspected_model_->nodes.size(); ++index) {
            const auto parent = inspected_model_->nodes[index].parent;
            if (parent == assets::no_model_index) {
                continue;
            }
            const auto& child_matrix = inspected_node_matrices_[index];
            const auto& parent_matrix = inspected_node_matrices_[parent];
            renderer::DebugLineDesc line;
            line.start = world::WorldPosition{static_cast<double>(parent_matrix.at(0, 3)),
                                              static_cast<double>(parent_matrix.at(1, 3)),
                                              -static_cast<double>(preview_camera_distance_) +
                                                  static_cast<double>(parent_matrix.at(2, 3))};
            line.end = world::WorldPosition{static_cast<double>(child_matrix.at(0, 3)),
                                            static_cast<double>(child_matrix.at(1, 3)),
                                            -static_cast<double>(preview_camera_distance_) +
                                                static_cast<double>(child_matrix.at(2, 3))};
            line.color = {1.0F, 0.45F, 0.1F, 1.0F};
            line.lifetime_seconds = 0.05F;
            auto status = active_renderer->debug_renderer()->submit_line(std::move(line));
            if (!status) {
                return core::Result<game::GameApplicationFrameOutput>::failure(
                    status.error().code, status.error().message);
            }
        }
    }
    game::GameApplicationFrameOutput output;
    output.render = renderer::RenderFrameInput{camera_, 1.0F, frame.delta_seconds()};
    return core::Result<game::GameApplicationFrameOutput>::success(std::move(output));
}

core::Status AssetLabMode::shutdown(game::GameApplicationServices& services) {
    auto status = core::Status::ok();
    if (auto* active_renderer = services.renderer(); active_renderer != nullptr) {
        if (particles_initialized_) {
            status = particle_presentation_.shutdown(*active_renderer);
        }
        if (models_initialized_) {
            const auto model_status = models_.shutdown(*active_renderer);
            if (!model_status && status) {
                status = model_status;
            }
        }
        if (direct_preview_object_.is_valid()) {
            renderer::RenderSceneUpdate removal;
            removal.kind = renderer::RenderSceneUpdateKind::remove_object;
            removal.object_id = direct_preview_object_;
            const auto remove_status = active_renderer->apply_scene_updates(
                std::span<const renderer::RenderSceneUpdate>(&removal, 1U));
            if (!remove_status && status) {
                status = remove_status;
            }
        }
        if (direct_preview_mesh_.is_valid()) {
            const auto release_status = active_renderer->release_static_mesh(direct_preview_mesh_);
            if (!release_status && status) {
                status = release_status;
            }
        }
    }
    particles_.reset();
    terrain_world_.reset();
    inspected_model_.reset();
    inspected_node_matrices_.clear();
    cooked_assets_.reset();
    direct_preview_object_ = {};
    direct_preview_mesh_ = {};
    particles_initialized_ = false;
    models_initialized_ = false;
    return status;
}

std::string AssetLabMode::summary() const {
    if (!inspection_.has_value()) {
        return "Asset Lab: no inspection";
    }
    return inspection_->summary() + " preview=" + std::string(preview_kind_name(config_.preview)) +
           " lighting=" + std::string(lighting_preset_name(config_.lighting)) +
           " debug=" + std::string(asset_lab_debug_view_name(config_.debug_view));
}

} // namespace heartstead::asset_lab
