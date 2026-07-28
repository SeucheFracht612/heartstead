#include "game/presentation/animated_model_presentation.hpp"

#include "engine/animation/skeletal_animation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <unordered_set>
#include <utility>

namespace heartstead::game {

namespace {

[[nodiscard]] bool finite_color(const std::array<float, 4>& color) noexcept {
    return std::ranges::all_of(
        color, [](float value) { return std::isfinite(value) && value >= 0.0F && value <= 1.0F; });
}

[[nodiscard]] bool can_convert_to_float(double value) noexcept {
    return std::isfinite(value) &&
           std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max());
}

[[nodiscard]] core::Result<math::Transform3f>
render_transform(const world::WorldTransform& transform, const world::WorldPosition& anchor) {
    const auto relative = transform.position.relative_to(anchor.anchor) - anchor.local_offset;
    if (!can_convert_to_float(relative.x) || !can_convert_to_float(relative.y) ||
        !can_convert_to_float(relative.z) || !can_convert_to_float(transform.rotation_degrees.x) ||
        !can_convert_to_float(transform.rotation_degrees.y) ||
        !can_convert_to_float(transform.rotation_degrees.z) ||
        !can_convert_to_float(transform.scale.x) || !can_convert_to_float(transform.scale.y) ||
        !can_convert_to_float(transform.scale.z)) {
        return core::Result<math::Transform3f>::failure(
            "animated_model_presentation.transform_out_of_range",
            "animated model transform cannot be represented by the renderer");
    }
    math::Transform3f result;
    result.position = {static_cast<float>(relative.x), static_cast<float>(relative.y),
                       static_cast<float>(relative.z)};
    result.rotation_degrees = {static_cast<float>(transform.rotation_degrees.x),
                               static_cast<float>(transform.rotation_degrees.y),
                               static_cast<float>(transform.rotation_degrees.z)};
    result.scale = {static_cast<float>(transform.scale.x), static_cast<float>(transform.scale.y),
                    static_cast<float>(transform.scale.z)};
    return core::Result<math::Transform3f>::success(result);
}

[[nodiscard]] renderer::RenderSceneUpdate
remove_object_update(renderer::RenderObjectId id) noexcept {
    renderer::RenderSceneUpdate update;
    update.kind = renderer::RenderSceneUpdateKind::remove_object;
    update.object_id = id;
    return update;
}

[[nodiscard]] renderer::RenderSceneUpdate
remove_palette_update(renderer::RenderSkinPaletteId id) noexcept {
    renderer::RenderSceneUpdate update;
    update.kind = renderer::RenderSceneUpdateKind::remove_skin_palette;
    update.skin_palette_id = id;
    return update;
}

} // namespace

core::Status AnimatedModelPresentationConfig::validate() const {
    auto status = assets::validate_model_asset(model);
    if (!status) {
        return status;
    }
    status = locomotion_clips.validate(model);
    if (!status) {
        return status;
    }
    if (asset_id.empty() || !visual_prototype.is_valid() || !material.is_valid() ||
        !animated_bounds.is_valid() || !finite_color(color) ||
        std::ranges::none_of(model.primitives, [](const assets::ModelPrimitive& primitive) {
            return primitive.skin != assets::no_model_index;
        })) {
        return core::Status::failure(
            "animated_model_presentation.invalid_config",
            "animated presentation requires an id, prototype, material, bounds, and skinned mesh");
    }
    return core::Status::ok();
}

core::Status AnimatedModelPresentation::initialize(renderer::Renderer& renderer,
                                                   AnimatedModelPresentationConfig config) {
    if (initialized_) {
        return core::Status::failure("animated_model_presentation.already_initialized",
                                     "animated model presentation is already initialized");
    }
    auto status = config.validate();
    if (!status) {
        return status;
    }

    std::vector<PrimitiveBinding> uploaded;
    uploaded.reserve(config.model.primitives.size());
    for (std::uint32_t index = 0; index < config.model.primitives.size(); ++index) {
        const auto& primitive = config.model.primitives[index];
        auto mesh = renderer.create_model_primitive(config.asset_id + "#" + std::to_string(index) +
                                                        ":" + primitive.name,
                                                    config.model, index);
        if (!mesh) {
            for (const auto& binding : uploaded) {
                (void)renderer.release_static_mesh(binding.mesh);
            }
            return core::Status::failure(mesh.error().code, mesh.error().message);
        }
        uploaded.push_back({index, mesh.value()});
    }

    config_ = std::move(config);
    primitives_ = std::move(uploaded);
    entities_.clear();
    stats_ = {};
    initialized_ = true;
    return core::Status::ok();
}

