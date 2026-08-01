#include "engine/renderer/scene/render_scene.hpp"

#include "engine/renderer/camera/frustum.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iterator>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace heartstead::renderer {

namespace {

[[nodiscard]] bool finite_color(const std::array<float, 4>& color) noexcept {
    return std::ranges::all_of(
        color, [](float value) { return std::isfinite(value) && value >= 0.0F && value <= 4.0F; });
}

[[nodiscard]] float lerp(float left, float right, float alpha) noexcept {
    return left + (right - left) * alpha;
}

[[nodiscard]] float interpolate_degrees(float previous, float current, float alpha) noexcept {
    auto delta = std::fmod(current - previous, 360.0F);
    if (delta > 180.0F) {
        delta -= 360.0F;
    } else if (delta < -180.0F) {
        delta += 360.0F;
    }
    return previous + delta * alpha;
}

[[nodiscard]] constexpr VisibilityKey visibility_key(RenderObjectId id) noexcept {
    return (static_cast<std::uint64_t>(id.generation) << 32U) | id.index;
}

template <typename Slot, typename Id>
[[nodiscard]] const Slot* find_slot(const std::vector<Slot>& slots, Id id) noexcept {
    if (!id.is_valid() || id.index > slots.size()) {
        return nullptr;
    }
    const auto& slot = slots[id.index - 1U];
    return slot.occupied && slot.generation == id.generation ? &slot : nullptr;
}

template <typename Slot, typename Id>
[[nodiscard]] Slot* find_reserved_slot(std::vector<Slot>& slots, Id id) noexcept {
    if (!id.is_valid() || id.index > slots.size()) {
        return nullptr;
    }
    auto& slot = slots[id.index - 1U];
    return slot.reserved && slot.generation == id.generation ? &slot : nullptr;
}

template <typename Slot, typename Id>
[[nodiscard]] Id reserve_id(std::vector<Slot>& slots, std::vector<std::uint32_t>& free_slots) {
    std::uint32_t slot_index = 0;
    if (!free_slots.empty()) {
        slot_index = free_slots.back();
        free_slots.pop_back();
    } else {
        if (slots.size() >= std::numeric_limits<std::uint32_t>::max()) {
            return {};
        }
        slots.emplace_back();
        slot_index = static_cast<std::uint32_t>(slots.size() - 1U);
    }
    auto& slot = slots[slot_index];
    slot.reserved = true;
    slot.occupied = false;
    return {slot_index + 1U, slot.generation};
}

template <typename Slot, typename Id>
void release_slot(std::vector<Slot>& slots, std::vector<std::uint32_t>& free_slots, Id id) {
    auto& slot = slots[id.index - 1U];
    slot.reserved = false;
    slot.occupied = false;
    slot.proxy = {};
    ++slot.generation;
    if (slot.generation == 0) {
        std::terminate();
    }
    free_slots.push_back(id.index - 1U);
}

} // namespace

