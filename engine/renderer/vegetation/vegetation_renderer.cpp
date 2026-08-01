#include "engine/renderer/vegetation/vegetation_renderer.hpp"

#include "engine/animation/skeletal_animation.hpp"
#include "engine/assets/asset_cooker.hpp"
#include "engine/assets/cooked_asset_store.hpp"
#include "engine/assets/model_asset.hpp"
#include "engine/renderer/renderer.hpp"
#include "engine/renderer/visibility/hierarchical_depth_occlusion.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heartstead::renderer {

namespace {

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] float random_unit(std::uint64_t seed, std::uint64_t stream) noexcept {
    const auto bits = static_cast<std::uint32_t>(splitmix64(seed ^ splitmix64(stream)) >> 40U);
    return static_cast<float>(bits) / static_cast<float>(0x00FF'FFFFU);
}

[[nodiscard]] float interpolate(float minimum, float maximum, float value) noexcept {
    return minimum + (maximum - minimum) * value;
}

[[nodiscard]] std::array<float, 4> random_color(const VegetationSpecies& species,
                                                std::uint64_t seed,
                                                std::uint64_t index) noexcept {
    std::array<float, 4> result{};
    for (std::size_t component = 0; component < result.size(); ++component) {
        result[component] =
            interpolate(species.color_min[component], species.color_max[component],
                        random_unit(seed, index * 17U + component + 7U));
    }
    return result;
}

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
            "vegetation_renderer.invalid_cooked_model",
            "vegetation model must be produced by the production model cooker: " +
                std::string(logical_id));
    }
    return assets::decode_model_asset(payload.value().bytes);
}

} // namespace

struct VegetationRenderer::Impl {
    struct LoadedPrimitive {
        RenderMeshHandle mesh;
        MaterialRuntimeHandle material;
        RenderLayer layer = RenderLayer::alpha_tested;
        RenderObjectFlags flags =
            RenderObjectFlags::two_sided | RenderObjectFlags::cast_shadow;
        math::Bounds3f bounds{};
        math::Mat4f model_transform = math::Mat4f::identity();
    };

    struct LoadedModel {
        std::shared_ptr<const assets::ModelAsset> source;
        std::vector<LoadedPrimitive> primitives;
    };

    struct RetainedObject {
        RenderObjectProxy proxy;
    };

    struct RetainedPatch {
        VegetationPatchDesc desc;
        math::Vec3f local_center{};
        math::Bounds3f local_bounds{};
        std::vector<RetainedObject> objects;
        std::uint32_t logical_instances = 0;
        std::uint32_t density_rejections = 0;
        bool occluded = false;
    };

    Renderer* renderer = nullptr;
    const VegetationSpeciesRegistry* registry = nullptr;
    VegetationRendererConfig config{};
    std::unordered_map<std::string, LoadedModel> models;
    std::unordered_map<std::uint64_t, RetainedPatch> patches;
    HierarchicalDepthOcclusion occlusion;
    VegetationRendererStats stats{};

    [[nodiscard]] core::Status load_model(const assets::CookedAssetStore& store,
                                          std::string_view logical_id) {
        if (models.contains(std::string(logical_id))) {
            return core::Status::ok();
        }
        auto decoded = load_production_model(store, logical_id);
        if (!decoded) {
            return core::Status::failure(decoded.error().code, decoded.error().message);
        }
        if (std::ranges::any_of(decoded.value().primitives, [](const auto& primitive) {
                return primitive.renderable && primitive.lod_level == 0U &&
                       primitive.skin != assets::no_model_index;
            })) {
            return core::Status::failure(
                "vegetation_renderer.skinned_model",
                "vegetation LOD models must be rigid; use growth states instead of skinning: " +
                    std::string(logical_id));
        }
        auto source =
            std::make_shared<assets::ModelAsset>(std::move(decoded).value());
        auto materials = renderer->create_model_materials(logical_id, *source);
        if (!materials) {
            return core::Status::failure(materials.error().code, materials.error().message);
        }
        auto bind_pose = animation::bind_node_pose(*source);
        auto matrices = animation::evaluate_model_node_matrices(*source, bind_pose);
        if (!matrices) {
            return core::Status::failure(matrices.error().code, matrices.error().message);
        }

        LoadedModel loaded;
        loaded.source = source;
        for (std::uint32_t index = 0; index < source->primitives.size(); ++index) {
            const auto& primitive = source->primitives[index];
            if (!primitive.renderable || primitive.lod_level != 0U) {
                continue;
            }
            auto mesh = renderer->create_model_primitive(
                std::string(logical_id) + "#vegetation:" + std::to_string(index), *source, index);
            if (!mesh) {
                for (const auto& existing : loaded.primitives) {
                    (void)renderer->release_static_mesh(existing.mesh);
                }
                return core::Status::failure(mesh.error().code, mesh.error().message);
            }
            LoadedPrimitive binding;
            binding.mesh = mesh.value();
            binding.bounds = primitive.bounds;
            binding.model_transform = matrices.value()[primitive.node];
            if (primitive.material != assets::no_model_index) {
                const auto& material = materials.value()[primitive.material];
                binding.material = material.material;
                binding.layer = material.layer;
                binding.flags = material.flags | RenderObjectFlags::cast_shadow;
            } else {
                binding.material = renderer->fallback_material();
            }
            // Foliage is intentionally two-sided even if a source author forgot the glTF flag.
            binding.flags = binding.flags | RenderObjectFlags::two_sided;
            loaded.primitives.push_back(binding);
        }
        if (loaded.primitives.empty()) {
            return core::Status::failure(
                "vegetation_renderer.empty_model",
                "vegetation model contains no renderable LOD-0 primitives: " +
                    std::string(logical_id));
        }
        models.emplace(std::string(logical_id), std::move(loaded));
        return core::Status::ok();
    }