core::Result<AnimatedModelPresentationStats>
AnimatedModelPresentation::synchronize(renderer::Renderer& renderer,
                                       const RenderSnapshot& snapshot) {
    if (!initialized_) {
        return core::Result<AnimatedModelPresentationStats>::failure(
            "animated_model_presentation.not_initialized",
            "animated model presentation must be initialized before synchronization");
    }

    AnimatedModelPresentationStats frame_stats;
    std::unordered_set<std::uint64_t> current_entities;
    current_entities.reserve(snapshot.objects.size());
    for (const auto& source : snapshot.objects) {
        if (!source.visible || source.visual_prototype != config_.visual_prototype) {
            continue;
        }
        const auto key = source.source_net_id.value();
        if (!current_entities.insert(key).second) {
            return core::Result<AnimatedModelPresentationStats>::failure(
                "animated_model_presentation.duplicate_entity",
                "render snapshot contains duplicate animated entity ids");
        }
        auto retained = entities_.find(key);
        if (retained != entities_.end() &&
            retained->second.source_revision == source.source_revision) {
            continue;
        }

        auto pose = animation::sample_locomotion_animation(config_.model, config_.locomotion_clips,
                                                           source.current_locomotion,
                                                           snapshot.simulation_tick);
        if (!pose) {
            return core::Result<AnimatedModelPresentationStats>::failure(pose.error().code,
                                                                         pose.error().message);
        }
        ++frame_stats.evaluated_poses;
        auto previous_transform =
            render_transform(source.previous_transform, source.current_transform.position);
        auto current_transform =
            render_transform(source.current_transform, source.current_transform.position);
        if (!previous_transform || !current_transform) {
            const auto& error =
                !previous_transform ? previous_transform.error() : current_transform.error();
            return core::Result<AnimatedModelPresentationStats>::failure(error.code, error.message);
        }

        if (retained == entities_.end()) {
            EntityVisual entity;
            entity.primitives.reserve(primitives_.size());
            const auto rollback_entity = [&](renderer::RenderSkinPaletteId pending_palette = {}) {
                std::vector<renderer::RenderSceneUpdate> rollback;
                rollback.reserve(entity.primitives.size() * 2U +
                                 static_cast<std::size_t>(pending_palette.is_valid()));
                for (const auto& retained_primitive : entity.primitives) {
                    rollback.push_back(remove_object_update(retained_primitive.object));
                }
                for (const auto& retained_primitive : entity.primitives) {
                    if (retained_primitive.palette.is_valid()) {
                        rollback.push_back(remove_palette_update(retained_primitive.palette));
                    }
                }
                if (pending_palette.is_valid()) {
                    rollback.push_back(remove_palette_update(pending_palette));
                }
                (void)renderer.apply_scene_updates(rollback);
            };
            for (const auto& binding : primitives_) {
                const auto& primitive = config_.model.primitives[binding.primitive_index];
                renderer::RenderSkinPaletteId palette_id;
                if (primitive.skin != assets::no_model_index) {
                    auto palette = animation::build_model_space_skinning_palette(
                        config_.model, primitive.skin, primitive.node, pose.value());
                    if (!palette) {
                        rollback_entity();
                        return core::Result<AnimatedModelPresentationStats>::failure(
                            palette.error().code, palette.error().message);
                    }
                    renderer::RenderSkinPaletteProxy palette_proxy;
                    palette_proxy.joint_matrices = std::move(palette).value().joint_matrices;
                    auto created_palette = renderer.create_skin_palette(std::move(palette_proxy));
                    if (!created_palette) {
                        rollback_entity();
                        return core::Result<AnimatedModelPresentationStats>::failure(
                            created_palette.error().code, created_palette.error().message);
                    }
                    palette_id = created_palette.value();
                    ++frame_stats.uploaded_palettes;
                }

                renderer::RenderObjectProxy object;
                object.anchor = source.current_transform.position;
                object.previous_transform = previous_transform.value();
                object.current_transform = current_transform.value();
                object.mesh = binding.mesh;
                object.material = config_.material;
                object.local_bounds = config_.animated_bounds;
                object.skin_palette = palette_id;
                object.layer = config_.layer;
                object.flags = config_.flags;
                if (source.teleported) {
                    object.flags = object.flags | renderer::RenderObjectFlags::teleport;
                }
                object.color = {
                    config_.color[0] * source.color[0], config_.color[1] * source.color[1],
                    config_.color[2] * source.color[2], config_.color[3] * source.color[3]};
                auto created_object = renderer.create_object(std::move(object));
                if (!created_object) {
                    rollback_entity(palette_id);
                    return core::Result<AnimatedModelPresentationStats>::failure(
                        created_object.error().code, created_object.error().message);
                }
                entity.primitives.push_back({created_object.value(), palette_id});
            }
            entity.source_revision = source.source_revision;
            entities_.emplace(key, std::move(entity));
            ++frame_stats.inserted_entities;
            continue;
        }

        std::vector<renderer::RenderSceneUpdate> updates;
        updates.reserve(primitives_.size() * 2U);
        for (std::size_t index = 0; index < primitives_.size(); ++index) {
            const auto& binding = primitives_[index];
            const auto& primitive = config_.model.primitives[binding.primitive_index];
            const auto& visual = retained->second.primitives[index];
            if (primitive.skin != assets::no_model_index) {
                auto palette = animation::build_model_space_skinning_palette(
                    config_.model, primitive.skin, primitive.node, pose.value());
                if (!palette) {
                    return core::Result<AnimatedModelPresentationStats>::failure(
                        palette.error().code, palette.error().message);
                }
                renderer::RenderSceneUpdate palette_update;
                palette_update.kind = renderer::RenderSceneUpdateKind::upsert_skin_palette;
                palette_update.skin_palette.id = visual.palette;
                palette_update.skin_palette.joint_matrices =
                    std::move(palette).value().joint_matrices;
                updates.push_back(std::move(palette_update));
                ++frame_stats.uploaded_palettes;
            }

            renderer::RenderSceneUpdate object_update;
            object_update.kind = renderer::RenderSceneUpdateKind::upsert_object;
            object_update.object.id = visual.object;
            object_update.object.anchor = source.current_transform.position;
            object_update.object.previous_transform = previous_transform.value();
            object_update.object.current_transform = current_transform.value();
            object_update.object.mesh = binding.mesh;
            object_update.object.material = config_.material;
            object_update.object.local_bounds = config_.animated_bounds;
            object_update.object.skin_palette = visual.palette;
            object_update.object.layer = config_.layer;
            object_update.object.flags = config_.flags;
            if (source.teleported) {
                object_update.object.flags =
                    object_update.object.flags | renderer::RenderObjectFlags::teleport;
            }
            object_update.object.color = {
                config_.color[0] * source.color[0], config_.color[1] * source.color[1],
                config_.color[2] * source.color[2], config_.color[3] * source.color[3]};
            updates.push_back(std::move(object_update));
        }
        auto status = renderer.apply_scene_updates(updates);
        if (!status) {
            return core::Result<AnimatedModelPresentationStats>::failure(status.error().code,
                                                                         status.error().message);
        }
        retained->second.source_revision = source.source_revision;
        ++frame_stats.updated_entities;
    }

    std::vector<std::uint64_t> removals;
    for (const auto& [key, entity] : entities_) {
        if (current_entities.contains(key)) {
            continue;
        }
        std::vector<renderer::RenderSceneUpdate> updates;
        updates.reserve(entity.primitives.size() * 2U);
        for (const auto& primitive : entity.primitives) {
            updates.push_back(remove_object_update(primitive.object));
        }
        for (const auto& primitive : entity.primitives) {
            if (primitive.palette.is_valid()) {
                updates.push_back(remove_palette_update(primitive.palette));
            }
        }
        auto status = renderer.apply_scene_updates(updates);
        if (!status) {
            return core::Result<AnimatedModelPresentationStats>::failure(status.error().code,
                                                                         status.error().message);
        }
        removals.push_back(key);
        ++frame_stats.removed_entities;
    }
    for (const auto key : removals) {
        entities_.erase(key);
    }

    frame_stats.retained_entities = static_cast<std::uint32_t>(entities_.size());
    frame_stats.retained_primitives =
        static_cast<std::uint32_t>(entities_.size() * primitives_.size());
    stats_ = frame_stats;
    return core::Result<AnimatedModelPresentationStats>::success(stats_);
}