core::Status validate_render_object_proxy(const RenderObjectProxy& object) {
    if (!object.id.is_valid() || !object.anchor.is_valid() || !object.mesh.is_valid() ||
        !object.material.is_valid() || !object.previous_transform.is_finite() ||
        !object.previous_transform.has_non_zero_scale() || !object.current_transform.is_finite() ||
        !object.current_transform.has_non_zero_scale() || !object.model_transform.is_finite() ||
        !object.local_bounds.is_valid() || !finite_color(object.color) ||
        !std::isfinite(object.minimum_view_distance) ||
        !std::isfinite(object.maximum_view_distance) || object.minimum_view_distance < 0.0F ||
        object.maximum_view_distance < 0.0F ||
        (object.maximum_view_distance != 0.0F &&
         object.maximum_view_distance <= object.minimum_view_distance) ||
        object.atlas_columns == 0U || object.atlas_rows == 0U ||
        object.sprite_frame >=
            static_cast<std::uint32_t>(object.atlas_columns) * object.atlas_rows ||
        !std::isfinite(object.wind_phase) || !std::isfinite(object.wind_stiffness) ||
        object.wind_stiffness < 0.0F || object.wind_stiffness > 1.0F ||
        !std::isfinite(object.foliage_transmission) || object.foliage_transmission < 0.0F ||
        object.foliage_transmission > 4.0F || !std::isfinite(object.water_wave_height) ||
        object.water_wave_height < 0.0F || object.water_wave_height > 16.0F ||
        !std::isfinite(object.water_wave_speed) || object.water_wave_speed < 0.0F ||
        object.water_wave_speed > 16.0F || !std::isfinite(object.water_optical_depth) ||
        object.water_optical_depth < 0.0F || object.water_optical_depth > 1'024.0F ||
        !std::isfinite(object.water_foam_strength) || object.water_foam_strength < 0.0F ||
        object.water_foam_strength > 4.0F || !object.water_world_phase.is_finite() ||
        !std::isfinite(object.particle_emissive_intensity) ||
        object.particle_emissive_intensity < 0.0F || object.particle_emissive_intensity > 64.0F ||
        !std::isfinite(object.particle_soft_fade_distance) ||
        object.particle_soft_fade_distance < 0.0F || object.particle_soft_fade_distance > 64.0F ||
        !std::isfinite(object.particle_velocity_stretch) ||
        object.particle_velocity_stretch < 0.0F || object.particle_velocity_stretch > 16.0F ||
        !std::isfinite(object.distance_fade_width) || object.distance_fade_width < 0.0F ||
        object.morph_weights.size() > 64U ||
        !std::ranges::all_of(object.morph_weights,
                             [](float value) { return std::isfinite(value); })) {
        return core::Status::failure("render_scene.invalid_object",
                                     "render object proxy contains invalid retained render data");
    }
    if (object.parent == object.id) {
        return core::Status::failure("render_scene.self_parent",
                                     "render object cannot parent itself");
    }
    return core::Status::ok();
}

core::Status validate_render_light_proxy(const RenderLightProxy& light) {
    if (!light.id.is_valid() || !light.anchor.is_valid() || !light.direction.is_finite() ||
        !light.color.is_finite() || !std::isfinite(light.intensity) ||
        !std::isfinite(light.radius) || light.intensity < 0.0F || light.radius <= 0.0F ||
        !std::isfinite(light.inner_cone_cosine) || !std::isfinite(light.outer_cone_cosine) ||
        !std::isfinite(light.gameplay_importance) || light.gameplay_importance < 0.0F ||
        light.inner_cone_cosine < light.outer_cone_cosine || light.inner_cone_cosine > 1.0F ||
        light.outer_cone_cosine < -1.0F || light.color.x < 0.0F || light.color.y < 0.0F ||
        light.color.z < 0.0F ||
        ((light.kind == RenderLightKind::directional || light.kind == RenderLightKind::spot) &&
         math::length_squared(light.direction) <= 0.0F)) {
        return core::Status::failure("render_scene.invalid_light",
                                     "render light proxy contains invalid retained render data");
    }
    return core::Status::ok();
}

core::Status validate_render_skin_palette_proxy(const RenderSkinPaletteProxy& palette) {
    if (!palette.id.is_valid() || palette.joint_matrices.empty() ||
        palette.joint_matrices.size() > maximum_render_skin_joints ||
        !std::ranges::all_of(palette.joint_matrices,
                             [](const math::Mat4f& matrix) { return matrix.is_finite(); })) {
        return core::Status::failure(
            "render_scene.invalid_skin_palette",
            "render skin palette must contain one to 256 finite joint matrices");
    }
    return core::Status::ok();
}

math::Transform3f interpolate_render_transform(const RenderObjectProxy& object,
                                               float simulation_alpha) noexcept {
    const auto alpha = std::clamp(simulation_alpha, 0.0F, 1.0F);
    if (any(object.flags & RenderObjectFlags::teleport)) {
        return object.current_transform;
    }
    math::Transform3f result;
    result.position = {
        lerp(object.previous_transform.position.x, object.current_transform.position.x, alpha),
        lerp(object.previous_transform.position.y, object.current_transform.position.y, alpha),
        lerp(object.previous_transform.position.z, object.current_transform.position.z, alpha),
    };
    result.rotation_degrees = {
        interpolate_degrees(object.previous_transform.rotation_degrees.x,
                            object.current_transform.rotation_degrees.x, alpha),
        interpolate_degrees(object.previous_transform.rotation_degrees.y,
                            object.current_transform.rotation_degrees.y, alpha),
        interpolate_degrees(object.previous_transform.rotation_degrees.z,
                            object.current_transform.rotation_degrees.z, alpha),
    };
    result.scale = {
        lerp(object.previous_transform.scale.x, object.current_transform.scale.x, alpha),
        lerp(object.previous_transform.scale.y, object.current_transform.scale.y, alpha),
        lerp(object.previous_transform.scale.z, object.current_transform.scale.z, alpha),
    };
    return result;
}

