#include "game/presentation/model_presentation_system.hpp"

#include "engine/assets/asset_cooker.hpp"
#include "engine/assets/cooked_asset_store.hpp"
#include "engine/assets/model_asset.hpp"
#include "engine/core/logging.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>

namespace heartstead::game {

namespace {

[[nodiscard]] core::Result<assets::ModelAsset>
load_production_model(const assets::CookedAssetStore& store, std::string_view logical_id) {
    auto payload = store.load_payload(logical_id);
    if (!payload) {
        return core::Result<assets::ModelAsset>::failure(payload.error().code,
                                                         payload.error().message);
    }
    const auto pipeline = assets::asset_cook_pipeline_name(
        assets::AssetKind::model, assets::AssetCookBackend::production_converters);
    if (payload.value().kind != assets::AssetKind::model ||
        payload.value().profile != "production" || payload.value().backend != pipeline) {
        return core::Result<assets::ModelAsset>::failure(
            "model_presentation.invalid_cooked_model",
            "visual definition model is not a production-cooked model: " + std::string(logical_id));
    }
    return assets::decode_model_asset(payload.value().bytes);
}

void add_stats(AnimatedModelPresentationStats& total,
               const AnimatedModelPresentationStats& value) noexcept {
    total.retained_entities += value.retained_entities;
    total.retained_primitives += value.retained_primitives;
    total.evaluated_poses += value.evaluated_poses;
    total.uploaded_palettes += value.uploaded_palettes;
    total.inserted_entities += value.inserted_entities;
    total.updated_entities += value.updated_entities;
    total.removed_entities += value.removed_entities;
}

[[nodiscard]] PresentationAssetLoadDiagnostic
make_load_diagnostic(const assets::CookedAssetStore& store, std::string logical_id,
                     std::string failing_dependency, std::string fallback_used,
                     const core::Error& error) {
    PresentationAssetLoadDiagnostic diagnostic;
    diagnostic.logical_id = std::move(logical_id);
    diagnostic.failing_dependency = std::move(failing_dependency);
    diagnostic.fallback_used = std::move(fallback_used);
    diagnostic.error_code = error.code;
    diagnostic.message = error.message;
    const auto* record = store.manifest().find_active(diagnostic.logical_id);
    if (record == nullptr) {
        diagnostic.source_path = "<missing>";
        diagnostic.cooked_path = "<missing>";
    } else {
        diagnostic.source_path = record->source_virtual_path.to_string();
        diagnostic.cooked_path = (store.root() / record->cooked_relative_path).generic_string();
    }
    return diagnostic;
}

void log_load_diagnostic(const PresentationAssetLoadDiagnostic& diagnostic) {
    core::log(core::LogLevel::warning,
              "model_presentation.asset_fallback: logical_id=" + diagnostic.logical_id +
                  " source=" + diagnostic.source_path + " cooked=" + diagnostic.cooked_path +
                  " dependency=" + diagnostic.failing_dependency +
                  " error=" + diagnostic.error_code + " fallback=" + diagnostic.fallback_used +
                  ": " + diagnostic.message);
}

[[nodiscard]] const renderer::materials::MaterialScalarParameter*
find_scalar(const renderer::materials::MaterialDefinition& material, std::string_view name) {
    const auto found = std::ranges::find_if(
        material.scalars, [name](const auto& value) { return value.name == name; });
    return found == material.scalars.end() ? nullptr : &*found;
}

[[nodiscard]] const renderer::materials::MaterialColorParameter*
find_color(const renderer::materials::MaterialDefinition& material, std::string_view name) {
    const auto found = std::ranges::find_if(
        material.colors, [name](const auto& value) { return value.name == name; });
    return found == material.colors.end() ? nullptr : &*found;
}

[[nodiscard]] core::Status
apply_material_overrides(assets::ModelAsset& model,
                         std::span<const entities::VisualMaterialOverride> overrides,
                         const renderer::materials::MaterialRegistry* materials) {
    if (overrides.empty()) {
        return core::Status::ok();
    }
    if (materials == nullptr) {
        return core::Status::failure(
            "model_presentation.missing_material_registry",
            "visual prefab material overrides require the validated material registry");
    }
    for (const auto& override : overrides) {
        const auto slot =
            std::ranges::find_if(model.materials, [&](const assets::ModelMaterial& material) {
                return material.name == override.slot;
            });
        if (slot == model.materials.end()) {
            return core::Status::failure(
                "model_presentation.missing_material_slot",
                "visual prefab material slot does not exist in the cooked model: " + override.slot);
        }
        const auto* material = materials->find(override.material.value());
        if (material == nullptr) {
            return core::Status::failure(
                "model_presentation.missing_material_override",
                "visual prefab material override is absent from the validated registry: " +
                    override.material.value());
        }
        if (!material->textures.empty()) {
            return core::Status::failure(
                "model_presentation.external_override_texture_unsupported",
                "model material overrides currently require parameter-only materials; external "
                "texture bindings must be authored in the source glTF material");
        }
        if (const auto* tint = find_color(*material, "tint"); tint != nullptr) {
            slot->base_color_factor = {tint->value.red, tint->value.green, tint->value.blue,
                                       tint->value.alpha};
        }
        if (const auto* emissive = find_color(*material, "emissive"); emissive != nullptr) {
            slot->emissive_factor = {emissive->value.red, emissive->value.green,
                                     emissive->value.blue};
        }
        if (const auto* value = find_scalar(*material, "roughness"); value != nullptr) {
            slot->roughness_factor = value->value;
        }
        if (const auto* value = find_scalar(*material, "metallic"); value != nullptr) {
            slot->metallic_factor = value->value;
        }
        if (const auto* value = find_scalar(*material, "normal_scale"); value != nullptr) {
            slot->normal_scale = value->value;
        }
        if (const auto* value = find_scalar(*material, "occlusion_strength"); value != nullptr) {
            slot->occlusion_strength = value->value;
        }
        if (const auto* value = find_scalar(*material, "alpha_cutoff"); value != nullptr) {
            slot->alpha_cutoff = value->value;
        }
        slot->double_sided = material->double_sided;
        switch (material->blend_mode) {
        case renderer::materials::MaterialBlendMode::opaque:
            slot->alpha_mode = assets::ModelAlphaMode::opaque;
            break;
        case renderer::materials::MaterialBlendMode::masked:
            slot->alpha_mode = assets::ModelAlphaMode::mask;
            break;
        case renderer::materials::MaterialBlendMode::translucent:
        case renderer::materials::MaterialBlendMode::additive:
            slot->alpha_mode = assets::ModelAlphaMode::blend;
            break;
        }
    }
    return assets::validate_model_asset(model);
}

} // namespace

core::Status ModelPresentationSystem::initialize(
    renderer::Renderer& renderer, const entities::VisualDefinitionRegistry& visual_definitions,
    const std::filesystem::path& cooked_asset_root, ModelPresentationSystemConfig config) {
    if (initialized_) {
        return core::Status::failure("model_presentation.already_initialized",
                                     "model presentation system cannot be initialized twice");
    }
    stats_ = {};
    load_diagnostics_.clear();
    const auto fallback_visual_id = core::PrototypeId::parse(config.fallback_visual_id);
    const auto* fallback_definition =
        fallback_visual_id ? visual_definitions.find(*fallback_visual_id) : nullptr;
    if (fallback_definition == nullptr) {
        return core::Status::failure("model_presentation.missing_fallback_visual",
                                     "model presentation requires a declared fallback visual: " +
                                         config.fallback_visual_id);
    }
    auto store = assets::CookedAssetStore::load(cooked_asset_root);
    if (!store) {
        return core::Status::failure(store.error().code, store.error().message);
    }

    if (config.fallback_animation_role.empty()) {
        return core::Status::failure("model_presentation.invalid_animation_fallback",
                                     "model presentation fallback animation role is required");
    }
    auto fallback_model_asset =
        load_production_model(store.value(), fallback_definition->model_asset);
    if (!fallback_model_asset) {
        return core::Status::failure(fallback_model_asset.error().code,
                                     "fallback visual model '" + fallback_definition->model_asset +
                                         "' could not be loaded from the cooked store: " +
                                         fallback_model_asset.error().message);
    }

    std::unordered_map<std::string, std::shared_ptr<const assets::ModelAsset>> model_cache;
    auto fallback_model =
        std::make_shared<assets::ModelAsset>(std::move(fallback_model_asset).value());
    model_cache.emplace(fallback_definition->model_asset, fallback_model);
    std::size_t presentation_capacity = 0;
    for (const auto& definition : visual_definitions.definitions()) {
        presentation_capacity += definition.lods.size() * (definition.state_rules.size() + 1U);
    }
    presentations_.reserve(presentation_capacity);
    const auto rollback = [&]() {
        for (auto entry = presentations_.rbegin(); entry != presentations_.rend(); ++entry) {
            (void)entry->presentation.shutdown(renderer);
        }
        presentations_.clear();
    };
    const auto load_cached_model =
        [&](std::string_view logical_id,
            bool& used_fallback) -> std::shared_ptr<const assets::ModelAsset> {
        auto cached = model_cache.find(std::string(logical_id));
        if (cached != model_cache.end()) {
            return cached->second;
        }
        auto model = load_production_model(store.value(), logical_id);
        if (!model) {
            auto diagnostic = make_load_diagnostic(store.value(), std::string(logical_id),
                                                   std::string(logical_id),
                                                   fallback_definition->model_asset, model.error());
            log_load_diagnostic(diagnostic);
            load_diagnostics_.push_back(std::move(diagnostic));
            used_fallback = true;
            return fallback_model;
        }
        auto shared = std::make_shared<assets::ModelAsset>(std::move(model).value());
        model_cache.emplace(std::string(logical_id), shared);
        return shared;
    };
    const auto validate_variant_metadata = [&](const entities::EntityVisualDefinition& definition,
                                               const entities::VisualStateRule* state_rule,
                                               const assets::ModelAsset& model) -> core::Status {
        const auto has_node = [&](std::string_view name) {
            return std::ranges::any_of(
                       model.nodes,
                       [name](const assets::ModelNode& node) { return node.name == name; }) ||
                   std::ranges::any_of(model.primitives,
                                       [name](const assets::ModelPrimitive& primitive) {
                                           return primitive.name == name;
                                       });
        };
        for (const auto& [alias, socket_name] : definition.socket_aliases) {
            (void)alias;
            if (!std::ranges::any_of(model.sockets, [&](const assets::ModelSocket& socket) {
                    return socket.name == socket_name;
                })) {
                return core::Status::failure("model_presentation.missing_socket",
                                             "visual prefab '" + definition.id.value() +
                                                 "' references missing socket '" + socket_name +
                                                 "'");
            }
        }
        for (const auto& anchor : definition.anchors) {
            auto socket_name = anchor.socket;
            if (const auto alias = definition.socket_aliases.find(socket_name);
                alias != definition.socket_aliases.end()) {
                socket_name = alias->second;
            }
            if (!std::ranges::any_of(model.sockets, [&](const assets::ModelSocket& socket) {
                    return socket.name == socket_name;
                })) {
                return core::Status::failure(
                    "model_presentation.missing_anchor_socket",
                    "visual prefab '" + definition.id.value() + "' anchor '" + anchor.name +
                        "' references missing socket '" + socket_name + "'");
            }
        }
        if (state_rule != nullptr) {
            for (const auto& [group_name, visible] : state_rule->group_visibility) {
                (void)visible;
                const auto* group = definition.visibility_group(group_name);
                if (group == nullptr || !std::ranges::all_of(group->nodes, has_node)) {
                    return core::Status::failure(
                        "model_presentation.missing_visibility_node",
                        "visual prefab '" + definition.id.value() + "' group '" + group_name +
                            "' references a node absent from its state model");
                }
            }
        }
        return core::Status::ok();
    };
    for (const auto& definition : visual_definitions.definitions()) {
        bool definition_used_fallback = false;
        std::vector<const entities::VisualStateRule*> state_variants{nullptr};
        for (const auto& state_rule : definition.state_rules) {
            state_variants.push_back(&state_rule);
        }
        for (const auto* state_rule : state_variants) {
            for (const auto& lod : definition.lods) {
                const auto logical_model =
                    state_rule != nullptr && !state_rule->model_asset.empty()
                        ? std::string_view{state_rule->model_asset}
                        : (definition.lods.size() == 1U ? std::string_view{definition.model_asset}
                                                        : std::string_view{lod.model_asset});
                bool using_model_fallback = false;
                auto model_shared = load_cached_model(logical_model, using_model_fallback);
                definition_used_fallback = definition_used_fallback || using_model_fallback;
                std::vector<entities::VisualMaterialOverride> material_overrides =
                    definition.material_overrides;
                if (state_rule != nullptr) {
                    for (const auto& state_override : state_rule->material_overrides) {
                        const auto existing = std::ranges::find_if(
                            material_overrides,
                            [&](const entities::VisualMaterialOverride& candidate) {
                                return candidate.slot == state_override.slot;
                            });
                        if (existing == material_overrides.end()) {
                            material_overrides.push_back(state_override);
                        } else {
                            *existing = state_override;
                        }
                    }
                }
                std::string material_variant_id = std::string(logical_model);
                if (!using_model_fallback && !material_overrides.empty()) {
                    material_variant_id += "#visual-materials";
                    for (const auto& material_override : material_overrides) {
                        material_variant_id +=
                            "/" + material_override.slot + "=" + material_override.material.value();
                    }
                    if (const auto existing = model_cache.find(material_variant_id);
                        existing != model_cache.end()) {
                        model_shared = existing->second;
                    } else {
                        auto overridden_model = std::make_shared<assets::ModelAsset>(*model_shared);
                        auto override_status = apply_material_overrides(
                            *overridden_model, material_overrides, config.material_registry);
                        if (!override_status) {
                            rollback();
                            return override_status;
                        }
                        model_shared = overridden_model;
                        model_cache.emplace(material_variant_id, std::move(overridden_model));
                    }
                }
                const auto& model = *model_shared;
                auto metadata_status = validate_variant_metadata(definition, state_rule, model);
                if (!metadata_status && !using_model_fallback) {
                    rollback();
                    return metadata_status;
                }

                AnimatedModelPresentationConfig presentation_config;
                presentation_config.asset_id =
                    using_model_fallback ? fallback_definition->model_asset : material_variant_id;
                presentation_config.visual_prototype = definition.entity_prototype;
                presentation_config.model = std::move(model_shared);
                presentation_config.animated_bounds =
                    definition.bounds_override.value_or(model.bounds)
                        .expanded(definition.bounds_padding);
                presentation_config.model_scale = definition.model_scale;
                presentation_config.bounds_padding = definition.bounds_padding;
                presentation_config.minimum_view_distance = lod.minimum_distance;
                presentation_config.maximum_view_distance = lod.maximum_distance;
                presentation_config.flags =
                    definition.shadow_policy == entities::VisualShadowPolicy::cast
                        ? renderer::RenderObjectFlags::cast_shadow
                        : renderer::RenderObjectFlags::none;
                if (!definition.state_rules.empty()) {
                    const auto definition_copy = definition;
                    const auto selected_channel =
                        state_rule == nullptr ? std::string{} : state_rule->channel;
                    const auto selected_value =
                        state_rule == nullptr ? std::string{} : state_rule->value;
                    presentation_config.object_filter =
                        [definition_copy, selected_channel,
                         selected_value](const RenderObjectSnapshot& object) {
                            const auto* selected =
                                definition_copy.resolve_state_rule(object.visual_states);
                            if (selected_channel.empty()) {
                                return selected == nullptr;
                            }
                            return selected != nullptr && selected->channel == selected_channel &&
                                   selected->value == selected_value;
                        };
                }
                if (state_rule != nullptr) {
                    for (const auto& [group_name, visible] : state_rule->group_visibility) {
                        if (visible) {
                            continue;
                        }
                        const auto* group = definition.visibility_group(group_name);
                        presentation_config.hidden_model_nodes.insert(group->nodes.begin(),
                                                                      group->nodes.end());
                    }
                }
                if ((!definition.animation_clips.empty() ||
                     (state_rule != nullptr && !state_rule->animation_clip.empty())) &&
                    !using_model_fallback) {
                    if (!assets::model_capabilities(model).has_animation_clips) {
                        rollback();
                        return core::Status::failure(
                            "model_presentation.model_has_no_animation_clips",
                            "visual maps animations but its model has no animation clips: " +
                                definition.id.value());
                    }
                    const auto* fallback_animation =
                        definition.animation(config.fallback_animation_role);
                    const auto forced_animation =
                        state_rule == nullptr ? std::string_view{}
                                              : std::string_view{state_rule->animation_clip};
                    if (!forced_animation.empty()) {
                        fallback_animation = &state_rule->animation_clip;
                    }
                    auto fallback_clip =
                        fallback_animation == nullptr
                            ? core::Result<std::uint32_t>::failure(
                                  "model_presentation.missing_fallback_animation",
                                  "visual does not map the configured fallback animation role")
                            : assets::resolve_model_animation_clip(model, *fallback_animation);
                    if (!fallback_clip) {
                        rollback();
                        return core::Status::failure(
                            fallback_clip.error().code,
                            "animated visual '" + definition.id.value() +
                                "' cannot resolve its named fallback animation role '" +
                                config.fallback_animation_role +
                                "': " + fallback_clip.error().message);
                    }
                    constexpr std::array<std::string_view, 6> locomotion_roles{
                        "idle", "walk", "run", "jump", "fall", "swim"};
                    std::array<std::uint32_t, locomotion_roles.size()> resolved{};
                    for (std::size_t index = 0; index < locomotion_roles.size(); ++index) {
                        const auto role = locomotion_roles[index];
                        const auto* mapped = forced_animation.empty() ? definition.animation(role)
                                                                      : &state_rule->animation_clip;
                        auto clip =
                            mapped == nullptr
                                ? core::Result<std::uint32_t>::failure(
                                      "model_presentation.missing_locomotion_mapping",
                                      "visual does not map locomotion role " + std::string(role))
                                : assets::resolve_model_animation_clip(model, *mapped);
                        if (clip) {
                            resolved[index] = clip.value();
                            continue;
                        }
                        resolved[index] = fallback_clip.value();
                        auto diagnostic = make_load_diagnostic(
                            store.value(), std::string(logical_model),
                            definition.id.value() + "#animations/" + std::string(role),
                            config.fallback_animation_role + "=" + *fallback_animation,
                            clip.error());
                        log_load_diagnostic(diagnostic);
                        load_diagnostics_.push_back(std::move(diagnostic));
                        ++stats_.fallback_animation_mapping_count;
                    }
                    animation::LocomotionClipSet clips;
                    clips.idle = resolved[0];
                    clips.walk = resolved[1];
                    clips.run = resolved[2];
                    clips.jump = resolved[3];
                    clips.fall = resolved[4];
                    clips.swim = resolved[5];
                    clips.transition_ticks = definition.transition_ticks;
                    auto clips_status = clips.validate(model);
                    if (!clips_status) {
                        rollback();
                        return clips_status;
                    }
                    presentation_config.locomotion_clips = clips;
                }
                PresentationEntry entry;
                entry.visual_id = definition.id;
                entry.is_fallback = definition.id == *fallback_visual_id;
                auto status =
                    entry.presentation.initialize(renderer, std::move(presentation_config));
                if (!status) {
                    rollback();
                    return status;
                }
                presentations_.push_back(std::move(entry));
                ++stats_.lod_variant_count;
            }
        }
        stats_.fallback_model_definition_count += definition_used_fallback ? 1U : 0U;
    }
    known_visual_prototypes_.clear();
    known_visual_prototypes_.reserve(visual_definitions.size());
    for (const auto& definition : visual_definitions.definitions()) {
        known_visual_prototypes_.insert(definition.entity_prototype.value());
    }
    unresolved_visuals_.clear();
    fallback_visual_prototype_ = fallback_definition->entity_prototype;
    stats_.definition_count = static_cast<std::uint32_t>(visual_definitions.size());
    stats_.presentation_variant_count = static_cast<std::uint32_t>(presentations_.size());
    stats_.loaded_model_count = static_cast<std::uint32_t>(model_cache.size());
    stats_.load_diagnostic_count = static_cast<std::uint32_t>(load_diagnostics_.size());
    initialized_ = true;
    return core::Status::ok();
}

core::Result<ModelPresentationSystemStats>
ModelPresentationSystem::synchronize(renderer::Renderer& renderer, const RenderSnapshot& snapshot) {
    if (!initialized_) {
        return core::Result<ModelPresentationSystemStats>::failure(
            "model_presentation.not_initialized",
            "model presentation system must be initialized before synchronization");
    }
    ModelPresentationSystemStats frame_stats;
    frame_stats.definition_count = stats_.definition_count;
    frame_stats.presentation_variant_count = stats_.presentation_variant_count;
    frame_stats.lod_variant_count = stats_.lod_variant_count;
    frame_stats.loaded_model_count = stats_.loaded_model_count;
    frame_stats.fallback_model_definition_count = stats_.fallback_model_definition_count;
    frame_stats.fallback_animation_mapping_count = stats_.fallback_animation_mapping_count;
    frame_stats.load_diagnostic_count = stats_.load_diagnostic_count;
    RenderSnapshot fallback_snapshot = snapshot;
    fallback_snapshot.objects.clear();
    for (const auto& object : snapshot.objects) {
        const auto& prototype = object.visual_prototype.value();
        const bool is_declared = known_visual_prototypes_.contains(prototype);
        if (is_declared && object.visual_prototype != fallback_visual_prototype_) {
            continue;
        }
        auto fallback_object = object;
        fallback_object.visual_prototype = fallback_visual_prototype_;
        fallback_snapshot.objects.push_back(std::move(fallback_object));
        if (!is_declared && unresolved_visuals_.insert(prototype).second) {
            core::log(core::LogLevel::warning,
                      "model_presentation.unresolved_visual: using fallback visual for " +
                          prototype);
        }
    }
    frame_stats.fallback_entity_count =
        static_cast<std::uint32_t>(fallback_snapshot.objects.size());
    frame_stats.unresolved_visual_count = static_cast<std::uint32_t>(unresolved_visuals_.size());
    for (auto& entry : presentations_) {
        auto synchronized = entry.presentation.synchronize(
            renderer, entry.is_fallback ? fallback_snapshot : snapshot);
        if (!synchronized) {
            return core::Result<ModelPresentationSystemStats>::failure(
                synchronized.error().code, synchronized.error().message);
        }
        add_stats(frame_stats.models, synchronized.value());
    }
    stats_ = frame_stats;
    return core::Result<ModelPresentationSystemStats>::success(stats_);
}

core::Status ModelPresentationSystem::shutdown(renderer::Renderer& renderer) {
    core::Status first_failure = core::Status::ok();
    for (auto entry = presentations_.rbegin(); entry != presentations_.rend(); ++entry) {
        auto status = entry->presentation.shutdown(renderer);
        if (!status && first_failure) {
            first_failure = status;
        }
    }
    presentations_.clear();
    known_visual_prototypes_.clear();
    unresolved_visuals_.clear();
    load_diagnostics_.clear();
    fallback_visual_prototype_ = {};
    stats_ = {};
    initialized_ = false;
    return first_failure;
}

bool ModelPresentationSystem::is_initialized() const noexcept {
    return initialized_;
}

const ModelPresentationSystemStats& ModelPresentationSystem::stats() const noexcept {
    return stats_;
}

std::span<const PresentationAssetLoadDiagnostic>
ModelPresentationSystem::load_diagnostics() const noexcept {
    return load_diagnostics_;
}

} // namespace heartstead::game
