#include "game/presentation/animated_model_presentation.hpp"

#include "engine/animation/skeletal_animation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>

namespace heartstead::game {

namespace {

[[nodiscard]] bool finite_color(const std::array<float, 4>& color) noexcept {
    return std::ranges::all_of(
        color, [](float value) { return std::isfinite(value) && value >= 0.0F && value <= 1.0F; });
}

[[nodiscard]] bool has_any_locomotion_clip(const animation::LocomotionClipSet& clips) noexcept {
    return clips.idle != assets::no_model_index || clips.walk != assets::no_model_index ||
           clips.run != assets::no_model_index || clips.jump != assets::no_model_index ||
           clips.fall != assets::no_model_index || clips.swim != assets::no_model_index;
}

[[nodiscard]] bool has_all_locomotion_clips(const animation::LocomotionClipSet& clips) noexcept {
    return clips.idle != assets::no_model_index && clips.walk != assets::no_model_index &&
           clips.run != assets::no_model_index && clips.jump != assets::no_model_index &&
           clips.fall != assets::no_model_index && clips.swim != assets::no_model_index;
}

[[nodiscard]] math::Bounds3f primitive_local_bounds(const assets::ModelAsset& model,
                                                    const assets::ModelPrimitive& primitive) {
    const auto& first = model.vertices[primitive.first_vertex].position;
    math::Bounds3f result{first, first};
    for (std::size_t offset = 0; offset < primitive.vertex_count; ++offset) {
        const auto& position = model.vertices[primitive.first_vertex + offset].position;
        math::Vec3f maximum_delta{};
        for (const auto& target : primitive.morph_targets) {
            if (target.position_deltas.empty()) {
                continue;
            }
            const auto delta = target.position_deltas[offset];
            maximum_delta.x += std::abs(delta.x);
            maximum_delta.y += std::abs(delta.y);
            maximum_delta.z += std::abs(delta.z);
        }
        result.min = math::component_min(result.min, position - maximum_delta);
        result.max = math::component_max(result.max, position + maximum_delta);
    }
    return result;
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
    if (model == nullptr) {
        return core::Status::failure("animated_model_presentation.missing_model",
                                     "model presentation requires shared model data");
    }
    auto status = assets::validate_model_asset(*model);
    if (!status) {
        return status;
    }
    const auto any_locomotion_clip = has_any_locomotion_clip(locomotion_clips);
    if (any_locomotion_clip != has_all_locomotion_clips(locomotion_clips)) {
        return core::Status::failure(
            "animated_model_presentation.partial_locomotion_clips",
            "animated model presentation must map either all locomotion roles or none");
    }
    if (any_locomotion_clip) {
        if (!assets::model_capabilities(*model).has_animation_clips) {
            return core::Status::failure(
                "animated_model_presentation.model_has_no_animation_clips",
                "animated model presentation cannot map clips on a model without animation");
        }
        status = locomotion_clips.validate(*model);
        if (!status) {
            return status;
        }
    }
    if (asset_id.empty() || !visual_prototype.is_valid() || !animated_bounds.is_valid() ||
        !std::isfinite(model_scale) || model_scale <= 0.0F || model_scale > 100.0F ||
        !std::isfinite(bounds_padding) || bounds_padding < 0.0F || bounds_padding > 100.0F ||
        !std::isfinite(minimum_view_distance) || !std::isfinite(maximum_view_distance) ||
        minimum_view_distance < 0.0F ||
        (maximum_view_distance != 0.0F && maximum_view_distance <= minimum_view_distance) ||
        !finite_color(color)) {
        return core::Status::failure(
            "animated_model_presentation.invalid_config",
            "model presentation requires an id, prototype, finite positive scale, bounds/padding, "
            "and color");
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

    const auto& model = *config.model;
    auto bind_pose = animation::bind_node_pose(model);
    auto bind_node_matrices = animation::evaluate_model_node_matrices(model, bind_pose);
    if (!bind_node_matrices) {
        return core::Status::failure(bind_node_matrices.error().code,
                                     bind_node_matrices.error().message);
    }
    std::vector<renderer::ModelRenderMaterialBinding> model_materials;
    if (!config.material.is_valid()) {
        auto created_materials = renderer.create_model_materials(config.asset_id, model);
        if (!created_materials) {
            return core::Status::failure(created_materials.error().code,
                                         created_materials.error().message);
        }
        model_materials = std::move(created_materials).value();
    }

    std::vector<PrimitiveBinding> uploaded;
    uploaded.reserve(model.primitives.size());
    for (std::uint32_t index = 0; index < model.primitives.size(); ++index) {
        const auto& primitive = model.primitives[index];
        if (!primitive.renderable || primitive.lod_level != 0U) {
            continue;
        }
        const auto& node = model.nodes[primitive.node];
        if (config.hidden_model_nodes.contains(primitive.name) ||
            config.hidden_model_nodes.contains(node.name)) {
            continue;
        }
        auto mesh = renderer.create_model_primitive(
            config.asset_id + "#" + std::to_string(index) + ":" + primitive.name, model, index);
        if (!mesh) {
            for (const auto& binding : uploaded) {
                (void)renderer.release_static_mesh(binding.mesh);
            }
            return core::Status::failure(mesh.error().code, mesh.error().message);
        }
        PrimitiveBinding binding;
        binding.primitive_index = index;
        binding.mesh = mesh.value();
        binding.material = config.material;
        binding.layer = config.layer;
        binding.flags = config.flags;
        binding.local_bounds =
            primitive.skin == assets::no_model_index
                ? primitive_local_bounds(model, primitive).expanded(config.bounds_padding)
                : config.animated_bounds;
        if (!binding.material.is_valid() && primitive.material != assets::no_model_index) {
            const auto& model_material = model_materials[primitive.material];
            binding.material = model_material.material;
            binding.layer = model_material.layer;
            binding.flags = binding.flags | model_material.flags;
        }
        if (!binding.material.is_valid()) {
            binding.material = renderer.fallback_material();
        }
        uploaded.push_back(binding);
    }

    config_ = std::move(config);
    primitives_ = std::move(uploaded);
    bind_pose_ = std::move(bind_pose);
    bind_node_matrices_ = std::move(bind_node_matrices).value();
    model_scale_matrix_ =
        math::scale_matrix({config_.model_scale, config_.model_scale, config_.model_scale});
    entities_.clear();
    stats_ = {};
    has_active_animation_ = has_any_locomotion_clip(config_.locomotion_clips);
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
    const auto& model = *config_.model;
    std::vector<animation::AnimationBudgetCandidate> animation_candidates;
    animation_candidates.reserve(snapshot.objects.size());
    for (const auto& source : snapshot.objects) {
        if (!source.visible || source.visual_prototype != config_.visual_prototype ||
            (config_.object_filter && !config_.object_filter(source))) {
            continue;
        }
        const auto key = source.source_net_id.value();
        const auto retained = entities_.find(key);
        animation_candidates.push_back({
            .object_id = key,
            .distance_squared = config_.animation_distance_squared
                                    ? config_.animation_distance_squared(source)
                                    : 0.0F,
            .importance = source.animation_importance,
            .visible = true,
            .force_update = retained == entities_.end() || source.teleported,
        });
    }
    const auto budget_decisions = animation::schedule_animation_updates(
        animation_candidates, config_.animation_budget, snapshot.simulation_tick);
    std::unordered_map<std::uint64_t, animation::AnimationBudgetDecision> budget_by_entity;
    budget_by_entity.reserve(budget_decisions.size());
    for (const auto& decision : budget_decisions) {
        budget_by_entity.emplace(decision.object_id, decision);
    }
    std::unordered_set<std::uint64_t> current_entities;
    current_entities.reserve(snapshot.objects.size());
    for (const auto& source : snapshot.objects) {
        if (!source.visible || source.visual_prototype != config_.visual_prototype ||
            (config_.object_filter && !config_.object_filter(source))) {
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

        const animation::NodePose* pose = &bind_pose_;
        const std::vector<math::Mat4f>* node_matrices = &bind_node_matrices_;
        std::optional<animation::NodePose> animated_pose;
        std::optional<std::vector<math::Mat4f>> animated_node_matrices;
        const auto budget = budget_by_entity.find(key);
        const auto evaluate_pose =
            retained == entities_.end() || budget == budget_by_entity.end() ||
            budget->second.evaluate_pose;
        if (has_active_animation_ && evaluate_pose) {
            const StateAnimationClipBinding* state_animation = nullptr;
            for (const auto& binding : config_.state_animation_clips) {
                const auto matches = std::ranges::any_of(
                    source.visual_states, [&](const entities::VisualStateValue& state) {
                        return state.channel == binding.channel && state.value == binding.value;
                    });
                if (matches &&
                    (state_animation == nullptr || binding.priority > state_animation->priority ||
                     (binding.priority == state_animation->priority &&
                      std::tie(binding.channel, binding.value) >
                          std::tie(state_animation->channel, state_animation->value)))) {
                    state_animation = &binding;
                }
            }
            const auto state_key =
                state_animation == nullptr
                    ? std::string{}
                    : state_animation->channel + ":" + state_animation->value;
            core::Result<animation::NodePose> sampled =
                state_animation == nullptr
                    ? animation::sample_locomotion_animation(model, config_.locomotion_clips,
                                                             source.current_locomotion,
                                                             snapshot.simulation_tick)
                    : animation::sample_animation_clip(
                          model,
                          {.clip = state_animation->clip,
                           .time_seconds = static_cast<float>(snapshot.simulation_tick % 216'000U) /
                                           60.0F,
                           .looping = true});
            if (!sampled) {
                return core::Result<AnimatedModelPresentationStats>::failure(
                    sampled.error().code, sampled.error().message);
            }
            animated_pose.emplace(std::move(sampled).value());
            if (retained != entities_.end() &&
                retained->second.retained_state_animation != state_key) {
                retained->second.state_transition_source = retained->second.retained_pose;
                retained->second.state_transition_start_tick = snapshot.simulation_tick;
            }
            if (retained != entities_.end() && config_.state_transition_ticks > 0U &&
                snapshot.simulation_tick >= retained->second.state_transition_start_tick) {
                const auto elapsed =
                    snapshot.simulation_tick - retained->second.state_transition_start_tick;
                if (elapsed < config_.state_transition_ticks &&
                    retained->second.state_transition_source.local_transforms.size() ==
                        model.nodes.size()) {
                    const auto blend = static_cast<float>(elapsed) /
                                       static_cast<float>(config_.state_transition_ticks);
                    auto blended = animation::blend_node_poses(
                        model, retained->second.state_transition_source, *animated_pose, blend);
                    if (!blended) {
                        return core::Result<AnimatedModelPresentationStats>::failure(
                            blended.error().code, blended.error().message);
                    }
                    *animated_pose = std::move(blended).value();
                    ++frame_stats.interpolated_poses;
                }
            }
            if (config_.ignore_horizontal_root_motion) {
                auto root_motion = animation::apply_root_motion_policy(
                    model, *animated_pose,
                    animation::RootMotionPolicy::ignore_horizontal_translation);
                if (!root_motion) {
                    return core::Result<AnimatedModelPresentationStats>::failure(
                        root_motion.error().code, root_motion.error().message);
                }
            }
            std::vector<animation::AnimationLayer> layers;
            layers.reserve(source.animation_layers.size());
            for (const auto& source_layer : source.animation_layers) {
                const auto clip = config_.animation_clips.find(source_layer.clip_role);
                if (clip == config_.animation_clips.end() || clip->second >= model.animations.size()) {
                    return core::Result<AnimatedModelPresentationStats>::failure(
                        "animated_model_presentation.unknown_layer_clip",
                        "animation layer references an unmapped clip role");
                }
                const animation::AnimationNodeMask* mask = nullptr;
                if (!source_layer.mask.empty()) {
                    const auto found_mask = config_.animation_masks.find(source_layer.mask);
                    if (found_mask == config_.animation_masks.end()) {
                        return core::Result<AnimatedModelPresentationStats>::failure(
                            "animated_model_presentation.unknown_layer_mask",
                            "animation layer references an unmapped node mask");
                    }
                    mask = &found_mask->second;
                }
                const auto phase = static_cast<float>(source_layer.normalized_phase) / 65535.0F;
                layers.push_back({
                    .playback = {
                        .clip = clip->second,
                        .time_seconds = model.animations[clip->second].duration_seconds * phase,
                        .looping = source_layer.looping,
                    },
                    .mode = source_layer.additive ? animation::AnimationLayerMode::additive
                                                  : animation::AnimationLayerMode::override_pose,
                    .weight = static_cast<float>(source_layer.weight) / 65535.0F,
                    .mask = mask,
                    .additive_reference = std::nullopt,
                });

                if (config_.animation_event_sink && retained != entities_.end()) {
                    const auto previous =
                        retained->second.retained_layer_phases.find(source_layer.clip_role);
                    const auto event_set = config_.animation_events.find(source_layer.clip_role);
                    if (previous != retained->second.retained_layer_phases.end() &&
                        event_set != config_.animation_events.end()) {
                        auto crossed = animation::crossed_animation_events(
                            event_set->second, previous->second, phase, source_layer.looping);
                        if (!crossed) {
                            return core::Result<AnimatedModelPresentationStats>::failure(
                                crossed.error().code, crossed.error().message);
                        }
                        for (const auto& event_name : crossed.value()) {
                            config_.animation_event_sink(source.source_net_id, event_name);
                        }
                    }
                }
            }
            if (!layers.empty()) {
                auto composed = animation::compose_animation_layers(model, *animated_pose, layers);
                if (!composed) {
                    return core::Result<AnimatedModelPresentationStats>::failure(
                        composed.error().code, composed.error().message);
                }
                *animated_pose = std::move(composed).value();
            }
            auto evaluated = animation::evaluate_model_node_matrices(model, *animated_pose);
            if (!evaluated) {
                return core::Result<AnimatedModelPresentationStats>::failure(
                    evaluated.error().code, evaluated.error().message);
            }
            animated_node_matrices.emplace(std::move(evaluated).value());
            pose = &*animated_pose;
            node_matrices = &*animated_node_matrices;
            ++frame_stats.evaluated_poses;
        } else if (has_active_animation_ && retained != entities_.end()) {
            pose = &retained->second.retained_pose;
            node_matrices = &retained->second.retained_node_matrices;
            ++frame_stats.deferred_pose_evaluations;
        }
        std::optional<std::vector<math::Mat4f>> overridden_node_matrices;
        if (config_.node_matrices_override) {
            auto overridden = config_.node_matrices_override(source);
            if (!overridden) {
                return core::Result<AnimatedModelPresentationStats>::failure(
                    overridden.error().code, overridden.error().message);
            }
            if (overridden.value().size() != model.nodes.size()) {
                return core::Result<AnimatedModelPresentationStats>::failure(
                    "animated_model_presentation.invalid_node_override",
                    "external node matrix override must match the attached model node count");
            }
            overridden_node_matrices.emplace(std::move(overridden).value());
            node_matrices = &*overridden_node_matrices;
        }
        auto previous_transform =
            render_transform(source.previous_transform, source.current_transform.position);
        auto current_transform =
            render_transform(source.current_transform, source.current_transform.position);
        if (!previous_transform || !current_transform) {
            const auto& error =
                !previous_transform ? previous_transform.error() : current_transform.error();
            return core::Result<AnimatedModelPresentationStats>::failure(error.code, error.message);
        }
        math::Mat4f instance_model_transform = math::Mat4f::identity();
        if (config_.instance_model_transform) {
            auto resolved_transform = config_.instance_model_transform(source);
            if (!resolved_transform) {
                return core::Result<AnimatedModelPresentationStats>::failure(
                    resolved_transform.error().code, resolved_transform.error().message);
            }
            instance_model_transform = resolved_transform.value();
        }
        if (config_.pose_sink) {
            config_.pose_sink(source.source_net_id, *node_matrices);
        }

        if (retained == entities_.end()) {
            EntityVisual entity;
            entity.retained_pose = *pose;
            entity.retained_node_matrices = *node_matrices;
            entity.last_evaluated_tick = snapshot.simulation_tick;
            const StateAnimationClipBinding* initial_state_animation = nullptr;
            for (const auto& binding : config_.state_animation_clips) {
                if (std::ranges::any_of(
                        source.visual_states, [&](const entities::VisualStateValue& state) {
                            return state.channel == binding.channel && state.value == binding.value;
                        }) &&
                    (initial_state_animation == nullptr ||
                     binding.priority > initial_state_animation->priority)) {
                    initial_state_animation = &binding;
                }
            }
            entity.retained_state_animation =
                initial_state_animation == nullptr
                    ? std::string{}
                    : initial_state_animation->channel + ":" + initial_state_animation->value;
            for (const auto& layer : source.animation_layers) {
                entity.retained_layer_phases[layer.clip_role] =
                    static_cast<float>(layer.normalized_phase) / 65535.0F;
            }
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
                const auto& primitive = model.primitives[binding.primitive_index];
                const auto node_visible =
                    !config_.model_node_visibility ||
                    config_.model_node_visibility(source, model.nodes[primitive.node].name);
                renderer::RenderSkinPaletteId palette_id;
                if (primitive.skin != assets::no_model_index) {
                    auto palette = animation::build_model_space_skinning_palette(
                        model, primitive.skin, primitive.node, *node_matrices);
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
                object.model_transform =
                    model_scale_matrix_ * instance_model_transform *
                    (primitive.skin == assets::no_model_index
                         ? (*node_matrices)[primitive.node]
                         : math::Mat4f::identity());
                object.mesh = binding.mesh;
                object.material = binding.material;
                object.local_bounds = binding.local_bounds;
                object.skin_palette = palette_id;
                object.morph_weights = pose->morph_weights[primitive.node];
                object.layer = binding.layer;
                object.flags = binding.flags;
                if (source.teleported) {
                    object.flags = object.flags | renderer::RenderObjectFlags::teleport;
                }
                object.color = {
                    config_.color[0] * source.color[0], config_.color[1] * source.color[1],
                    config_.color[2] * source.color[2], config_.color[3] * source.color[3]};
                object.minimum_view_distance =
                    node_visible ? config_.minimum_view_distance
                                 : std::numeric_limits<float>::max();
                object.maximum_view_distance = config_.maximum_view_distance;
                object.use_object_origin_for_view_distance = true;
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
            const auto& primitive = model.primitives[binding.primitive_index];
            const auto node_visible =
                !config_.model_node_visibility ||
                config_.model_node_visibility(source, model.nodes[primitive.node].name);
            const auto& visual = retained->second.primitives[index];
            if (primitive.skin != assets::no_model_index) {
                auto palette = animation::build_model_space_skinning_palette(
                    model, primitive.skin, primitive.node, *node_matrices);
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
            object_update.object.model_transform =
                model_scale_matrix_ * instance_model_transform *
                (primitive.skin == assets::no_model_index
                     ? (*node_matrices)[primitive.node]
                     : math::Mat4f::identity());
            object_update.object.mesh = binding.mesh;
            object_update.object.material = binding.material;
            object_update.object.local_bounds = binding.local_bounds;
            object_update.object.skin_palette = visual.palette;
            object_update.object.morph_weights = pose->morph_weights[primitive.node];
            object_update.object.layer = binding.layer;
            object_update.object.flags = binding.flags;
            if (source.teleported) {
                object_update.object.flags =
                    object_update.object.flags | renderer::RenderObjectFlags::teleport;
            }
            object_update.object.color = {
                config_.color[0] * source.color[0], config_.color[1] * source.color[1],
                config_.color[2] * source.color[2], config_.color[3] * source.color[3]};
            object_update.object.minimum_view_distance =
                node_visible ? config_.minimum_view_distance
                             : std::numeric_limits<float>::max();
            object_update.object.maximum_view_distance = config_.maximum_view_distance;
            object_update.object.use_object_origin_for_view_distance = true;
            updates.push_back(std::move(object_update));
        }
        auto status = renderer.apply_scene_updates(updates);
        if (!status) {
            return core::Result<AnimatedModelPresentationStats>::failure(status.error().code,
                                                                         status.error().message);
        }
        retained->second.source_revision = source.source_revision;
        if (evaluate_pose) {
            retained->second.retained_pose = *pose;
            retained->second.retained_node_matrices = *node_matrices;
            retained->second.last_evaluated_tick = snapshot.simulation_tick;
            const StateAnimationClipBinding* retained_state_animation = nullptr;
            for (const auto& binding : config_.state_animation_clips) {
                if (std::ranges::any_of(
                        source.visual_states, [&](const entities::VisualStateValue& state) {
                            return state.channel == binding.channel && state.value == binding.value;
                        }) &&
                    (retained_state_animation == nullptr ||
                     binding.priority > retained_state_animation->priority)) {
                    retained_state_animation = &binding;
                }
            }
            retained->second.retained_state_animation =
                retained_state_animation == nullptr
                    ? std::string{}
                    : retained_state_animation->channel + ":" + retained_state_animation->value;
            retained->second.retained_layer_phases.clear();
            for (const auto& layer : source.animation_layers) {
                retained->second.retained_layer_phases[layer.clip_role] =
                    static_cast<float>(layer.normalized_phase) / 65535.0F;
            }
        }
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
    bind_pose_ = {};
    bind_node_matrices_.clear();
    model_scale_matrix_ = math::Mat4f::identity();
    config_ = {};
    stats_ = {};
    has_active_animation_ = false;
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