RenderObjectId RenderScene::reserve_object_id() {
    return reserve_id<ObjectSlot, RenderObjectId>(objects_, free_objects_);
}

RenderLightId RenderScene::reserve_light_id() {
    return reserve_id<LightSlot, RenderLightId>(lights_, free_lights_);
}

RenderSkinPaletteId RenderScene::reserve_skin_palette_id() {
    return reserve_id<SkinPaletteSlot, RenderSkinPaletteId>(skin_palettes_, free_skin_palettes_);
}

core::Result<RenderObjectId> RenderScene::create_object(RenderObjectProxy object) {
    object.id = reserve_object_id();
    if (!object.id.is_valid()) {
        return core::Result<RenderObjectId>::failure("render_scene.object_capacity_exhausted",
                                                     "render object id capacity is exhausted");
    }
    const auto id = object.id;
    auto status = upsert_object(object);
    if (!status) {
        release_slot(objects_, free_objects_, id);
        return core::Result<RenderObjectId>::failure(status.error().code, status.error().message);
    }
    return core::Result<RenderObjectId>::success(id);
}

core::Result<RenderLightId> RenderScene::create_light(RenderLightProxy light) {
    light.id = reserve_light_id();
    if (!light.id.is_valid()) {
        return core::Result<RenderLightId>::failure("render_scene.light_capacity_exhausted",
                                                    "render light id capacity is exhausted");
    }
    const auto id = light.id;
    auto status = upsert_light(light);
    if (!status) {
        release_slot(lights_, free_lights_, id);
        return core::Result<RenderLightId>::failure(status.error().code, status.error().message);
    }
    return core::Result<RenderLightId>::success(id);
}

core::Result<RenderSkinPaletteId> RenderScene::create_skin_palette(RenderSkinPaletteProxy palette) {
    palette.id = reserve_skin_palette_id();
    if (!palette.id.is_valid()) {
        return core::Result<RenderSkinPaletteId>::failure(
            "render_scene.skin_palette_capacity_exhausted",
            "render skin palette id capacity is exhausted");
    }
    const auto id = palette.id;
    auto status = upsert_skin_palette(palette);
    if (!status) {
        release_slot(skin_palettes_, free_skin_palettes_, id);
        return core::Result<RenderSkinPaletteId>::failure(status.error().code,
                                                          status.error().message);
    }
    return core::Result<RenderSkinPaletteId>::success(id);
}

core::Status RenderScene::upsert_object(const RenderObjectProxy& object) {
    auto status = validate_render_object_proxy(object);
    if (!status) {
        return status;
    }
    auto* slot = find_reserved_slot(objects_, object.id);
    if (slot == nullptr) {
        return core::Status::failure("render_scene.stale_object_id",
                                     "render object update uses an unknown or stale generation");
    }
    if (object.parent.is_valid() && find_object(object.parent) == nullptr) {
        return core::Status::failure("render_scene.missing_parent",
                                     "render object parent is not retained in this scene");
    }
    if (object.skin_palette.is_valid() && find_skin_palette(object.skin_palette) == nullptr) {
        return core::Status::failure("render_scene.missing_skin_palette",
                                     "render object references an unknown or stale skin palette");
    }
    auto parent = object.parent;
    for (std::size_t depth = 0; parent.is_valid(); ++depth) {
        if (depth >= objects_.size() || parent == object.id) {
            return core::Status::failure("render_scene.parent_cycle",
                                         "render object parent hierarchy contains a cycle");
        }
        const auto* parent_proxy = find_object(parent);
        if (parent_proxy == nullptr) {
            return core::Status::failure("render_scene.missing_parent",
                                         "render object parent hierarchy is stale");
        }
        parent = parent_proxy->parent;
    }
    const auto previous_parent = slot->occupied ? slot->proxy.parent : RenderObjectId{};
    if (previous_parent != object.parent) {
        if (previous_parent.is_valid()) {
            auto& previous_parent_slot = objects_[previous_parent.index - 1U];
            if (previous_parent_slot.child_count == 0) {
                return core::Status::failure("render_scene.invalid_child_count",
                                             "render object hierarchy child count is inconsistent");
            }
            --previous_parent_slot.child_count;
        }
        if (object.parent.is_valid()) {
            ++objects_[object.parent.index - 1U].child_count;
        }
    }
    slot->proxy = object;
    slot->occupied = true;
    return core::Status::ok();
}