    [[nodiscard]] core::Status remove_patch(std::uint64_t patch_id) {
        const auto found = patches.find(patch_id);
        if (found == patches.end()) {
            return core::Status::failure("vegetation_renderer.missing_patch",
                                         "vegetation patch id is not retained");
        }
        std::vector<RenderSceneUpdate> updates;
        updates.reserve(found->second.objects.size());
        for (const auto& object : found->second.objects) {
            RenderSceneUpdate update;
            update.kind = RenderSceneUpdateKind::remove_object;
            update.object_id = object.proxy.id;
            updates.push_back(update);
        }
        auto status = renderer->apply_scene_updates(updates);
        if (!status) {
            return status;
        }
        patches.erase(found);
        occlusion.erase(patch_id);
        refresh_stats();
        return core::Status::ok();
    }

    void refresh_stats() noexcept {
        stats.loaded_species =
            registry == nullptr ? 0U : static_cast<std::uint32_t>(registry->size());
        stats.loaded_models = static_cast<std::uint32_t>(models.size());
        stats.retained_patches = static_cast<std::uint32_t>(patches.size());
        stats.logical_instances = 0;
        stats.render_objects = 0;
        stats.density_rejected_lods = 0;
        stats.occluded_patches = 0;
        for (const auto& [id, patch] : patches) {
            (void)id;
            stats.logical_instances += patch.logical_instances;
            stats.render_objects += static_cast<std::uint32_t>(patch.objects.size());
            stats.density_rejected_lods += patch.density_rejections;
            stats.occluded_patches += patch.occluded ? 1U : 0U;
        }
    }
};

core::Status VegetationPatchDesc::validate() const {
    if (id == 0 || !species.is_valid() || !origin.is_valid() || !extent.is_finite() ||
        extent.x <= 0.0F || extent.y <= 0.0F || instance_count == 0) {
        return core::Status::failure(
            "vegetation_renderer.invalid_patch",
            "vegetation patch requires an id, species, valid origin, positive extent, and plants");
    }
    return core::Status::ok();
}

core::Status VegetationRendererConfig::validate() const {
    if (maximum_patches == 0 || maximum_logical_instances == 0 ||
        maximum_render_objects == 0 || maximum_render_objects < maximum_logical_instances) {
        return core::Status::failure(
            "vegetation_renderer.invalid_config",
            "vegetation renderer capacities must be non-zero and allow at least one object per "
            "logical instance");
    }
    return core::Status::ok();
}

VegetationRenderer::VegetationRenderer() : impl_(std::make_unique<Impl>()) {}
VegetationRenderer::~VegetationRenderer() {
    (void)shutdown();
}
VegetationRenderer::VegetationRenderer(VegetationRenderer&&) noexcept = default;
VegetationRenderer& VegetationRenderer::operator=(VegetationRenderer&&) noexcept = default;

