#include "engine/renderer/scene/render_scene.hpp"

#include "engine/renderer/camera/frustum.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iterator>
#include <limits>
#include <ranges>
#include <utility>

namespace heartstead::renderer {

namespace {

[[nodiscard]] bool finite_color(const std::array<float, 4>& color) noexcept {
    return std::ranges::all_of(
        color, [](float value) { return std::isfinite(value) && value >= 0.0F && value <= 1.0F; });
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
        !object.current_transform.has_non_zero_scale() || !object.local_bounds.is_valid() ||
        !finite_color(object.color)) {
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
        light.color.x < 0.0F || light.color.y < 0.0F || light.color.z < 0.0F ||
        (light.kind == RenderLightKind::directional &&
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
    slot->proxy = object;
    slot->occupied = true;
    return core::Status::ok();
}

core::Status RenderScene::remove_object(RenderObjectId id) {
    if (find_object(id) == nullptr) {
        return core::Status::failure("render_scene.stale_object_id",
                                     "render object removal uses an unknown or stale generation");
    }
    if (std::ranges::any_of(objects_, [id](const ObjectSlot& slot) {
            return slot.occupied && slot.proxy.parent == id;
        })) {
        return core::Status::failure("render_scene.object_has_children",
                                     "render object must have its children removed first");
    }
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

core::Result<RenderSceneFrame> RenderScene::extract(const RenderCamera& camera,
                                                    float simulation_alpha) const {
    if (!std::isfinite(simulation_alpha)) {
        return core::Result<RenderSceneFrame>::failure("render_scene.invalid_simulation_alpha",
                                                       "render interpolation alpha must be finite");
    }
    RenderSceneFrame frame;
    frame.stats = stats();
    const auto frustum = RenderFrustum::from_view_projection(camera.view_projection);
    std::vector<math::Mat4f> transforms(objects_.size());
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
        const auto local =
            math::transform_matrix(interpolate_render_transform(object, simulation_alpha));
        math::Mat4f model;
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
            model = parent_transform.value() * local;
        } else {
            auto relative = world::to_camera_relative(object.anchor, camera.floating_origin);
            if (!relative) {
                return core::Result<math::Mat4f>::failure(relative.error().code,
                                                          relative.error().message);
            }
            model = math::translation_matrix(relative.value()) * local;
        }
        transforms[index] = model;
        resolved[index] = true;
        return core::Result<math::Mat4f>::success(model);
    };

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
        if (!frustum.intersects(bounds)) {
            ++frame.stats.culled_objects;
            continue;
        }
        RenderObjectInstance instance{
            object.id,    object.mesh,       object.material, object.skin_palette,
            object.layer, transform.value(), bounds,          object.color};
        auto batch =
            std::ranges::find_if(frame.batches, [&object](const RenderInstanceBatch& value) {
                return value.mesh == object.mesh && value.material == object.material &&
                       value.layer == object.layer;
            });
        if (batch == frame.batches.end()) {
            frame.batches.push_back({object.mesh, object.material, object.layer, {}});
            batch = std::prev(frame.batches.end());
        }
        batch->instances.push_back(std::move(instance));
        ++frame.stats.visible_objects;
    }
    std::ranges::sort(frame.batches,
                      [](const RenderInstanceBatch& left, const RenderInstanceBatch& right) {
                          if (left.layer != right.layer) {
                              return left.layer < right.layer;
                          }
                          if (left.material != right.material) {
                              return left.material < right.material;
                          }
                          return left.mesh < right.mesh;
                      });
    frame.stats.instance_batches = static_cast<std::uint32_t>(frame.batches.size());

    frame.lights.reserve(frame.stats.retained_lights);
    for (const auto& slot : lights_) {
        if (!slot.occupied) {
            continue;
        }
        auto position = world::to_camera_relative(slot.proxy.anchor, camera.floating_origin);
        if (!position) {
            continue;
        }
        frame.lights.push_back({slot.proxy.id, slot.proxy.kind, position.value(),
                                slot.proxy.direction, slot.proxy.color, slot.proxy.intensity,
                                slot.proxy.radius});
    }
    return core::Result<RenderSceneFrame>::success(std::move(frame));
}

void RenderScene::clear() noexcept {
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
