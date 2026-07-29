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

[[nodiscard]] PresentationAssetLoadDiagnostic make_load_diagnostic(
    const assets::CookedAssetStore& store, std::string logical_id,
    std::string failing_dependency, std::string fallback_used, const core::Error& error) {
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
                  " dependency=" + diagnostic.failing_dependency + " error=" +
                  diagnostic.error_code + " fallback=" + diagnostic.fallback_used + ": " +
                  diagnostic.message);
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
        return core::Status::failure(
            fallback_model_asset.error().code,
            "fallback visual model '" + fallback_definition->model_asset +
                "' could not be loaded from the cooked store: " +
                fallback_model_asset.error().message);
    }

    std::unordered_map<std::string, std::shared_ptr<const assets::ModelAsset>> model_cache;
    auto fallback_model = std::make_shared<assets::ModelAsset>(
        std::move(fallback_model_asset).value());
    model_cache.emplace(fallback_definition->model_asset, fallback_model);
    presentations_.reserve(visual_definitions.size());
    const auto rollback = [&]() {
        for (auto entry = presentations_.rbegin(); entry != presentations_.rend(); ++entry) {
            (void)entry->presentation.shutdown(renderer);
        }
        presentations_.clear();
    };
    for (const auto& definition : visual_definitions.definitions()) {
        bool using_model_fallback = false;
        auto cached = model_cache.find(definition.model_asset);
        if (cached == model_cache.end()) {
            auto model = load_production_model(store.value(), definition.model_asset);
            if (!model) {
                auto diagnostic = make_load_diagnostic(
                    store.value(), definition.model_asset, definition.model_asset,
                    fallback_definition->model_asset, model.error());
                log_load_diagnostic(diagnostic);
                load_diagnostics_.push_back(std::move(diagnostic));
                using_model_fallback = true;
                cached = model_cache.find(fallback_definition->model_asset);
            } else {
                cached = model_cache
                             .emplace(definition.model_asset,
                                      std::make_shared<assets::ModelAsset>(
                                          std::move(model).value()))
                             .first;
            }
        }
        const auto& model = *cached->second;

        AnimatedModelPresentationConfig presentation_config;
        presentation_config.asset_id =
            using_model_fallback ? fallback_definition->model_asset : definition.model_asset;
        presentation_config.visual_prototype = definition.entity_prototype;
        presentation_config.model = cached->second;
        presentation_config.animated_bounds = model.bounds.expanded(definition.bounds_padding);
        presentation_config.flags = definition.cast_shadow
                                        ? renderer::RenderObjectFlags::cast_shadow
                                        : renderer::RenderObjectFlags::none;
        const auto has_skinned_primitives =
            std::ranges::any_of(model.primitives, [](const assets::ModelPrimitive& primitive) {
                return primitive.skin != assets::no_model_index;
            });
        if (has_skinned_primitives) {
            const auto* fallback_animation =
                definition.animation(config.fallback_animation_role);
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
                    "skinned visual '" + definition.id.value() +
                        "' cannot resolve its named fallback animation role '" +
                        config.fallback_animation_role + "': " + fallback_clip.error().message);
            }
            constexpr std::array<std::string_view, 6> locomotion_roles{
                "idle", "walk", "run", "jump", "fall", "swim"};
            std::array<std::uint32_t, locomotion_roles.size()> resolved{};
            for (std::size_t index = 0; index < locomotion_roles.size(); ++index) {
                const auto role = locomotion_roles[index];
                const auto* mapped = definition.animation(role);
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
                    store.value(), definition.model_asset,
                    definition.id.value() + "#animations/" + std::string(role),
                    config.fallback_animation_role + "=" + *fallback_animation, clip.error());
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
        } else if (!definition.animation_clips.empty() && !using_model_fallback) {
            rollback();
            return core::Status::failure("model_presentation.static_model_has_animations",
                                         "static visual cannot declare animation mappings: " +
                                             definition.id.value());
        }

        PresentationEntry entry;
        entry.visual_id = definition.id;
        entry.is_fallback = definition.id == *fallback_visual_id;
        auto status = entry.presentation.initialize(renderer, std::move(presentation_config));
        if (!status) {
            rollback();
            return status;
        }
        presentations_.push_back(std::move(entry));
        stats_.fallback_model_definition_count += using_model_fallback ? 1U : 0U;
    }
    known_visual_prototypes_.clear();
    known_visual_prototypes_.reserve(visual_definitions.size());
    for (const auto& definition : visual_definitions.definitions()) {
        known_visual_prototypes_.insert(definition.entity_prototype.value());
    }
    unresolved_visuals_.clear();
    fallback_visual_prototype_ = fallback_definition->entity_prototype;
    stats_.definition_count = static_cast<std::uint32_t>(presentations_.size());
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