core::Status
VegetationRenderer::initialize(Renderer& renderer, const VegetationSpeciesRegistry& registry,
                               const std::filesystem::path& cooked_asset_root,
                               VegetationRendererConfig config) {
    if (impl_->renderer != nullptr) {
        return core::Status::failure("vegetation_renderer.already_initialized",
                                     "vegetation renderer is already initialized");
    }
    auto status = config.validate();
    if (!status) {
        return status;
    }
    status = impl_->occlusion.initialize();
    if (!status) {
        return status;
    }
    if (!renderer.is_initialized() || registry.size() == 0) {
        return core::Status::failure(
            "vegetation_renderer.missing_dependencies",
            "vegetation renderer requires an initialized renderer and validated species");
    }
    auto store = assets::CookedAssetStore::load(cooked_asset_root);
    if (!store) {
        return core::Status::failure(store.error().code, store.error().message);
    }

    impl_->renderer = &renderer;
    impl_->registry = &registry;
    impl_->config = config;
    for (const auto& species : registry.species()) {
        for (const auto& lod : species.lods) {
            status = impl_->load_model(store.value(), lod.model_asset);
            if (!status) {
                (void)shutdown();
                return status;
            }
        }
        for (const auto& growth : species.growth_states) {
            if (growth.model_override.empty()) {
                continue;
            }
            status = impl_->load_model(store.value(), growth.model_override);
            if (!status) {
                (void)shutdown();
                return status;
            }
        }
    }
    impl_->refresh_stats();
    return core::Status::ok();
}

