#include "engine/renderer/effects/trail_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace heartstead::renderer {

namespace {

[[nodiscard]] core::Result<math::Vec3f>
relative_vector(const world::WorldPosition& from,
                const world::WorldPosition& to) {
    const auto relative = to.relative_to(from.anchor) - from.local_offset;
    if (!relative.is_finite() ||
        std::abs(relative.x) > static_cast<double>(std::numeric_limits<float>::max()) ||
        std::abs(relative.y) > static_cast<double>(std::numeric_limits<float>::max()) ||
        std::abs(relative.z) > static_cast<double>(std::numeric_limits<float>::max())) {
        return core::Result<math::Vec3f>::failure(
            "trail.point_delta_out_of_range",
            "trail point delta cannot be represented by the renderer");
    }
    return core::Result<math::Vec3f>::success(
        {static_cast<float>(relative.x), static_cast<float>(relative.y),
         static_cast<float>(relative.z)});
}

[[nodiscard]] RenderEffectFlags trail_effect_flags(const TrailDesc& desc) noexcept {
    auto flags = RenderEffectFlags::particle | RenderEffectFlags::unlit_particle;
    if (desc.emissive) {
        flags = RenderEffectFlags::particle | RenderEffectFlags::emissive_particle;
    }
    if (desc.layer == RenderLayer::premultiplied) {
        flags = flags | RenderEffectFlags::premultiplied_particle;
    }
    return flags;
}

} // namespace

core::Status TrailDesc::validate() const noexcept {
    if (material_group > 3U ||
        (layer != RenderLayer::transparent && layer != RenderLayer::additive &&
         layer != RenderLayer::premultiplied) ||
        !std::ranges::all_of(color, [](float component) {
            return std::isfinite(component) && component >= 0.0F && component <= 4.0F;
        }) ||
        !std::isfinite(width) || width <= 0.0F || width > 16.0F ||
        !std::isfinite(segment_lifetime_seconds) || segment_lifetime_seconds <= 0.0F ||
        segment_lifetime_seconds > 120.0F || !std::isfinite(minimum_point_distance) ||
        minimum_point_distance <= 0.0F || minimum_point_distance > 64.0F ||
        maximum_segments < 2U || maximum_segments > 16'384U ||
        !std::isfinite(emissive_intensity) || emissive_intensity < 0.0F ||
        emissive_intensity > 64.0F) {
        return core::Status::failure(
            "trail.invalid_desc",
            "trail requires valid material, layer, color, width, lifetime, spacing, and capacity");
    }
    return core::Status::ok();
}

core::Status TrailRendererConfig::validate() const noexcept {
    if (maximum_trails == 0U || maximum_trails > 65'536U ||
        maximum_segments == 0U || maximum_segments > 1'000'000U ||
        std::ranges::any_of(material_groups, [](auto material) {
            return !material.is_valid();
        })) {
        return core::Status::failure(
            "trail.invalid_config",
            "trail renderer requires materials and bounded trail/segment capacities");
    }
    return core::Status::ok();
}

core::Status TrailRenderer::initialize(Renderer& renderer,
                                       TrailRendererConfig config) {
    if (is_initialized()) {
        return core::Status::failure("trail.already_initialized",
                                     "trail renderer is already initialized");
    }
    for (auto& material : config.material_groups) {
        if (!material.is_valid()) {
            material = renderer.fallback_material();
        }
    }
    auto status = config.validate();
    if (!status) {
        return status;
    }
    constexpr std::array<GpuStaticMeshVertex, 4> vertices{{
        {{-0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F}},
        {{0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 1.0F}},
        {{0.5F, 0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F}},
        {{-0.5F, 0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F}},
    }};
    constexpr std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    auto mesh = renderer.create_static_mesh(
        {"builtin:trail_segment_quad", vertices, indices,
         {{-0.5F, -0.5F, -0.01F}, {0.5F, 0.5F, 0.01F}}});
    if (!mesh) {
        return core::Status::failure(mesh.error().code, mesh.error().message);
    }
    renderer_ = &renderer;
    config_ = config;
    quad_mesh_ = mesh.value();
    trails_.reserve(config.maximum_trails);
    return core::Status::ok();
}

