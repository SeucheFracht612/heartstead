#include "game/presentation/particle_presentation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numbers>
#include <unordered_set>
#include <utility>
#include <vector>

namespace heartstead::game {

namespace {

constexpr std::array<renderer::GpuStaticMeshVertex, 4> billboard_vertices{{
    {{-0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F}},
    {{0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 1.0F}},
    {{0.5F, 0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F}},
    {{-0.5F, 0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F}},
}};

constexpr std::array<std::uint32_t, 6> billboard_indices{0, 1, 2, 0, 2, 3};

[[nodiscard]] math::Transform3f particle_transform(const world::WorldPosition& position,
                                                   world::BlockCoord anchor, float size,
                                                   const renderer::RenderCamera& camera,
                                                   float roll_degrees) noexcept {
    const auto relative = position.relative_to(anchor);
    math::Transform3f transform;
    transform.position = {static_cast<float>(relative.x), static_cast<float>(relative.y),
                          static_cast<float>(relative.z)};
    constexpr float radians_to_degrees = 180.0F / std::numbers::pi_v<float>;
    transform.rotation_degrees = {
        -camera.pitch_radians * radians_to_degrees,
        camera.yaw_radians * radians_to_degrees + 180.0F,
        roll_degrees,
    };
    transform.scale = {size, size, size};
    return transform;
}

[[nodiscard]] renderer::RenderSceneUpdate
remove_update(renderer::RenderObjectId id) noexcept {
    renderer::RenderSceneUpdate result;
    result.kind = renderer::RenderSceneUpdateKind::remove_object;
    result.object_id = id;
    return result;
}

} // namespace

core::Status ParticlePresentationConfig::validate() const noexcept {
    if (maximum_presented_particles == 0 || maximum_presented_particles > 1'000'000 ||
        std::ranges::any_of(material_groups,
                            [](renderer::MaterialRuntimeHandle material) {
                                return !material.is_valid();
                            })) {
        return core::Status::failure(
            "particle_presentation.invalid_config",
            "particle presentation requires valid material groups and a bounded capacity");
    }
    return core::Status::ok();
}

core::Status ParticlePresentation::initialize(renderer::Renderer& renderer,
                                              ParticlePresentationConfig config) {
    if (initialized_) {
        return core::Status::failure("particle_presentation.already_initialized",
                                     "particle presentation is already initialized");
    }
    auto status = config.validate();
    if (!status) {
        return status;
    }
    const renderer::StaticMeshUploadDesc upload{
        "builtin:particle_billboard",
        billboard_vertices,
        billboard_indices,
        {{-0.5F, -0.5F, -0.01F}, {0.5F, 0.5F, 0.01F}},
    };
    auto mesh = renderer.create_static_mesh(upload);
    if (!mesh) {
        return core::Status::failure(mesh.error().code, mesh.error().message);
    }
    config_ = config;
    billboard_mesh_ = mesh.value();
    retained_.reserve(config_.maximum_presented_particles);
    stats_ = {};
    initialized_ = true;
    return core::Status::ok();
}

core::Result<ParticlePresentationStats>
ParticlePresentation::synchronize(renderer::Renderer& renderer,
                                  const renderer::CpuParticleSystem& particles,
                                  const renderer::RenderCamera& camera) {
    if (!initialized_) {
        return core::Result<ParticlePresentationStats>::failure(
            "particle_presentation.not_initialized",
            "particle presentation must be initialized before synchronization");
    }
    const auto started = std::chrono::steady_clock::now();
    ParticlePresentationStats frame_stats;
    std::unordered_set<std::uint64_t> live;
    const auto sources = particles.particles();
    const auto accepted_count =
        std::min<std::size_t>(sources.size(), config_.maximum_presented_particles);
    live.reserve(accepted_count);
    std::array<bool, 4> used_groups{};
    std::vector<renderer::RenderSceneUpdate> updates;
    updates.reserve(accepted_count);
    for (std::size_t index = 0; index < accepted_count; ++index) {
        const auto& particle = sources[index];
        live.insert(particle.serial);
        used_groups[particle.material_group] = true;
        const auto current_size = particle.size();
        renderer::RenderObjectProxy object;
        const auto found = retained_.find(particle.serial);
        if (found != retained_.end()) {
            object.id = found->second.object;
        }
        object.anchor = particle.position;
        object.previous_transform =
            particle_transform(particle.previous_position, particle.position.anchor, current_size,
                               camera, particle.roll_degrees);
        object.current_transform =
            particle_transform(particle.position, particle.position.anchor, current_size, camera,
                               particle.roll_degrees);
        object.mesh = billboard_mesh_;
        object.material = config_.material_groups[particle.material_group];
        object.local_bounds = {{-0.5F, -0.5F, -0.01F}, {0.5F, 0.5F, 0.01F}};
        object.layer = renderer::RenderLayer::transparent;
        object.flags = renderer::RenderObjectFlags::two_sided;
        object.color = particle.color();
        object.sprite_frame = particle.atlas_frame();
        if (found == retained_.end()) {
            auto created = renderer.create_object(std::move(object));
            if (!created) {
                return core::Result<ParticlePresentationStats>::failure(
                    created.error().code, created.error().message);
            }
            retained_.emplace(particle.serial, RetainedParticle{created.value()});
            ++frame_stats.inserted_particles;
        } else {
            renderer::RenderSceneUpdate update;
            update.kind = renderer::RenderSceneUpdateKind::upsert_object;
            update.object = std::move(object);
            updates.push_back(std::move(update));
            ++frame_stats.updated_particles;
        }
    }
    auto status = renderer.apply_scene_updates(updates);
    if (!status) {
        return core::Result<ParticlePresentationStats>::failure(status.error().code,
                                                                status.error().message);
    }

    std::vector<std::uint64_t> removed;
    std::vector<renderer::RenderSceneUpdate> removal_updates;
    removed.reserve(retained_.size() - std::min(retained_.size(), live.size()));
    for (const auto& [serial, retained] : retained_) {
        if (!live.contains(serial)) {
            removed.push_back(serial);
            removal_updates.push_back(remove_update(retained.object));
        }
    }
    status = renderer.apply_scene_updates(removal_updates);
    if (!status) {
        return core::Result<ParticlePresentationStats>::failure(status.error().code,
                                                                status.error().message);
    }
    for (const auto serial : removed) {
        retained_.erase(serial);
    }
    frame_stats.removed_particles = static_cast<std::uint32_t>(removed.size());
    frame_stats.retained_particles = static_cast<std::uint32_t>(retained_.size());
    frame_stats.material_groups = static_cast<std::uint32_t>(std::ranges::count(used_groups, true));
    frame_stats.dropped_particles = sources.size() - accepted_count;
    frame_stats.synchronize_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    stats_ = frame_stats;
    return core::Result<ParticlePresentationStats>::success(stats_);
}

core::Status ParticlePresentation::shutdown(renderer::Renderer& renderer) {
    if (!initialized_) {
        return core::Status::ok();
    }
    std::vector<renderer::RenderSceneUpdate> updates;
    updates.reserve(retained_.size());
    for (const auto& [_, particle] : retained_) {
        updates.push_back(remove_update(particle.object));
    }
    auto status = renderer.apply_scene_updates(updates);
    if (!status) {
        return status;
    }
    status = renderer.release_static_mesh(billboard_mesh_);
    if (!status) {
        return status;
    }
    retained_.clear();
    billboard_mesh_ = {};
    stats_ = {};
    initialized_ = false;
    return core::Status::ok();
}

bool ParticlePresentation::is_initialized() const noexcept {
    return initialized_;
}

const ParticlePresentationStats& ParticlePresentation::stats() const noexcept {
    return stats_;
}

} // namespace heartstead::game