core::Status VegetationRenderer::upsert_patch(VegetationPatchDesc patch,
                                               VegetationHeightSampler height_sampler) {
    if (!is_initialized()) {
        return core::Status::failure("vegetation_renderer.not_initialized",
                                     "vegetation renderer must be initialized first");
    }
    auto status = patch.validate();
    if (!status) {
        return status;
    }
    const auto* species = impl_->registry->find(patch.species);
    if (species == nullptr) {
        return core::Status::failure("vegetation_renderer.missing_species",
                                     "vegetation patch references an unknown species");
    }
    const auto* growth =
        patch.growth_state.empty() ? nullptr : species->growth_state(patch.growth_state);
    if (!patch.growth_state.empty() && growth == nullptr) {
        return core::Status::failure("vegetation_renderer.missing_growth_state",
                                     "vegetation patch references an unknown growth state");
    }
    const auto existing = impl_->patches.find(patch.id);
    const auto existing_logical =
        existing == impl_->patches.end() ? 0U : existing->second.logical_instances;
    if (impl_->stats.logical_instances - existing_logical + patch.instance_count >
        impl_->config.maximum_logical_instances) {
        return core::Status::failure("vegetation_renderer.instance_capacity",
                                     "vegetation patch exceeds logical instance capacity");
    }
    if (existing != impl_->patches.end()) {
        status = impl_->remove_patch(patch.id);
        if (!status) {
            return status;
        }
    }

    Impl::RetainedPatch retained;
    retained.desc = patch;
    retained.local_center = {patch.extent.x * 0.5F, 0.0F, patch.extent.y * 0.5F};
    retained.logical_instances = patch.instance_count;
    const auto growth_scale = growth == nullptr ? 1.0F : growth->scale_multiplier;
    bool has_local_bounds = false;

    const auto rollback = [&]() {
        std::vector<RenderSceneUpdate> updates;
        updates.reserve(retained.objects.size());
        for (const auto& object : retained.objects) {
            RenderSceneUpdate update;
            update.kind = RenderSceneUpdateKind::remove_object;
            update.object_id = object.proxy.id;
            updates.push_back(update);
        }
        (void)impl_->renderer->apply_scene_updates(updates);
    };

    for (std::uint32_t instance_index = 0; instance_index < patch.instance_count;
         ++instance_index) {
        const auto local_x =
            random_unit(patch.seed, static_cast<std::uint64_t>(instance_index) * 31U + 1U) *
            patch.extent.x;
        const auto local_z =
            random_unit(patch.seed, static_cast<std::uint64_t>(instance_index) * 31U + 2U) *
            patch.extent.y;
        const auto local_y = height_sampler ? height_sampler(local_x, local_z) : 0.0F;
        if (!std::isfinite(local_y)) {
            rollback();
            return core::Status::failure("vegetation_renderer.invalid_height",
                                         "vegetation height sampler returned a non-finite value");
        }
        auto position = world::WorldPosition::from_anchor(
            patch.origin.anchor,
            patch.origin.local_offset +
                math::Vec3d{static_cast<double>(local_x), static_cast<double>(local_y),
                            static_cast<double>(local_z)});
        if (!position) {
            rollback();
            return core::Status::failure(position.error().code, position.error().message);
        }
        auto scale =
            interpolate(species->scale_min, species->scale_max,
                        random_unit(patch.seed,
                                    static_cast<std::uint64_t>(instance_index) * 31U + 3U)) *
            growth_scale;
        const auto mirrored =
            species->mirror_variation &&
            random_unit(patch.seed, static_cast<std::uint64_t>(instance_index) * 31U + 4U) <
                0.5F;
        const auto yaw =
            (random_unit(patch.seed, static_cast<std::uint64_t>(instance_index) * 31U + 5U) -
             0.5F) *
            species->yaw_variation_degrees;
        const auto color = random_color(*species, patch.seed, instance_index);
        const auto wind_phase =
            random_unit(patch.seed, static_cast<std::uint64_t>(instance_index) * 31U + 6U) *
            2.0F * std::numbers::pi_v<float>;
        const auto density_cutoff =
            species->density_fade_start +
            (species->density_fade_end - species->density_fade_start) *
                random_unit(patch.seed,
                            static_cast<std::uint64_t>(instance_index) * 31U + 7U);

        float minimum_distance = 0.0F;
        for (std::size_t lod_index = 0; lod_index < species->lods.size(); ++lod_index) {
            const auto& lod = species->lods[lod_index];
            const auto density_random =
                random_unit(patch.seed,
                            static_cast<std::uint64_t>(instance_index) * 131U + lod_index + 17U);
            if (density_random > lod.density) {
                ++retained.density_rejections;
                minimum_distance = lod.maximum_distance - lod.transition_width;
                continue;
            }
            const auto maximum_distance = std::min(lod.maximum_distance, density_cutoff);
            if (maximum_distance <= minimum_distance + 0.001F) {
                ++retained.density_rejections;
                minimum_distance = lod.maximum_distance - lod.transition_width;
                continue;
            }
            const auto model_id =
                growth != nullptr && !growth->model_override.empty()
                    ? std::string_view{growth->model_override}
                    : std::string_view{lod.model_asset};
            const auto model = impl_->models.find(std::string(model_id));
            if (model == impl_->models.end()) {
                rollback();
                return core::Status::failure("vegetation_renderer.unloaded_model",
                                             "vegetation LOD model was not loaded");
            }
            for (const auto& primitive : model->second.primitives) {
                if (impl_->stats.render_objects + retained.objects.size() + 1U >
                    impl_->config.maximum_render_objects) {
                    rollback();
                    return core::Status::failure(
                        "vegetation_renderer.object_capacity",
                        "vegetation patch exceeds retained render-object capacity");
                }
                RenderObjectProxy object;
                object.anchor = position.value();
                object.current_transform.rotation_degrees.y = yaw;
                object.current_transform.scale = {mirrored ? -scale : scale, scale, scale};
                object.previous_transform = object.current_transform;
                object.model_transform = primitive.model_transform;
                object.mesh = primitive.mesh;
                object.material = primitive.material;
                object.local_bounds = primitive.bounds;
                object.layer = primitive.layer;
                object.flags = primitive.flags;
                if (lod_index > species->shadow_lod) {
                    object.flags = static_cast<RenderObjectFlags>(
                        static_cast<std::uint32_t>(object.flags) &
                        ~static_cast<std::uint32_t>(RenderObjectFlags::cast_shadow));
                }
                object.color = color;
                object.minimum_view_distance = std::max(0.0F, minimum_distance);
                object.maximum_view_distance = maximum_distance;
                object.distance_fade_width =
                    std::min(lod.transition_width,
                             maximum_distance - object.minimum_view_distance);
                object.effect_flags =
                    RenderEffectFlags::vegetation | RenderEffectFlags::foliage_transmission;
                if (lod.impostor) {
                    object.effect_flags =
                        object.effect_flags | RenderEffectFlags::billboard;
                }
                if (!species->receives_weather) {
                    object.effect_flags =
                        object.effect_flags | RenderEffectFlags::disable_weather_response;
                }
                object.wind_phase = wind_phase;
                object.wind_stiffness = species->wind_stiffness;
                object.foliage_transmission = species->foliage_transmission;
                const auto anchor_relative = position.value().relative_to(patch.origin.anchor) -
                                             patch.origin.local_offset;
                const math::Vec3f local_offset{static_cast<float>(anchor_relative.x),
                                               static_cast<float>(anchor_relative.y),
                                               static_cast<float>(anchor_relative.z)};
                auto object_bounds = math::transform_bounds(
                    math::transform_matrix(object.current_transform) * object.model_transform,
                    object.local_bounds);
                object_bounds.min += local_offset;
                object_bounds.max += local_offset;
                if (!has_local_bounds) {
                    retained.local_bounds = object_bounds;
                    has_local_bounds = true;
                } else {
                    retained.local_bounds.min.x =
                        std::min(retained.local_bounds.min.x, object_bounds.min.x);
                    retained.local_bounds.min.y =
                        std::min(retained.local_bounds.min.y, object_bounds.min.y);
                    retained.local_bounds.min.z =
                        std::min(retained.local_bounds.min.z, object_bounds.min.z);
                    retained.local_bounds.max.x =
                        std::max(retained.local_bounds.max.x, object_bounds.max.x);
                    retained.local_bounds.max.y =
                        std::max(retained.local_bounds.max.y, object_bounds.max.y);
                    retained.local_bounds.max.z =
                        std::max(retained.local_bounds.max.z, object_bounds.max.z);
                }
                auto created = impl_->renderer->create_object(object);
                if (!created) {
                    rollback();
                    return core::Status::failure(created.error().code,
                                                 created.error().message);
                }
                object.id = created.value();
                retained.objects.push_back({std::move(object)});
            }
            minimum_distance = lod.maximum_distance - lod.transition_width;
        }
    }
    impl_->patches.emplace(patch.id, std::move(retained));
    impl_->refresh_stats();
    return core::Status::ok();
}

