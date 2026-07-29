#include "game/presentation/model_presentation_system.hpp"

#include "engine/assets/asset_cooker.hpp"
#include "engine/assets/cooked_asset_store.hpp"
#include "engine/assets/model_asset.hpp"

#include <algorithm>
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

} // namespace

core::Status
ModelPresentationSystem::initialize(renderer::Renderer& renderer,
                                    const entities::VisualDefinitionRegistry& visual_definitions,
                                    const std::filesystem::path& cooked_asset_root) {
    if (initialized_) {
        return core::Status::failure("model_presentation.already_initialized",
                                     "model presentation system cannot be initialized twice");
    }
    auto store = assets::CookedAssetStore::load(cooked_asset_root);
    if (!store) {
        return core::Status::failure(store.error().code, store.error().message);
    }

    std::unordered_map<std::string, assets::ModelAsset> model_cache;
    presentations_.reserve(visual_definitions.size());
    const auto rollback = [&]() {
        for (auto entry = presentations_.rbegin(); entry != presentations_.rend(); ++entry) {
            (void)entry->presentation.shutdown(renderer);
        }
        presentations_.clear();
    };
    for (const auto& definition : visual_definitions.definitions()) {
        auto cached = model_cache.find(definition.model_asset);
        if (cached == model_cache.end()) {
            auto model = load_production_model(store.value(), definition.model_asset);
            if (!model) {
                rollback();
                return core::Status::failure(model.error().code, model.error().message);
            }
            cached = model_cache.emplace(definition.model_asset, std::move(model).value()).first;
        }
        const auto& model = cached->second;
        for (const auto& [role, clip_name] : definition.animation_clips) {
            auto clip = assets::resolve_model_animation_clip(model, clip_name);
            if (!clip) {
                rollback();
                return core::Status::failure(clip.error().code,
                                             definition.id.value() + " maps " + role +
                                                 " to an invalid clip: " + clip.error().message);
            }
        }

        AnimatedModelPresentationConfig config;
        config.asset_id = definition.model_asset;
        config.visual_prototype = definition.entity_prototype;
        config.model = model;
        config.animated_bounds = model.bounds.expanded(definition.bounds_padding);
        config.flags = definition.cast_shadow ? renderer::RenderObjectFlags::cast_shadow
                                              : renderer::RenderObjectFlags::none;
        const auto has_skinned_primitives =
            std::ranges::any_of(model.primitives, [](const assets::ModelPrimitive& primitive) {
                return primitive.skin != assets::no_model_index;
            });
        if (has_skinned_primitives) {
            const auto* idle = definition.animation("idle");
            const auto* walk = definition.animation("walk");
            const auto* run = definition.animation("run");
            const auto* jump = definition.animation("jump");
            const auto* fall = definition.animation("fall");
            const auto* swim = definition.animation("swim");
            if (idle == nullptr || walk == nullptr || run == nullptr || jump == nullptr ||
                fall == nullptr || swim == nullptr) {
                rollback();
                return core::Status::failure(
                    "model_presentation.missing_locomotion_mapping",
                    "skinned visual must map idle, walk, run, jump, fall, and swim clips: " +
                        definition.id.value());
            }
            auto clips = animation::resolve_locomotion_clips(
                model, *idle, *walk, *run, *jump, *fall, *swim, definition.transition_ticks);
            if (!clips) {
                rollback();
                return core::Status::failure(clips.error().code, clips.error().message);
            }
            config.locomotion_clips = clips.value();
        } else if (!definition.animation_clips.empty()) {
            rollback();
            return core::Status::failure("model_presentation.static_model_has_animations",
                                         "static visual cannot declare animation mappings: " +
                                             definition.id.value());
        }

        PresentationEntry entry;
        entry.visual_id = definition.id;
        auto status = entry.presentation.initialize(renderer, std::move(config));
        if (!status) {
            rollback();
            return status;
        }
        presentations_.push_back(std::move(entry));
    }
    stats_.definition_count = static_cast<std::uint32_t>(presentations_.size());
    stats_.loaded_model_count = static_cast<std::uint32_t>(model_cache.size());
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
    for (auto& entry : presentations_) {
        auto synchronized = entry.presentation.synchronize(renderer, snapshot);
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

} // namespace heartstead::game
