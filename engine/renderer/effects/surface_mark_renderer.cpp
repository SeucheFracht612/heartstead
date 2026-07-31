#include "engine/renderer/effects/surface_mark_renderer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>

namespace heartstead::renderer {

namespace {

[[nodiscard]] const std::string* field(const modding::GenericPrototype& prototype,
                                       std::string_view key) {
    const auto found = prototype.fields.find(std::string(key));
    return found == prototype.fields.end() ? nullptr : &found->second;
}

template <typename Value>
[[nodiscard]] core::Result<Value> number_field(const modding::GenericPrototype& prototype,
                                               std::string_view key, Value fallback) {
    const auto* text = field(prototype, key);
    if (text == nullptr) {
        return core::Result<Value>::success(fallback);
    }
    Value value{};
    const auto [end, error] =
        std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc{} || end != text->data() + text->size() ||
        (std::is_floating_point_v<Value> && !std::isfinite(value))) {
        return core::Result<Value>::failure("surface_mark.invalid_number",
                                            std::string(key) + " must be a finite number");
    }
    return core::Result<Value>::success(value);
}

[[nodiscard]] core::Result<bool> bool_field(const modding::GenericPrototype& prototype,
                                            std::string_view key, bool fallback) {
    const auto* text = field(prototype, key);
    if (text == nullptr) {
        return core::Result<bool>::success(fallback);
    }
    if (*text == "true") {
        return core::Result<bool>::success(true);
    }
    if (*text == "false") {
        return core::Result<bool>::success(false);
    }
    return core::Result<bool>::failure("surface_mark.invalid_bool",
                                       std::string(key) + " must be true or false");
}

[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] float random_unit(std::uint64_t value) noexcept {
    return static_cast<float>(mix(value) >> 40U) /
           static_cast<float>(0x00ff'ffffU);
}

[[nodiscard]] RenderLayer layer_for(SurfaceMarkBlendMode mode) noexcept {
    switch (mode) {
    case SurfaceMarkBlendMode::alpha:
        return RenderLayer::transparent;
    case SurfaceMarkBlendMode::premultiplied_alpha:
        return RenderLayer::premultiplied;
    case SurfaceMarkBlendMode::additive:
        return RenderLayer::additive;
    }
    return RenderLayer::premultiplied;
}

[[nodiscard]] core::Result<SurfaceMarkBlendMode>
blend_field(const modding::GenericPrototype& prototype) {
    const auto* text = field(prototype, "blend_mode");
    if (text == nullptr || *text == "premultiplied_alpha") {
        return core::Result<SurfaceMarkBlendMode>::success(
            SurfaceMarkBlendMode::premultiplied_alpha);
    }
    if (*text == "alpha") {
        return core::Result<SurfaceMarkBlendMode>::success(SurfaceMarkBlendMode::alpha);
    }
    if (*text == "additive") {
        return core::Result<SurfaceMarkBlendMode>::success(SurfaceMarkBlendMode::additive);
    }
    return core::Result<SurfaceMarkBlendMode>::failure(
        "surface_mark.invalid_blend",
        "surface mark blend_mode must be alpha, premultiplied_alpha, or additive");
}

} // namespace

core::Status SurfaceMarkPrototype::validate() const noexcept {
    if (!id.is_valid() || display_name.empty() || material_group > 3U ||
        !std::isfinite(size_min) || !std::isfinite(size_max) || size_min <= 0.0F ||
        size_max < size_min || size_max > 64.0F || !std::isfinite(lifetime_seconds) ||
        lifetime_seconds < 0.0F || !std::isfinite(fade_seconds) || fade_seconds < 0.0F ||
        (lifetime_seconds > 0.0F && fade_seconds > lifetime_seconds) ||
        !std::isfinite(surface_offset) || surface_offset < 0.0F ||
        surface_offset > 0.25F || !std::isfinite(maximum_distance) ||
        maximum_distance <= 0.0F || atlas_columns == 0U || atlas_rows == 0U ||
        atlas_frame_count == 0U ||
        static_cast<std::uint32_t>(atlas_frame_count) >
            static_cast<std::uint32_t>(atlas_columns) * atlas_rows) {
        return core::Status::failure(
            "surface_mark.invalid_prototype",
            "surface mark prototype contains invalid material, size, lifetime, projection, or "
            "atlas data");
    }
    return core::Status::ok();
}

core::Status SurfaceMarkPrototypeRegistry::add(SurfaceMarkPrototype prototype) {
    auto status = prototype.validate();
    if (!status) {
        return status;
    }
    if (find(prototype.id) != nullptr) {
        return core::Status::failure("surface_mark.duplicate_id",
                                     "surface mark prototype id is already registered");
    }
    prototypes_.push_back(std::move(prototype));
    std::ranges::sort(prototypes_, {}, [](const auto& value) { return value.id.value(); });
    return core::Status::ok();
}

const SurfaceMarkPrototype*
SurfaceMarkPrototypeRegistry::find(const core::PrototypeId& id) const noexcept {
    const auto found = std::ranges::lower_bound(
        prototypes_, id.value(), {}, [](const auto& value) { return value.id.value(); });
    return found == prototypes_.end() || found->id != id ? nullptr : &*found;
}

std::span<const SurfaceMarkPrototype>
SurfaceMarkPrototypeRegistry::prototypes() const noexcept {
    return prototypes_;
}

std::size_t SurfaceMarkPrototypeRegistry::size() const noexcept {
    return prototypes_.size();
}

core::Result<SurfaceMarkPrototype>
surface_mark_prototype_from_generic(const modding::GenericPrototype& prototype) {
    if (prototype.kind != modding::PrototypeKinds::decal) {
        return core::Result<SurfaceMarkPrototype>::failure(
            "surface_mark.invalid_kind", "prototype kind must be decal");
    }
    auto material = number_field<std::uint16_t>(prototype, "material_group", 0);
    auto blend = blend_field(prototype);
    auto size_min = number_field<float>(prototype, "size_min", 0.2F);
    auto size_max = number_field<float>(prototype, "size_max", 0.5F);
    auto lifetime = number_field<float>(prototype, "lifetime_seconds", 0.0F);
    auto fade = number_field<float>(prototype, "fade_seconds", 0.0F);
    auto offset = number_field<float>(prototype, "surface_offset", 0.006F);
    auto distance = number_field<float>(prototype, "maximum_distance", 96.0F);
    auto columns = number_field<std::uint16_t>(prototype, "atlas_columns", 1);
    auto rows = number_field<std::uint16_t>(prototype, "atlas_rows", 1);
    auto frames = number_field<std::uint16_t>(prototype, "atlas_frame_count", 1);
    auto lighting = bool_field(prototype, "receives_lighting", true);
    if (!material || !blend || !size_min || !size_max || !lifetime || !fade || !offset ||
        !distance || !columns || !rows || !frames || !lighting ||
        material.value() > std::numeric_limits<std::uint8_t>::max()) {
        return core::Result<SurfaceMarkPrototype>::failure(
            "surface_mark.invalid_fields", "surface mark prototype fields are invalid");
    }
    SurfaceMarkPrototype result;
    result.id = prototype.id;
    result.display_name = field(prototype, "display_name") == nullptr
                              ? prototype.id.value()
                              : *field(prototype, "display_name");
    result.material_group = static_cast<std::uint8_t>(material.value());
    result.blend_mode = blend.value();
    result.size_min = size_min.value();
    result.size_max = size_max.value();
    result.lifetime_seconds = lifetime.value();
    result.fade_seconds = fade.value();
    result.surface_offset = offset.value();
    result.maximum_distance = distance.value();
    result.atlas_columns = columns.value();
    result.atlas_rows = rows.value();
    result.atlas_frame_count = frames.value();
    result.receives_lighting = lighting.value();
    auto status = result.validate();
    if (!status) {
        return core::Result<SurfaceMarkPrototype>::failure(status.error().code,
                                                           status.error().message);
    }
    return core::Result<SurfaceMarkPrototype>::success(std::move(result));
}

core::Result<SurfaceMarkPrototypeRegistry>
surface_mark_registry_from_prototypes(const modding::PrototypeRegistry& prototypes) {
    SurfaceMarkPrototypeRegistry result;
    for (const auto* prototype :
         prototypes.prototypes_of_kind(modding::PrototypeKinds::decal)) {
        auto parsed = surface_mark_prototype_from_generic(*prototype);
        if (!parsed) {
            return core::Result<SurfaceMarkPrototypeRegistry>::failure(
                parsed.error().code,
                prototype->source.generic_string() + ": " + parsed.error().message);
        }
        auto status = result.add(std::move(parsed).value());
        if (!status) {
            return core::Result<SurfaceMarkPrototypeRegistry>::failure(
                status.error().code, status.error().message);
        }
    }
    return core::Result<SurfaceMarkPrototypeRegistry>::success(std::move(result));
}

core::Status SurfaceMarkRendererConfig::validate() const noexcept {
    if (maximum_marks == 0U || maximum_marks > 1'000'000U ||
        maximum_spawns_per_update == 0U ||
        maximum_spawns_per_update > maximum_marks ||
        std::ranges::any_of(material_groups, [](auto material) {
            return !material.is_valid();
        })) {
        return core::Status::failure(
            "surface_mark.invalid_config",
            "surface mark renderer requires materials and bounded pool/spawn capacities");
    }
    return core::Status::ok();
}

core::Status
SurfaceMarkRenderer::initialize(Renderer& renderer,
                                const SurfaceMarkPrototypeRegistry& prototypes,
                                SurfaceMarkRendererConfig config) {
    if (is_initialized()) {
        return core::Status::failure("surface_mark.already_initialized",
                                     "surface mark renderer is already initialized");
    }
    for (auto& material : config.material_groups) {
        if (!material.is_valid()) {
            material = renderer.fallback_material();
        }
    }
    auto status = config.validate();
    if (!status || prototypes.size() == 0U) {
        return !status ? status
                       : core::Status::failure("surface_mark.missing_prototypes",
                                               "surface mark prototypes are required");
    }
    constexpr std::array<GpuStaticMeshVertex, 4> vertices{{
        {{-0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F}},
        {{0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 1.0F}},
        {{0.5F, 0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F}},
        {{-0.5F, 0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F}},
    }};
    constexpr std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    auto mesh = renderer.create_static_mesh(
        {"builtin:surface_mark_quad", vertices, indices,
         {{-0.5F, -0.5F, -0.01F}, {0.5F, 0.5F, 0.01F}}});
    if (!mesh) {
        return core::Status::failure(mesh.error().code, mesh.error().message);
    }
    renderer_ = &renderer;
    prototypes_ = &prototypes;
    config_ = config;
    quad_mesh_ = mesh.value();
    marks_.reserve(config.maximum_marks);
    return core::Status::ok();
}

core::Status SurfaceMarkRenderer::spawn(const SurfaceMarkSpawn& mark) {
    if (!is_initialized()) {
        return core::Status::failure("surface_mark.not_initialized",
                                     "surface mark renderer must be initialized first");
    }
    const auto* prototype = prototypes_->find(mark.prototype);
    if (prototype == nullptr || !mark.position.is_valid() || !mark.normal.is_finite() ||
        math::length_squared(mark.normal) <= 0.00001F ||
        !std::ranges::all_of(mark.color, [](float component) {
            return std::isfinite(component) && component >= 0.0F && component <= 4.0F;
        }) ||
        !std::isfinite(mark.size_multiplier) || mark.size_multiplier <= 0.0F ||
        !std::isfinite(mark.rotation_degrees) || mark.seed == 0U) {
        return core::Status::failure("surface_mark.invalid_spawn",
                                     "surface mark spawn data is invalid");
    }
    if (marks_.size() >= config_.maximum_marks ||
        spawned_in_window_ >= config_.maximum_spawns_per_update) {
        ++stats_.dropped_marks;
        return core::Status::failure("surface_mark.capacity",
                                     "surface mark pool or update spawn budget is exhausted");
    }
    const auto normal =
        mark.normal / std::sqrt(math::length_squared(mark.normal));
    auto position = world::WorldPosition::from_anchor(
        mark.position.anchor,
        mark.position.local_offset +
            math::Vec3d{static_cast<double>(normal.x * prototype->surface_offset),
                        static_cast<double>(normal.y * prototype->surface_offset),
                        static_cast<double>(normal.z * prototype->surface_offset)});
    if (!position) {
        return core::Status::failure(position.error().code, position.error().message);
    }
    const auto size =
        (prototype->size_min +
         (prototype->size_max - prototype->size_min) * random_unit(mark.seed)) *
        mark.size_multiplier;
    constexpr float radians_to_degrees =
        180.0F / std::numbers::pi_v<float>;
    RenderObjectProxy object;
    object.anchor = position.value();
    object.current_transform.rotation_degrees = {
        -std::asin(std::clamp(normal.y, -1.0F, 1.0F)) * radians_to_degrees,
        std::atan2(normal.x, normal.z) * radians_to_degrees,
        mark.rotation_degrees,
    };
    object.current_transform.scale = {size, size, size};
    object.previous_transform = object.current_transform;
    object.mesh = quad_mesh_;
    object.material = config_.material_groups[prototype->material_group];
    object.local_bounds = {{-0.5F, -0.5F, -0.01F}, {0.5F, 0.5F, 0.01F}};
    object.layer = layer_for(prototype->blend_mode);
    object.flags = RenderObjectFlags::two_sided;
    object.color = mark.color;
    object.atlas_columns = prototype->atlas_columns;
    object.atlas_rows = prototype->atlas_rows;
    object.sprite_frame =
        static_cast<std::uint32_t>(mix(mark.seed) % prototype->atlas_frame_count);
    object.maximum_view_distance = prototype->maximum_distance;
    object.effect_flags = RenderEffectFlags::particle;
    if (!prototype->receives_lighting) {
        object.effect_flags =
            object.effect_flags | RenderEffectFlags::unlit_particle;
    }
    if (prototype->blend_mode == SurfaceMarkBlendMode::premultiplied_alpha) {
        object.effect_flags =
            object.effect_flags | RenderEffectFlags::premultiplied_particle;
    }
    auto created = renderer_->create_object(object);
    if (!created) {
        return core::Status::failure(created.error().code, created.error().message);
    }
    object.id = created.value();
    marks_.push_back({std::move(object), mark.color, 0.0F,
                      prototype->lifetime_seconds, prototype->fade_seconds});
    ++spawned_in_window_;
    ++stats_.spawned_this_update;
    stats_.retained_marks = static_cast<std::uint32_t>(marks_.size());
    return core::Status::ok();
}

core::Status SurfaceMarkRenderer::update(float delta_seconds) {
    if (!is_initialized() || !std::isfinite(delta_seconds) || delta_seconds <= 0.0F ||
        delta_seconds > 0.25F) {
        return core::Status::failure("surface_mark.invalid_update",
                                     "surface mark update requires initialization and valid delta");
    }
    stats_.spawned_this_update = 0;
    stats_.expired_this_update = 0;
    spawned_in_window_ = 0;
    std::vector<RenderSceneUpdate> updates;
    std::size_t write = 0;
    for (std::size_t read = 0; read < marks_.size(); ++read) {
        auto& mark = marks_[read];
        mark.age_seconds += delta_seconds;
        if (mark.lifetime_seconds > 0.0F &&
            mark.age_seconds >= mark.lifetime_seconds) {
            RenderSceneUpdate removal;
            removal.kind = RenderSceneUpdateKind::remove_object;
            removal.object_id = mark.proxy.id;
            updates.push_back(removal);
            ++stats_.expired_this_update;
            continue;
        }
        if (mark.lifetime_seconds > 0.0F && mark.fade_seconds > 0.0F &&
            mark.age_seconds > mark.lifetime_seconds - mark.fade_seconds) {
            const auto alpha =
                std::clamp((mark.lifetime_seconds - mark.age_seconds) /
                               mark.fade_seconds,
                           0.0F, 1.0F);
            mark.proxy.color = mark.base_color;
            mark.proxy.color[3] *= alpha;
            RenderSceneUpdate update;
            update.kind = RenderSceneUpdateKind::upsert_object;
            update.object = mark.proxy;
            updates.push_back(std::move(update));
        }
        if (write != read) {
            marks_[write] = std::move(mark);
        }
        ++write;
    }
    auto status = renderer_->apply_scene_updates(updates);
    if (!status) {
        return status;
    }
    marks_.resize(write);
    stats_.retained_marks = static_cast<std::uint32_t>(marks_.size());
    return core::Status::ok();
}

core::Status SurfaceMarkRenderer::clear() {
    if (!is_initialized()) {
        return core::Status::ok();
    }
    std::vector<RenderSceneUpdate> updates;
    updates.reserve(marks_.size());
    for (const auto& mark : marks_) {
        RenderSceneUpdate update;
        update.kind = RenderSceneUpdateKind::remove_object;
        update.object_id = mark.proxy.id;
        updates.push_back(update);
    }
    auto status = renderer_->apply_scene_updates(updates);
    marks_.clear();
    stats_.retained_marks = 0;
    return status;
}

core::Status SurfaceMarkRenderer::shutdown() {
    if (!is_initialized()) {
        return core::Status::ok();
    }
    auto status = clear();
    auto release = renderer_->release_static_mesh(quad_mesh_);
    if (status && !release) {
        status = release;
    }
    renderer_ = nullptr;
    prototypes_ = nullptr;
    config_ = {};
    quad_mesh_ = {};
    marks_.clear();
    stats_ = {};
    spawned_in_window_ = 0;
    return status;
}

bool SurfaceMarkRenderer::is_initialized() const noexcept {
    return renderer_ != nullptr && prototypes_ != nullptr && quad_mesh_.is_valid();
}

const SurfaceMarkRendererStats& SurfaceMarkRenderer::stats() const noexcept {
    return stats_;
}

} // namespace heartstead::renderer
