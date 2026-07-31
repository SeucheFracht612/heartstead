#include "engine/renderer/water/large_water_renderer.hpp"

#include "engine/renderer/assets/mesh_manager.hpp"
#include "engine/renderer/renderer.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace heartstead::renderer {

namespace {

[[nodiscard]] float quadratic_coordinate(std::uint32_t index,
                                         std::uint32_t resolution) noexcept {
    const auto normalized =
        static_cast<float>(index) / static_cast<float>(resolution - 1U) * 2.0F - 1.0F;
    return std::copysign(normalized * normalized, normalized);
}

[[nodiscard]] std::int64_t snap_axis(std::int64_t block, double local,
                                     float distance) noexcept {
    const auto position = static_cast<long double>(block) + static_cast<long double>(local);
    const auto snapped =
        std::floor(position / static_cast<long double>(distance)) *
        static_cast<long double>(distance);
    if (snapped <= static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if (snapped >= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(snapped);
}

} // namespace

core::Status LargeWaterRendererConfig::validate() const noexcept {
    if (grid_resolution < 17U || grid_resolution > 257U ||
        (grid_resolution & 1U) == 0U || !std::isfinite(camera_snap_distance) ||
        camera_snap_distance <= 0.0F || camera_snap_distance > 256.0F ||
        maximum_bodies == 0U || maximum_bodies > 4'096U) {
        return core::Status::failure(
            "large_water.invalid_config",
            "large-water grid must be odd and bounded, with valid snap and body capacities");
    }
    return core::Status::ok();
}

core::Status LargeWaterBodyDesc::validate() const noexcept {
    if (id == 0 || !center.is_valid() || !std::isfinite(half_extent) ||
        half_extent <= 0.0F || half_extent > 8'192.0F ||
        !std::isfinite(wave_height) || wave_height < 0.0F || wave_height > 8.0F ||
        !std::isfinite(wave_speed) || wave_speed < 0.0F || wave_speed > 16.0F ||
        !std::isfinite(optical_depth) || optical_depth <= 0.0F ||
        optical_depth > 1'024.0F || !std::isfinite(foam_strength) ||
        foam_strength < 0.0F || foam_strength > 4.0F) {
        return core::Status::failure(
            "large_water.invalid_body",
            "large-water body requires valid position, extent, wave, optical, and foam settings");
    }
    return core::Status::ok();
}

core::Status LargeWaterRenderer::initialize(Renderer& renderer,
                                            LargeWaterRendererConfig config) {
    if (is_initialized()) {
        return core::Status::failure("large_water.already_initialized",
                                     "large-water renderer is already initialized");
    }
    auto status = config.validate();
    if (!status) {
        return status;
    }
    if (!renderer.is_initialized()) {
        return core::Status::failure("large_water.renderer_not_initialized",
                                     "large-water renderer requires an initialized renderer");
    }
    const auto vertex_count =
        static_cast<std::size_t>(config.grid_resolution) * config.grid_resolution;
    const auto cell_count =
        static_cast<std::size_t>(config.grid_resolution - 1U) *
        (config.grid_resolution - 1U);
    std::vector<GpuStaticMeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(vertex_count);
    indices.reserve(cell_count * 6U);
    for (std::uint32_t z = 0; z < config.grid_resolution; ++z) {
        for (std::uint32_t x = 0; x < config.grid_resolution; ++x) {
            const auto local_x = quadratic_coordinate(x, config.grid_resolution);
            const auto local_z = quadratic_coordinate(z, config.grid_resolution);
            GpuStaticMeshVertex vertex;
            vertex.position[0] = local_x;
            vertex.position[1] = 0.0F;
            vertex.position[2] = local_z;
            vertex.normal[0] = 0;
            vertex.normal[1] = std::numeric_limits<std::int16_t>::max();
            vertex.normal[2] = 0;
            vertex.tangent[0] = std::numeric_limits<std::int16_t>::max();
            vertex.tangent[1] = 0;
            vertex.tangent[2] = 0;
            vertex.tangent[3] = std::numeric_limits<std::int16_t>::max();
            vertex.uv0[0] = local_x * 0.5F + 0.5F;
            vertex.uv0[1] = local_z * 0.5F + 0.5F;
            vertices.push_back(vertex);
        }
    }
    for (std::uint32_t z = 0; z + 1U < config.grid_resolution; ++z) {
        for (std::uint32_t x = 0; x + 1U < config.grid_resolution; ++x) {
            const auto first = z * config.grid_resolution + x;
            const auto second = first + 1U;
            const auto third = first + config.grid_resolution;
            const auto fourth = third + 1U;
            indices.insert(indices.end(), {first, third, second, second, third, fourth});
        }
    }
    StaticMeshUploadDesc upload{
        "builtin:large_water_quadratic_grid_" + std::to_string(config.grid_resolution),
        vertices,
        indices,
        {{-1.0F, -8.0F, -1.0F}, {1.0F, 8.0F, 1.0F}},
    };
    auto mesh = renderer.create_static_mesh(upload);
    if (!mesh) {
        return core::Status::failure(mesh.error().code, mesh.error().message);
    }
    renderer_ = &renderer;
    config_ = config;
    mesh_ = mesh.value();
    stats_.mesh_vertices = static_cast<std::uint32_t>(vertices.size());
    stats_.mesh_triangles = static_cast<std::uint32_t>(indices.size() / 3U);
    return core::Status::ok();
}

core::Result<RenderObjectProxy>
LargeWaterRenderer::make_proxy(const LargeWaterBodyDesc& body) const {
    auto status = body.validate();
    if (!status) {
        return core::Result<RenderObjectProxy>::failure(status.error().code,
                                                        status.error().message);
    }
    RenderObjectProxy object;
    object.anchor = body.center;
    object.previous_transform.scale = {body.half_extent, 1.0F, body.half_extent};
    object.current_transform = object.previous_transform;
    object.mesh = mesh_;
    object.material = renderer_->fallback_material();
    object.local_bounds = {{-1.0F, -body.wave_height * 2.0F - 0.1F, -1.0F},
                           {1.0F, body.wave_height * 2.0F + 0.1F, 1.0F}};
    object.layer = RenderLayer::transparent;
    object.flags = RenderObjectFlags::two_sided;
    object.effect_flags = RenderEffectFlags::water_surface;
    object.water_wave_height = body.wave_height;
    object.water_wave_speed = body.wave_speed;
    object.water_optical_depth = body.optical_depth;
    object.water_foam_strength = body.foam_strength;
    object.maximum_view_distance = body.half_extent * 1.45F;
    return core::Result<RenderObjectProxy>::success(std::move(object));
}

core::Status LargeWaterRenderer::add_body(LargeWaterBodyDesc body) {
    if (!is_initialized()) {
        return core::Status::failure("large_water.not_initialized",
                                     "large-water renderer must be initialized first");
    }
    if (bodies_.contains(body.id)) {
        return core::Status::failure("large_water.duplicate_body",
                                     "large-water body id is already retained");
    }
    if (bodies_.size() >= config_.maximum_bodies) {
        return core::Status::failure("large_water.body_capacity",
                                     "large-water body capacity is exhausted");
    }
    auto proxy = make_proxy(body);
    if (!proxy) {
        return core::Status::failure(proxy.error().code, proxy.error().message);
    }
    auto created = renderer_->create_object(proxy.value());
    if (!created) {
        return core::Status::failure(created.error().code, created.error().message);
    }
    proxy.value().id = created.value();
    bodies_.emplace(body.id, RetainedBody{std::move(body), std::move(proxy).value()});
    refresh_stats();
    return core::Status::ok();
}

core::Status LargeWaterRenderer::update_body(const LargeWaterBodyDesc& body) {
    if (!is_initialized()) {
        return core::Status::failure("large_water.not_initialized",
                                     "large-water renderer must be initialized first");
    }
    const auto found = bodies_.find(body.id);
    if (found == bodies_.end()) {
        return core::Status::failure("large_water.missing_body",
                                     "large-water body id is not retained");
    }
    auto proxy = make_proxy(body);
    if (!proxy) {
        return core::Status::failure(proxy.error().code, proxy.error().message);
    }
    proxy.value().id = found->second.proxy.id;
    RenderSceneUpdate update;
    update.kind = RenderSceneUpdateKind::upsert_object;
    update.object = proxy.value();
    auto status = renderer_->apply_scene_updates({&update, 1});
    if (!status) {
        return status;
    }
    found->second = {body, std::move(proxy).value()};
    refresh_stats();
    return core::Status::ok();
}

core::Status LargeWaterRenderer::remove_body(std::uint64_t id) {
    if (!is_initialized()) {
        return core::Status::failure("large_water.not_initialized",
                                     "large-water renderer must be initialized first");
    }
    const auto found = bodies_.find(id);
    if (found == bodies_.end()) {
        return core::Status::failure("large_water.missing_body",
                                     "large-water body id is not retained");
    }
    RenderSceneUpdate update;
    update.kind = RenderSceneUpdateKind::remove_object;
    update.object_id = found->second.proxy.id;
    auto status = renderer_->apply_scene_updates({&update, 1});
    if (!status) {
        return status;
    }
    bodies_.erase(found);
    refresh_stats();
    return core::Status::ok();
}

core::Result<world::WorldPosition>
LargeWaterRenderer::snapped_ocean_center(const LargeWaterBodyDesc& body,
                                         const RenderCamera& camera) const {
    auto camera_position = world::WorldPosition::from_anchor(
        camera.floating_origin.block,
        {static_cast<double>(camera.local_position.x),
         static_cast<double>(camera.local_position.y),
         static_cast<double>(camera.local_position.z)});
    if (!camera_position) {
        return camera_position;
    }
    const auto x = snap_axis(camera_position.value().anchor.x,
                             camera_position.value().local_offset.x,
                             config_.camera_snap_distance);
    const auto z = snap_axis(camera_position.value().anchor.z,
                             camera_position.value().local_offset.z,
                             config_.camera_snap_distance);
    return world::WorldPosition::from_anchor(
        {x, body.center.anchor.y, z}, {0.0, body.center.local_offset.y, 0.0});
}

core::Status LargeWaterRenderer::synchronize(const RenderCamera& camera) {
    if (!is_initialized()) {
        return core::Status::failure("large_water.not_initialized",
                                     "large-water renderer must be initialized first");
    }
    std::vector<RenderSceneUpdate> updates;
    for (auto& [id, body] : bodies_) {
        (void)id;
        if (!body.desc.follows_camera) {
            continue;
        }
        auto center = snapped_ocean_center(body.desc, camera);
        if (!center) {
            return core::Status::failure(center.error().code, center.error().message);
        }
        if (center.value() == body.proxy.anchor) {
            continue;
        }
        body.proxy.anchor = center.value();
        RenderSceneUpdate update;
        update.kind = RenderSceneUpdateKind::upsert_object;
        update.object = body.proxy;
        updates.push_back(std::move(update));
        ++stats_.camera_recenters;
    }
    return renderer_->apply_scene_updates(updates);
}

core::Status LargeWaterRenderer::shutdown() {
    if (!is_initialized()) {
        return core::Status::ok();
    }
    std::vector<RenderSceneUpdate> updates;
    updates.reserve(bodies_.size());
    for (const auto& [id, body] : bodies_) {
        (void)id;
        RenderSceneUpdate update;
        update.kind = RenderSceneUpdateKind::remove_object;
        update.object_id = body.proxy.id;
        updates.push_back(update);
    }
    auto status = renderer_->apply_scene_updates(updates);
    auto release = renderer_->release_static_mesh(mesh_);
    if (status && !release) {
        status = release;
    }
    bodies_.clear();
    renderer_ = nullptr;
    config_ = {};
    mesh_ = {};
    stats_ = {};
    return status;
}

bool LargeWaterRenderer::is_initialized() const noexcept {
    return renderer_ != nullptr && mesh_.is_valid();
}

const LargeWaterRendererStats& LargeWaterRenderer::stats() const noexcept {
    return stats_;
}

void LargeWaterRenderer::refresh_stats() noexcept {
    const auto vertices = stats_.mesh_vertices;
    const auto triangles = stats_.mesh_triangles;
    const auto recenters = stats_.camera_recenters;
    stats_ = {};
    stats_.mesh_vertices = vertices;
    stats_.mesh_triangles = triangles;
    stats_.camera_recenters = recenters;
    stats_.retained_bodies = static_cast<std::uint32_t>(bodies_.size());
    for (const auto& [id, body] : bodies_) {
        (void)id;
        stats_.ocean_bodies += body.desc.follows_camera ? 1U : 0U;
    }
}

} // namespace heartstead::renderer