core::Result<TrailId> TrailRenderer::create_trail(TrailDesc trail) {
    if (!is_initialized()) {
        return core::Result<TrailId>::failure("trail.not_initialized",
                                              "trail renderer must be initialized first");
    }
    auto status = trail.validate();
    if (!status) {
        return core::Result<TrailId>::failure(status.error().code,
                                              status.error().message);
    }
    std::uint32_t index = 0;
    if (!free_trails_.empty()) {
        index = free_trails_.back();
        free_trails_.pop_back();
    } else {
        if (trails_.size() >= config_.maximum_trails) {
            return core::Result<TrailId>::failure(
                "trail.capacity", "trail pool capacity is exhausted");
        }
        trails_.emplace_back();
        index = static_cast<std::uint32_t>(trails_.size() - 1U);
    }
    auto& slot = trails_[index];
    slot.occupied = true;
    slot.desc = std::move(trail);
    slot.segments.clear();
    slot.has_last_point = false;
    refresh_stats();
    return core::Result<TrailId>::success({index + 1U, slot.generation});
}

TrailRenderer::TrailSlot* TrailRenderer::find(TrailId id) noexcept {
    if (!id.is_valid() || id.index > trails_.size()) {
        return nullptr;
    }
    auto& slot = trails_[id.index - 1U];
    return slot.occupied && slot.generation == id.generation ? &slot : nullptr;
}

core::Status TrailRenderer::append_point(TrailId id,
                                         const world::WorldPosition& point) {
    auto* trail = find(id);
    if (trail == nullptr || !point.is_valid()) {
        return core::Status::failure("trail.stale_id",
                                     "trail append uses invalid point or stale id");
    }
    if (!trail->has_last_point) {
        trail->last_point = point;
        trail->has_last_point = true;
        return core::Status::ok();
    }
    auto delta = relative_vector(trail->last_point, point);
    if (!delta) {
        return core::Status::failure(delta.error().code, delta.error().message);
    }
    if (math::length_squared(delta.value()) <
        trail->desc.minimum_point_distance * trail->desc.minimum_point_distance) {
        return core::Status::ok();
    }
    if (stats_.retained_segments >= config_.maximum_segments) {
        ++stats_.dropped_segments;
        return core::Status::failure("trail.segment_capacity",
                                     "global trail segment capacity is exhausted");
    }
    if (trail->segments.size() >= trail->desc.maximum_segments) {
        auto status = remove_segment(*trail, 0);
        if (!status) {
            return status;
        }
    }
    RenderObjectProxy object;
    object.anchor = trail->last_point;
    object.mesh = quad_mesh_;
    object.material = config_.material_groups[trail->desc.material_group];
    object.local_bounds = {{-0.5F, -0.5F, -0.01F}, {0.5F, 0.5F, 0.01F}};
    object.layer = trail->desc.layer;
    object.flags = RenderObjectFlags::two_sided;
    object.color = trail->desc.color;
    object.effect_flags = trail_effect_flags(trail->desc);
    object.particle_emissive_intensity = trail->desc.emissive_intensity;
    auto created = renderer_->create_object(object);
    if (!created) {
        return core::Status::failure(created.error().code, created.error().message);
    }
    object.id = created.value();
    trail->segments.push_back({trail->last_point, point, std::move(object), 0.0F});
    trail->last_point = point;
    refresh_stats();
    return core::Status::ok();
}

core::Status TrailRenderer::update_segment(Segment& segment, const TrailDesc& trail,
                                           const RenderCamera& camera) {
    auto delta = relative_vector(segment.start, segment.end);
    if (!delta) {
        return core::Status::failure(delta.error().code, delta.error().message);
    }
    const auto length = std::sqrt(math::length_squared(delta.value()));
    if (length <= 0.00001F) {
        return core::Status::ok();
    }
    const auto tangent = delta.value() / length;
    auto midpoint = world::WorldPosition::from_anchor(
        segment.start.anchor,
        segment.start.local_offset +
            math::Vec3d{static_cast<double>(delta.value().x * 0.5F),
                        static_cast<double>(delta.value().y * 0.5F),
                        static_cast<double>(delta.value().z * 0.5F)});
    if (!midpoint) {
        return core::Status::failure(midpoint.error().code, midpoint.error().message);
    }
    auto camera_relative =
        world::to_camera_relative(midpoint.value(), camera.floating_origin);
    if (!camera_relative) {
        return core::Status::failure(camera_relative.error().code,
                                     camera_relative.error().message);
    }
    auto to_camera = camera.local_position - camera_relative.value();
    auto side = math::cross(to_camera, tangent);
    if (math::length_squared(side) < 0.00001F) {
        side = math::cross(math::Vec3f{0.0F, 1.0F, 0.0F}, tangent);
    }
    if (math::length_squared(side) < 0.00001F) {
        side = {1.0F, 0.0F, 0.0F};
    } else {
        side /= std::sqrt(math::length_squared(side));
    }
    const auto normal = math::cross(tangent, side);
    math::Mat4f basis = math::Mat4f::identity();
    basis.at(0, 0) = tangent.x * length;
    basis.at(1, 0) = tangent.y * length;
    basis.at(2, 0) = tangent.z * length;
    basis.at(0, 1) = side.x * trail.width;
    basis.at(1, 1) = side.y * trail.width;
    basis.at(2, 1) = side.z * trail.width;
    basis.at(0, 2) = normal.x;
    basis.at(1, 2) = normal.y;
    basis.at(2, 2) = normal.z;
    segment.proxy.current_transform.position = delta.value() * 0.5F;
    segment.proxy.previous_transform = segment.proxy.current_transform;
    segment.proxy.model_transform = basis;
    segment.proxy.color = trail.color;
    segment.proxy.color[3] *=
        std::clamp(1.0F - segment.age_seconds /
                              trail.segment_lifetime_seconds,
                   0.0F, 1.0F);
    RenderSceneUpdate update;
    update.kind = RenderSceneUpdateKind::upsert_object;
    update.object = segment.proxy;
    return renderer_->apply_scene_updates({&update, 1});
}