core::Status RenderScene::remove_object(RenderObjectId id) {
    if (find_object(id) == nullptr) {
        return core::Status::failure("render_scene.stale_object_id",
                                     "render object removal uses an unknown or stale generation");
    }
    auto& slot = objects_[id.index - 1U];
    if (slot.child_count != 0) {
        return core::Status::failure("render_scene.object_has_children",
                                     "render object must have its children removed first");
    }
    if (slot.proxy.parent.is_valid()) {
        auto& parent_slot = objects_[slot.proxy.parent.index - 1U];
        if (parent_slot.child_count == 0) {
            return core::Status::failure("render_scene.invalid_child_count",
                                         "render object hierarchy child count is inconsistent");
        }
        --parent_slot.child_count;
    }
    slot.child_count = 0;
    static_cast<void>(visibility_hierarchy_.erase(visibility_key(id)));
    release_slot(objects_, free_objects_, id);
    return core::Status::ok();
}

core::Status RenderScene::upsert_light(const RenderLightProxy& light) {
    auto status = validate_render_light_proxy(light);
    if (!status) {
        return status;
    }
    auto* slot = find_reserved_slot(lights_, light.id);
    if (slot == nullptr) {
        return core::Status::failure("render_scene.stale_light_id",
                                     "render light update uses an unknown or stale generation");
    }
    slot->proxy = light;
    slot->occupied = true;
    return core::Status::ok();
}

core::Status RenderScene::remove_light(RenderLightId id) {
    if (find_light(id) == nullptr) {
        return core::Status::failure("render_scene.stale_light_id",
                                     "render light removal uses an unknown or stale generation");
    }
    release_slot(lights_, free_lights_, id);
    return core::Status::ok();
}

core::Status RenderScene::upsert_skin_palette(const RenderSkinPaletteProxy& palette) {
    auto status = validate_render_skin_palette_proxy(palette);
    if (!status) {
        return status;
    }
    auto* slot = find_reserved_slot(skin_palettes_, palette.id);
    if (slot == nullptr) {
        return core::Status::failure(
            "render_scene.stale_skin_palette_id",
            "render skin palette update uses an unknown or stale generation");
    }
    slot->proxy = palette;
    slot->occupied = true;
    return core::Status::ok();
}

core::Status RenderScene::remove_skin_palette(RenderSkinPaletteId id) {
    if (find_skin_palette(id) == nullptr) {
        return core::Status::failure(
            "render_scene.stale_skin_palette_id",
            "render skin palette removal uses an unknown or stale generation");
    }
    if (std::ranges::any_of(objects_, [id](const ObjectSlot& slot) {
            return slot.occupied && slot.proxy.skin_palette == id;
        })) {
        return core::Status::failure(
            "render_scene.skin_palette_in_use",
            "render skin palette must not be retained by an object when removed");
    }
    release_slot(skin_palettes_, free_skin_palettes_, id);
    return core::Status::ok();
}