core::Status AnimatedModelPresentation::shutdown(renderer::Renderer& renderer) {
    if (!initialized_) {
        return core::Status::ok();
    }
    std::vector<renderer::RenderSceneUpdate> updates;
    for (const auto& [key, entity] : entities_) {
        (void)key;
        for (const auto& primitive : entity.primitives) {
            updates.push_back(remove_object_update(primitive.object));
        }
    }
    for (const auto& [key, entity] : entities_) {
        (void)key;
        for (const auto& primitive : entity.primitives) {
            if (primitive.palette.is_valid()) {
                updates.push_back(remove_palette_update(primitive.palette));
            }
        }
    }
    auto status = renderer.apply_scene_updates(updates);
    if (!status) {
        return status;
    }
    for (const auto& primitive : primitives_) {
        status = renderer.release_static_mesh(primitive.mesh);
        if (!status) {
            return status;
        }
    }
    entities_.clear();
    primitives_.clear();
    config_ = {};
    stats_ = {};
    initialized_ = false;
    return core::Status::ok();
}

bool AnimatedModelPresentation::is_initialized() const noexcept {
    return initialized_;
}

const AnimatedModelPresentationStats& AnimatedModelPresentation::stats() const noexcept {
    return stats_;
}

} // namespace heartstead::game