core::Status VegetationRenderer::remove_patch(std::uint64_t patch_id) {
    if (!is_initialized()) {
        return core::Status::failure("vegetation_renderer.not_initialized",
                                     "vegetation renderer must be initialized first");
    }
    return impl_->remove_patch(patch_id);
}

core::Status
VegetationRenderer::update_occlusion(const RenderCamera& camera,
                                     std::span<const math::Bounds3f> occluders) {
    if (!is_initialized()) {
        return core::Status::failure("vegetation_renderer.not_initialized",
                                     "vegetation renderer must be initialized first");
    }
    auto rebuild_status = impl_->occlusion.rebuild(camera, occluders);
    if (!rebuild_status) {
        return rebuild_status;
    }
    for (auto& [id, patch] : impl_->patches) {
        (void)id;
        auto origin =
            world::to_camera_relative(patch.desc.origin, camera.floating_origin);
        if (!origin) {
            continue;
        }
        const math::Bounds3f bounds{patch.local_bounds.min + origin.value(),
                                    patch.local_bounds.max + origin.value()};
        const auto occluded = impl_->occlusion.query(patch.desc.id, bounds);
        if (occluded == patch.occluded) {
            continue;
        }
        std::vector<RenderSceneUpdate> updates;
        updates.reserve(patch.objects.size());
        for (auto& retained : patch.objects) {
            auto& object = retained.proxy;
            if (occluded) {
                object.flags = object.flags | RenderObjectFlags::hidden;
            } else {
                object.flags = static_cast<RenderObjectFlags>(
                    static_cast<std::uint32_t>(object.flags) &
                    ~static_cast<std::uint32_t>(RenderObjectFlags::hidden));
            }
            RenderSceneUpdate update;
            update.kind = RenderSceneUpdateKind::upsert_object;
            update.object = object;
            updates.push_back(std::move(update));
        }
        auto status = impl_->renderer->apply_scene_updates(updates);
        if (!status) {
            return status;
        }
        patch.occluded = occluded;
        ++impl_->stats.visibility_updates;
    }
    const auto updates = impl_->stats.visibility_updates;
    impl_->refresh_stats();
    impl_->stats.visibility_updates = updates;
    return core::Status::ok();
}

core::Status VegetationRenderer::shutdown() {
    if (!impl_ || impl_->renderer == nullptr) {
        return core::Status::ok();
    }
    std::vector<RenderSceneUpdate> updates;
    for (const auto& [id, patch] : impl_->patches) {
        (void)id;
        for (const auto& object : patch.objects) {
            RenderSceneUpdate update;
            update.kind = RenderSceneUpdateKind::remove_object;
            update.object_id = object.proxy.id;
            updates.push_back(update);
        }
    }
    auto status = impl_->renderer->apply_scene_updates(updates);
    for (const auto& [id, model] : impl_->models) {
        (void)id;
        for (const auto& primitive : model.primitives) {
            auto release = impl_->renderer->release_static_mesh(primitive.mesh);
            if (status && !release) {
                status = release;
            }
        }
    }
    impl_->patches.clear();
    impl_->occlusion.reset_history();
    impl_->models.clear();
    impl_->renderer = nullptr;
    impl_->registry = nullptr;
    impl_->config = {};
    impl_->stats = {};
    return status;
}

bool VegetationRenderer::is_initialized() const noexcept {
    return impl_ && impl_->renderer != nullptr;
}

const VegetationRendererStats& VegetationRenderer::stats() const noexcept {
    return impl_->stats;
}

} // namespace heartstead::renderer