core::Status RenderScene::apply(std::span<const RenderSceneUpdate> updates) {
    for (const auto& update : updates) {
        auto status = core::Status::ok();
        switch (update.kind) {
        case RenderSceneUpdateKind::upsert_object:
            status = upsert_object(update.object);
            break;
        case RenderSceneUpdateKind::remove_object:
            status = remove_object(update.object_id);
            break;
        case RenderSceneUpdateKind::upsert_light:
            status = upsert_light(update.light);
            break;
        case RenderSceneUpdateKind::remove_light:
            status = remove_light(update.light_id);
            break;
        case RenderSceneUpdateKind::upsert_skin_palette:
            status = upsert_skin_palette(update.skin_palette);
            break;
        case RenderSceneUpdateKind::remove_skin_palette:
            status = remove_skin_palette(update.skin_palette_id);
            break;
        }
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

core::Result<RenderSceneFrame>
RenderScene::extract(const RenderCamera& camera, float simulation_alpha,
                     std::span<const math::Mat4f> shadow_view_projections) const {
    if (!std::isfinite(simulation_alpha)) {
        return core::Result<RenderSceneFrame>::failure("render_scene.invalid_simulation_alpha",
                                                       "render interpolation alpha must be finite");
    }
    RenderSceneFrame frame;
    frame.stats = stats();
    if (shadow_view_projections.size() > 64U) {
        return core::Result<RenderSceneFrame>::failure(
            "render_scene.too_many_shadow_views",
            "render scene extraction supports at most 64 simultaneous shadow views");
    }
    std::vector<RenderFrustum> shadow_frusta;
    shadow_frusta.reserve(shadow_view_projections.size());
    for (const auto& projection : shadow_view_projections) {
        shadow_frusta.push_back(RenderFrustum::from_view_projection(projection));
    }
    std::vector<math::Mat4f> transforms(objects_.size());
    std::vector<math::Vec3f> object_origins(objects_.size());
    std::vector<bool> resolved(objects_.size(), false);

    const auto resolve_transform = [&](const auto& self, const RenderObjectProxy& object,
                                       std::size_t depth) -> core::Result<math::Mat4f> {
        if (depth > objects_.size()) {
            return core::Result<math::Mat4f>::failure(
                "render_scene.parent_cycle", "render object parent hierarchy contains a cycle");
        }
        const auto index = static_cast<std::size_t>(object.id.index - 1U);
        if (resolved[index]) {
            return core::Result<math::Mat4f>::success(transforms[index]);
        }
        const auto local_transform =
            math::transform_matrix(interpolate_render_transform(object, simulation_alpha));
        const auto local = local_transform * object.model_transform;
        math::Mat4f model;
        math::Mat4f object_basis;
        if (object.parent.is_valid()) {
            const auto* parent = find_object(object.parent);
            if (parent == nullptr) {
                return core::Result<math::Mat4f>::failure("render_scene.missing_parent",
                                                          "render object parent became stale");
            }
            auto parent_transform = self(self, *parent, depth + 1U);
            if (!parent_transform) {
                return parent_transform;
            }
            object_basis = parent_transform.value() * local_transform;
            model = parent_transform.value() * local;
        } else {
            auto relative = world::to_camera_relative(object.anchor, camera.floating_origin);
            if (!relative) {
                return core::Result<math::Mat4f>::failure(relative.error().code,
                                                          relative.error().message);
            }
            object_basis = math::translation_matrix(relative.value()) * local_transform;
            model = object_basis * object.model_transform;
        }
        const auto origin = object_basis * math::Vec4f{0.0F, 0.0F, 0.0F, 1.0F};
        object_origins[index] = {origin.x, origin.y, origin.z};
        transforms[index] = model;
        resolved[index] = true;
        return core::Result<math::Mat4f>::success(model);
    };

    // Transform resolution is required for animation and parenting even when an object is later
    // culled. Feed those resolved bounds into the retained hierarchy once, then query all views in
    // one traversal instead of scanning every object independently for every shadow cascade.
    for (const auto& slot : objects_) {
        if (!slot.occupied) {
            continue;
        }
        const auto& object = slot.proxy;
        const auto key = visibility_key(object.id);
        if (any(object.flags & RenderObjectFlags::hidden)) {
            static_cast<void>(visibility_hierarchy_.erase(key));
            continue;
        }
        auto transform = resolve_transform(resolve_transform, object, 0);
        if (!transform) {
            return core::Result<RenderSceneFrame>::failure(transform.error().code,
                                                           transform.error().message);
        }
        const auto bounds = math::transform_bounds(transform.value(), object.local_bounds);
        visibility_hierarchy_.upsert(
            {key,
             {{static_cast<double>(bounds.min.x), static_cast<double>(bounds.min.y),
               static_cast<double>(bounds.min.z)},
              {static_cast<double>(bounds.max.x), static_cast<double>(bounds.max.y),
               static_cast<double>(bounds.max.z)}},
             {},
             static_cast<double>(object.maximum_view_distance),
             1.0F,
             any(object.flags & RenderObjectFlags::cast_shadow) &&
                 (object.layer == RenderLayer::opaque ||
                  object.layer == RenderLayer::alpha_tested),
             false,
             {static_cast<double>(object_origins[object.id.index - 1U].x),
              static_cast<double>(object_origins[object.id.index - 1U].y),
              static_cast<double>(object_origins[object.id.index - 1U].z)},
             object.use_object_origin_for_view_distance});
    }

    std::vector<VisibilityView> visibility_views;
    visibility_views.reserve(shadow_frusta.size() + 1U);
    visibility_views.push_back({1U,
                                VisibilityViewKind::main,
                                {static_cast<double>(camera.local_position.x),
                                 static_cast<double>(camera.local_position.y),
                                 static_cast<double>(camera.local_position.z)},
                                RenderFrustum::from_view_projection(
                                    camera.camera_relative_view_projection()),
                                1U,
                                camera.vertical_fov_radians,
                                static_cast<double>(camera.far_plane)});
    for (std::size_t index = 0; index < shadow_frusta.size(); ++index) {
        visibility_views.push_back(
            {static_cast<VisibilityViewId>(index + 2U),
             VisibilityViewKind::directional_shadow, {}, shadow_frusta[index], 1U,
             camera.vertical_fov_radians, 0.0});
    }
    const auto visibility_result = visibility_hierarchy_.query(visibility_views);
    std::unordered_map<VisibilityKey, std::uint64_t> visibility_masks;
    visibility_masks.reserve(visibility_result.selections.size());
    for (const auto& selected : visibility_result.selections) {
        auto& mask = visibility_masks[selected.key];
        if (selected.view_id == 1U) {
            mask |= std::uint64_t{1} << 63U;
        } else {
            mask |= std::uint64_t{1} << (selected.view_id - 2U);
        }
    }
    frame.stats.visibility_hierarchy_nodes =
        static_cast<std::uint32_t>(visibility_hierarchy_.node_count());
    frame.stats.visibility_nodes_tested =
        static_cast<std::uint32_t>(visibility_result.stats.hierarchy_nodes_tested);
    frame.stats.visibility_nodes_culled =
        static_cast<std::uint32_t>(visibility_result.stats.hierarchy_nodes_culled);

    for (const auto& slot : objects_) {
        if (!slot.occupied) {
            continue;
        }
        const auto& object = slot.proxy;
        if (any(object.flags & RenderObjectFlags::hidden)) {
            ++frame.stats.hidden_objects;
            continue;
        }
        auto transform = resolve_transform(resolve_transform, object, 0);
        if (!transform) {
            return core::Result<RenderSceneFrame>::failure(transform.error().code,
                                                           transform.error().message);
        }
        const auto bounds = math::transform_bounds(transform.value(), object.local_bounds);
        const auto center = (bounds.min + bounds.max) * 0.5F;
        const auto distance_reference = object.use_object_origin_for_view_distance
                                            ? object_origins[object.id.index - 1U]
                                            : center;
        const auto camera_delta = distance_reference - camera.local_position;
        const auto camera_distance = std::sqrt(math::length_squared(camera_delta));
        if (camera_distance < object.minimum_view_distance ||
            (object.maximum_view_distance != 0.0F &&
             camera_distance >= object.maximum_view_distance)) {
            ++frame.stats.culled_objects;
            continue;
        }
        const auto selected = visibility_masks.find(visibility_key(object.id));
        const auto selected_mask =
            selected == visibility_masks.end() ? std::uint64_t{0} : selected->second;
        const auto camera_visible = (selected_mask & (std::uint64_t{1} << 63U)) != 0;
        const auto shadow_visibility_mask =
            selected_mask & ~(std::uint64_t{1} << 63U);
        if (!camera_visible) {
            ++frame.stats.culled_objects;
        }
        if (!camera_visible && shadow_visibility_mask == 0) {
            continue;
        }
        float visibility = 1.0F;
        if (object.distance_fade_width > 0.0F) {
            const auto smoothstep = [](float minimum, float maximum, float value) {
                const auto normalized = std::clamp(
                    (value - minimum) / std::max(maximum - minimum, 0.0001F), 0.0F, 1.0F);
                return normalized * normalized * (3.0F - 2.0F * normalized);
            };
            if (object.minimum_view_distance > 0.0F) {
                visibility *= smoothstep(object.minimum_view_distance,
                                         object.minimum_view_distance + object.distance_fade_width,
                                         camera_distance);
            }
            if (object.maximum_view_distance > 0.0F) {
                visibility *=
                    1.0F - smoothstep(object.maximum_view_distance - object.distance_fade_width,
                                      object.maximum_view_distance, camera_distance);
            }
        }
        RenderObjectInstance instance;
        instance.id = object.id;
        instance.mesh = object.mesh;
        instance.material = object.material;
        instance.skin_palette = object.skin_palette;
        instance.layer = object.layer;
        instance.camera_relative_transform = transform.value();
        instance.camera_relative_bounds = bounds;
        instance.color = object.color;
        instance.morph_weights = object.morph_weights;
        instance.sprite_frame = object.sprite_frame;
        instance.atlas_columns = object.atlas_columns;
        instance.atlas_rows = object.atlas_rows;
        instance.effect_flags = object.effect_flags;
        instance.wind_phase = object.wind_phase;
        instance.wind_stiffness = object.wind_stiffness;
        instance.foliage_transmission = object.foliage_transmission;
        instance.water_world_phase = object.water_world_phase;
        instance.effect_parameters2 =
            any(object.effect_flags & RenderEffectFlags::particle)
                ? std::array<float, 4>{object.particle_emissive_intensity,
                                       object.particle_soft_fade_distance,
                                       object.particle_velocity_stretch, 0.0F}
                : std::array<float, 4>{object.water_wave_height, object.water_wave_speed,
                                       object.water_optical_depth, object.water_foam_strength};
        instance.visibility = visibility;
        instance.camera_visible = camera_visible;
        instance.shadow_visibility_mask = shadow_visibility_mask;
        instance.reset_motion_history = any(object.flags & RenderObjectFlags::teleport);
        auto batch = std::ranges::find_if(
            frame.batches,
            [&object, camera_visible, shadow_visibility_mask](const RenderInstanceBatch& value) {
                return value.mesh == object.mesh && value.material == object.material &&
                       value.layer == object.layer &&
                       value.two_sided == any(object.flags & RenderObjectFlags::two_sided) &&
                       value.casts_shadow == any(object.flags & RenderObjectFlags::cast_shadow) &&
                       value.camera_visible == camera_visible &&
                       value.shadow_visibility_mask == shadow_visibility_mask;
            });
        if (batch == frame.batches.end()) {
            frame.batches.push_back({object.mesh,
                                     object.material,
                                     object.layer,
                                     any(object.flags & RenderObjectFlags::two_sided),
                                     any(object.flags & RenderObjectFlags::cast_shadow),
                                     camera_visible,
                                     shadow_visibility_mask,
                                     {}});
            batch = std::prev(frame.batches.end());
        }
        batch->instances.push_back(std::move(instance));
        frame.stats.visible_objects += camera_visible ? 1U : 0U;
    }
    std::ranges::sort(frame.batches,
                      [](const RenderInstanceBatch& left, const RenderInstanceBatch& right) {
                          if (left.layer != right.layer) {
                              return left.layer < right.layer;
                          }
                          if (left.material != right.material) {
                              return left.material < right.material;
                          }
                          if (left.two_sided != right.two_sided) {
                              return left.two_sided < right.two_sided;
                          }
                          if (left.casts_shadow != right.casts_shadow) {
                              return left.casts_shadow > right.casts_shadow;
                          }
                          return left.mesh < right.mesh;
                      });
    frame.stats.instance_batches = static_cast<std::uint32_t>(frame.batches.size());

    frame.lights = extract_lights(camera);
    return core::Result<RenderSceneFrame>::success(std::move(frame));
}

std::vector<RenderLightInstance> RenderScene::extract_lights(const RenderCamera& camera) const {
    std::vector<RenderLightInstance> result;
    result.reserve(stats().retained_lights);
    for (const auto& slot : lights_) {
        if (!slot.occupied) {
            continue;
        }
        auto position = world::to_camera_relative(slot.proxy.anchor, camera.floating_origin);
        if (!position) {
            continue;
        }
        result.push_back({slot.proxy.id, slot.proxy.kind, position.value(), slot.proxy.direction,
                          slot.proxy.color, slot.proxy.intensity, slot.proxy.radius,
                          slot.proxy.inner_cone_cosine, slot.proxy.outer_cone_cosine,
                          slot.proxy.gameplay_importance, slot.proxy.casts_shadow,
                          slot.proxy.light_revision, slot.proxy.nearby_geometry_revision});
    }
    return result;
}

void RenderScene::clear() noexcept {
    visibility_hierarchy_.clear();
    free_objects_.clear();
    free_lights_.clear();
    free_skin_palettes_.clear();
    free_objects_.reserve(objects_.size());
    for (std::size_t index = 0; index < objects_.size(); ++index) {
        auto& slot = objects_[index];
        if (slot.reserved || slot.occupied) {
            ++slot.generation;
            if (slot.generation == 0) {
                std::terminate();
            }
        }
        slot.reserved = false;
        slot.occupied = false;
        slot.child_count = 0;
        slot.proxy = {};
        free_objects_.push_back(static_cast<std::uint32_t>(objects_.size() - index - 1U));
    }
    free_lights_.reserve(lights_.size());
    for (std::size_t index = 0; index < lights_.size(); ++index) {
        auto& slot = lights_[index];
        if (slot.reserved || slot.occupied) {
            ++slot.generation;
            if (slot.generation == 0) {
                std::terminate();
            }
        }
        slot.reserved = false;
        slot.occupied = false;
        slot.proxy = {};
        free_lights_.push_back(static_cast<std::uint32_t>(lights_.size() - index - 1U));
    }
    free_skin_palettes_.reserve(skin_palettes_.size());
    for (std::size_t index = 0; index < skin_palettes_.size(); ++index) {
        auto& slot = skin_palettes_[index];
        if (slot.reserved || slot.occupied) {
            ++slot.generation;
            if (slot.generation == 0) {
                std::terminate();
            }
        }
        slot.reserved = false;
        slot.occupied = false;
        slot.proxy = {};
        free_skin_palettes_.push_back(
            static_cast<std::uint32_t>(skin_palettes_.size() - index - 1U));
    }
}

const RenderObjectProxy* RenderScene::find_object(RenderObjectId id) const noexcept {
    const auto* slot = find_slot(objects_, id);
    return slot == nullptr ? nullptr : &slot->proxy;
}

const RenderLightProxy* RenderScene::find_light(RenderLightId id) const noexcept {
    const auto* slot = find_slot(lights_, id);
    return slot == nullptr ? nullptr : &slot->proxy;
}

const RenderSkinPaletteProxy*
RenderScene::find_skin_palette(RenderSkinPaletteId id) const noexcept {
    const auto* slot = find_slot(skin_palettes_, id);
    return slot == nullptr ? nullptr : &slot->proxy;
}

RenderSceneStats RenderScene::stats() const noexcept {
    RenderSceneStats result;
    result.retained_objects = static_cast<std::uint32_t>(
        std::ranges::count_if(objects_, [](const ObjectSlot& slot) { return slot.occupied; }));
    result.retained_lights = static_cast<std::uint32_t>(
        std::ranges::count_if(lights_, [](const LightSlot& slot) { return slot.occupied; }));
    result.retained_skin_palettes = static_cast<std::uint32_t>(std::ranges::count_if(
        skin_palettes_, [](const SkinPaletteSlot& slot) { return slot.occupied; }));
    return result;
}

} // namespace heartstead::renderer