core::Status TrailRenderer::remove_segment(TrailSlot& trail, std::size_t index) {
    if (index >= trail.segments.size()) {
        return core::Status::failure("trail.invalid_segment",
                                     "trail segment index is invalid");
    }
    RenderSceneUpdate update;
    update.kind = RenderSceneUpdateKind::remove_object;
    update.object_id = trail.segments[index].proxy.id;
    auto status = renderer_->apply_scene_updates({&update, 1});
    if (!status) {
        return status;
    }
    trail.segments.erase(trail.segments.begin() +
                         static_cast<std::ptrdiff_t>(index));
    return core::Status::ok();
}

core::Status TrailRenderer::update(const RenderCamera& camera, float delta_seconds) {
    if (!is_initialized() || !std::isfinite(delta_seconds) || delta_seconds <= 0.0F ||
        delta_seconds > 0.25F) {
        return core::Status::failure("trail.invalid_update",
                                     "trail update requires initialization and valid delta");
    }
    stats_.submitted_segments = 0;
    stats_.expired_segments = 0;
    for (auto& trail : trails_) {
        if (!trail.occupied) {
            continue;
        }
        std::size_t index = 0;
        while (index < trail.segments.size()) {
            auto& segment = trail.segments[index];
            segment.age_seconds += delta_seconds;
            if (segment.age_seconds >= trail.desc.segment_lifetime_seconds) {
                auto status = remove_segment(trail, index);
                if (!status) {
                    return status;
                }
                ++stats_.expired_segments;
                continue;
            }
            auto status = update_segment(segment, trail.desc, camera);
            if (!status) {
                return status;
            }
            ++stats_.submitted_segments;
            ++index;
        }
    }
    refresh_stats();
    return core::Status::ok();
}

core::Status TrailRenderer::destroy_trail(TrailId id) {
    auto* trail = find(id);
    if (trail == nullptr) {
        return core::Status::failure("trail.stale_id",
                                     "trail removal uses a stale id");
    }
    while (!trail->segments.empty()) {
        auto status = remove_segment(*trail, trail->segments.size() - 1U);
        if (!status) {
            return status;
        }
    }
    trail->occupied = false;
    trail->desc = {};
    trail->last_point = {};
    trail->has_last_point = false;
    ++trail->generation;
    if (trail->generation == 0U) {
        std::terminate();
    }
    free_trails_.push_back(id.index - 1U);
    refresh_stats();
    return core::Status::ok();
}

core::Status TrailRenderer::shutdown() {
    if (!is_initialized()) {
        return core::Status::ok();
    }
    for (std::uint32_t index = 0; index < trails_.size(); ++index) {
        if (trails_[index].occupied) {
            auto status = destroy_trail({index + 1U, trails_[index].generation});
            if (!status) {
                return status;
            }
        }
    }
    auto status = renderer_->release_static_mesh(quad_mesh_);
    renderer_ = nullptr;
    config_ = {};
    quad_mesh_ = {};
    trails_.clear();
    free_trails_.clear();
    stats_ = {};
    return status;
}

bool TrailRenderer::is_initialized() const noexcept {
    return renderer_ != nullptr && quad_mesh_.is_valid();
}

const TrailRendererStats& TrailRenderer::stats() const noexcept {
    return stats_;
}

void TrailRenderer::refresh_stats() noexcept {
    stats_.retained_trails = 0;
    stats_.retained_segments = 0;
    for (const auto& trail : trails_) {
        if (!trail.occupied) {
            continue;
        }
        ++stats_.retained_trails;
        stats_.retained_segments +=
            static_cast<std::uint32_t>(trail.segments.size());
    }
}

} // namespace heartstead::renderer
