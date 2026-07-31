#include "engine/renderer/particles/particle_system.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>

namespace heartstead::renderer {

namespace {

[[nodiscard]] bool finite_color(const std::array<float, 4>& color) noexcept {
    return std::ranges::all_of(color, [](float value) {
        return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
    });
}

[[nodiscard]] float lerp(float start, float end, float alpha) noexcept {
    return start + (end - start) * alpha;
}

[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] float unit_float(std::uint64_t& state) noexcept {
    state = mix(state);
    constexpr auto scale = 1.0F / static_cast<float>(1U << 24U);
    return static_cast<float>(state >> 40U) * scale;
}

[[nodiscard]] math::Vec3f normalized_or(math::Vec3f value, math::Vec3f fallback) noexcept {
    const auto length_squared = math::length_squared(value);
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-8F) {
        return fallback;
    }
    return value / std::sqrt(length_squared);
}

[[nodiscard]] math::Vec3f random_direction(std::uint64_t& state) noexcept {
    for (std::uint32_t attempt = 0; attempt < 8; ++attempt) {
        const math::Vec3f candidate{unit_float(state) * 2.0F - 1.0F,
                                    unit_float(state) * 2.0F - 1.0F,
                                    unit_float(state) * 2.0F - 1.0F};
        const auto length_squared = math::length_squared(candidate);
        if (length_squared > 1.0e-6F && length_squared <= 1.0F) {
            return candidate / std::sqrt(length_squared);
        }
    }
    return {0.0F, 1.0F, 0.0F};
}

[[nodiscard]] core::Result<world::WorldPosition>
translated(const world::WorldPosition& position, math::Vec3f delta) {
    return world::WorldPosition::from_anchor(
        position.anchor,
        position.local_offset +
            math::Vec3d{static_cast<double>(delta.x), static_cast<double>(delta.y),
                        static_cast<double>(delta.z)});
}

} // namespace

std::string_view particle_blend_mode_name(ParticleBlendMode value) noexcept {
    switch (value) {
    case ParticleBlendMode::alpha:
        return "alpha";
    case ParticleBlendMode::additive:
        return "additive";
    case ParticleBlendMode::premultiplied_alpha:
        return "premultiplied_alpha";
    }
    return "alpha";
}

std::string_view particle_shading_name(ParticleShading value) noexcept {
    switch (value) {
    case ParticleShading::lit:
        return "lit";
    case ParticleShading::unlit:
        return "unlit";
    case ParticleShading::emissive:
        return "emissive";
    }
    return "lit";
}

std::string_view particle_geometry_name(ParticleGeometry value) noexcept {
    return value == ParticleGeometry::mesh ? "mesh" : "billboard";
}

std::string_view particle_alignment_name(ParticleAlignment value) noexcept {
    return value == ParticleAlignment::velocity ? "velocity" : "camera";
}

std::string_view particle_simulation_space_name(ParticleSimulationSpace value) noexcept {
    return value == ParticleSimulationSpace::local ? "local" : "world";
}

std::string_view particle_collision_mode_name(ParticleCollisionMode value) noexcept {
    switch (value) {
    case ParticleCollisionMode::none:
        return "none";
    case ParticleCollisionMode::depth:
        return "depth";
    case ParticleCollisionMode::voxel:
        return "voxel";
    }
    return "none";
}

core::Status ParticlePrototype::validate() const noexcept {
    if (!id.is_valid() || material_group > 3 || !std::isfinite(lifetime_min_seconds) ||
        !std::isfinite(lifetime_max_seconds) || lifetime_min_seconds <= 0.0F ||
        lifetime_max_seconds < lifetime_min_seconds || !std::isfinite(speed_min) ||
        !std::isfinite(speed_max) || speed_min < 0.0F || speed_max < speed_min ||
        !std::isfinite(direction_spread) || direction_spread < 0.0F ||
        direction_spread > 1.0F || !std::isfinite(gravity) || !std::isfinite(drag) ||
        drag < 0.0F || !std::isfinite(size_min) || !std::isfinite(size_max) ||
        size_min <= 0.0F || size_max < size_min || !std::isfinite(end_size_multiplier) ||
        end_size_multiplier < 0.0F || !finite_color(start_color) || !finite_color(end_color) ||
        atlas_columns == 0 || atlas_rows == 0 || atlas_frame_count == 0 ||
        static_cast<std::uint32_t>(atlas_frame_count) >
            static_cast<std::uint32_t>(atlas_columns) * atlas_rows ||
        !std::isfinite(atlas_frames_per_second) || atlas_frames_per_second < 0.0F ||
        mesh_group > 3U || !std::isfinite(emissive_intensity) ||
        emissive_intensity < 0.0F || emissive_intensity > 64.0F ||
        !std::isfinite(wind_response) || wind_response < 0.0F ||
        wind_response > 16.0F || !std::isfinite(soft_fade_distance) ||
        soft_fade_distance < 0.0F || soft_fade_distance > 64.0F ||
        !std::isfinite(velocity_stretch) || velocity_stretch < 0.0F ||
        velocity_stretch > 16.0F || !std::isfinite(collision_radius) ||
        collision_radius < 0.0F || collision_radius > 8.0F ||
        !std::isfinite(collision_restitution) || collision_restitution < 0.0F ||
        collision_restitution > 1.0F || !std::isfinite(lod_start_distance) ||
        !std::isfinite(lod_end_distance) || lod_start_distance < 0.0F ||
        lod_end_distance <= lod_start_distance || maximum_live_particles == 0U ||
        spawn_budget_per_update == 0U || priority > 3U) {
        return core::Status::failure(
            "particle.invalid_prototype",
            "particle prototype contains invalid ranges, color, material, atlas, simulation, "
            "collision, or budget data");
    }
    return core::Status::ok();
}

float ParticleState::normalized_age() const noexcept {
    return lifetime_seconds <= 0.0F ? 1.0F
                                    : std::clamp(age_seconds / lifetime_seconds, 0.0F, 1.0F);
}

float ParticleState::size() const noexcept {
    return lerp(start_size, end_size, normalized_age());
}

std::array<float, 4> ParticleState::color() const noexcept {
    const auto alpha = normalized_age();
    return {lerp(start_color[0], end_color[0], alpha),
            lerp(start_color[1], end_color[1], alpha),
            lerp(start_color[2], end_color[2], alpha),
            lerp(start_color[3], end_color[3], alpha)};
}

std::uint16_t ParticleState::atlas_frame() const noexcept {
    if (atlas_frame_count <= 1 || atlas_frames_per_second <= 0.0F) {
        return 0;
    }
    const auto frame = static_cast<std::uint64_t>(
        std::floor(static_cast<double>(age_seconds) * atlas_frames_per_second));
    return static_cast<std::uint16_t>(frame % atlas_frame_count);
}

core::Status ParticleSystemConfig::validate() const noexcept {
    if (maximum_particles == 0 || maximum_emitters == 0 || maximum_queued_events == 0 ||
        maximum_spawns_per_update == 0 || maximum_particles > 1'000'000 ||
        maximum_emitters > 65'536 || maximum_queued_events > 65'536 ||
        maximum_spawns_per_update > 1'000'000) {
        return core::Status::failure("particle.invalid_config",
                                     "particle pool and queue bounds are invalid");
    }
    return core::Status::ok();
}

CpuParticleSystem::CpuParticleSystem(ParticleSystemConfig config) : config_(config) {
    particles_.reserve(config.maximum_particles);
    queued_events_.reserve(config.maximum_queued_events);
    emitters_.reserve(config.maximum_emitters);
}

core::Result<CpuParticleSystem>
CpuParticleSystem::create(ParticleSystemConfig config,
                          std::span<const ParticlePrototype> prototypes) {
    auto status = config.validate();
    if (!status || prototypes.empty()) {
        return core::Result<CpuParticleSystem>::failure(
            !status ? status.error().code : "particle.missing_prototypes",
            !status ? status.error().message : "particle system requires at least one prototype");
    }
    std::unordered_set<std::string> ids;
    CpuParticleSystem result(config);
    result.prototypes_.reserve(prototypes.size());
    for (const auto& prototype : prototypes) {
        status = prototype.validate();
        if (!status) {
            return core::Result<CpuParticleSystem>::failure(status.error().code,
                                                            status.error().message);
        }
        if (!ids.insert(prototype.id.value()).second) {
            return core::Result<CpuParticleSystem>::failure(
                "particle.duplicate_prototype", "particle prototype ids must be unique");
        }
        result.prototypes_.push_back(prototype);
    }
    std::ranges::sort(result.prototypes_, {},
                      [](const ParticlePrototype& prototype) { return prototype.id.value(); });
    return core::Result<CpuParticleSystem>::success(std::move(result));
}

const ParticlePrototype*
CpuParticleSystem::find_prototype(const core::PrototypeId& id) const noexcept {
    const auto found = std::ranges::lower_bound(
        prototypes_, id.value(), {}, [](const ParticlePrototype& prototype) {
            return prototype.id.value();
        });
    return found != prototypes_.end() && found->id == id ? &*found : nullptr;
}

core::Status CpuParticleSystem::validate_event(const ParticleEmitEvent& event) const noexcept {
    if (find_prototype(event.prototype_id) == nullptr || !event.position.is_valid() ||
        !event.direction.is_finite() || !event.inherited_velocity.is_finite() ||
        event.count == 0 || event.seed == 0) {
        return core::Status::failure(
            "particle.invalid_event",
            "particle event requires a known prototype, finite vectors, count, and seed");
    }
    return core::Status::ok();
}

core::Status CpuParticleSystem::validate_emitter(const ParticleEmitterDesc& emitter) const noexcept {
    ParticleEmitEvent event{emitter.prototype_id, emitter.position, emitter.direction,
                            emitter.inherited_velocity, 1, emitter.seed};
    auto status = validate_event(event);
    if (!status || !std::isfinite(emitter.lifetime_seconds) ||
        !std::isfinite(emitter.rate_per_second) || emitter.lifetime_seconds <= 0.0F ||
        emitter.rate_per_second < 0.0F ||
        (emitter.rate_per_second == 0.0F && emitter.burst_count == 0)) {
        return core::Status::failure(
            "particle.invalid_emitter",
            "particle emitter requires a finite lifetime and a positive rate or burst");
    }
    return core::Status::ok();
}

core::Status CpuParticleSystem::queue_event(ParticleEmitEvent event) {
    auto status = validate_event(event);
    if (!status) {
        return status;
    }
    if (queued_events_.size() >= config_.maximum_queued_events) {
        ++stats_.dropped_events;
        return core::Status::failure("particle.event_queue_full",
                                     "particle event queue reached its configured bound");
    }
    queued_events_.push_back(std::move(event));
    stats_.queued_events = static_cast<std::uint32_t>(queued_events_.size());
    return core::Status::ok();
}

core::Result<ParticleEmitterId>
CpuParticleSystem::create_emitter(ParticleEmitterDesc emitter) {
    auto status = validate_emitter(emitter);
    if (!status) {
        return core::Result<ParticleEmitterId>::failure(status.error().code,
                                                        status.error().message);
    }
    std::uint32_t index = 0;
    if (!free_emitters_.empty()) {
        index = free_emitters_.back();
        free_emitters_.pop_back();
    } else {
        if (emitters_.size() >= config_.maximum_emitters) {
            return core::Result<ParticleEmitterId>::failure(
                "particle.emitter_capacity_exhausted",
                "particle emitter pool reached its configured bound");
        }
        emitters_.emplace_back();
        index = static_cast<std::uint32_t>(emitters_.size() - 1U);
    }
    auto& slot = emitters_[index];
    slot.occupied = true;
    slot.burst_pending = emitter.burst_count != 0;
    slot.desc = std::move(emitter);
    slot.age_seconds = 0.0F;
    slot.spawn_accumulator = 0.0F;
    slot.emission_serial = 0;
    stats_.active_emitters =
        static_cast<std::uint32_t>(emitters_.size() - free_emitters_.size());
    return core::Result<ParticleEmitterId>::success({index + 1U, slot.generation});
}

core::Status CpuParticleSystem::update_emitter(ParticleEmitterId id,
                                               const world::WorldPosition& position,
                                               math::Vec3f direction) {
    if (!id.is_valid() || id.index > emitters_.size() || !position.is_valid() ||
        !direction.is_finite()) {
        return core::Status::failure("particle.stale_emitter",
                                     "particle emitter update uses invalid data");
    }
    auto& slot = emitters_[id.index - 1U];
    if (!slot.occupied || slot.generation != id.generation) {
        return core::Status::failure("particle.stale_emitter",
                                     "particle emitter update uses a stale id");
    }
    if (slot.desc.position != position) {
        const auto delta =
            position.relative_to(slot.desc.position.anchor) -
            slot.desc.position.local_offset;
        if (!delta.is_finite() ||
            std::abs(delta.x) > static_cast<double>(std::numeric_limits<float>::max()) ||
            std::abs(delta.y) > static_cast<double>(std::numeric_limits<float>::max()) ||
            std::abs(delta.z) > static_cast<double>(std::numeric_limits<float>::max())) {
            return core::Status::failure(
                "particle.emitter_delta_out_of_range",
                "local particle emitter movement cannot be represented by simulation");
        }
        const math::Vec3f shift{static_cast<float>(delta.x), static_cast<float>(delta.y),
                                static_cast<float>(delta.z)};
        for (auto& particle : particles_) {
            if (particle.simulation_space != ParticleSimulationSpace::local ||
                particle.source_emitter != id) {
                continue;
            }
            auto shifted = translated(particle.position, shift);
            auto previous = translated(particle.previous_position, shift);
            if (!shifted || !previous) {
                const auto& error = !shifted ? shifted.error() : previous.error();
                return core::Status::failure(error.code, error.message);
            }
            particle.position = shifted.value();
            particle.previous_position = previous.value();
        }
    }
    slot.desc.position = position;
    slot.desc.direction = direction;
    return core::Status::ok();
}

core::Status CpuParticleSystem::destroy_emitter(ParticleEmitterId id) {
    if (!id.is_valid() || id.index > emitters_.size()) {
        return core::Status::failure("particle.stale_emitter",
                                     "particle emitter removal uses a stale id");
    }
    auto& slot = emitters_[id.index - 1U];
    if (!slot.occupied || slot.generation != id.generation) {
        return core::Status::failure("particle.stale_emitter",
                                     "particle emitter removal uses a stale id");
    }
    slot.occupied = false;
    slot.burst_pending = false;
    slot.desc = {};
    ++slot.generation;
    if (slot.generation == 0) {
        std::terminate();
    }
    free_emitters_.push_back(id.index - 1U);
    stats_.active_emitters =
        static_cast<std::uint32_t>(emitters_.size() - free_emitters_.size());
    return core::Status::ok();
}

void CpuParticleSystem::set_environment(
    math::Vec3f wind_velocity,
    std::optional<world::WorldPosition> viewpoint) noexcept {
    wind_velocity_ = wind_velocity.is_finite() ? wind_velocity : math::Vec3f{};
    viewpoint_ = viewpoint.has_value() && viewpoint->is_valid() ? viewpoint : std::nullopt;
}

void CpuParticleSystem::set_collision_queries(ParticleCollisionQuery depth_collision,
                                              ParticleCollisionQuery voxel_collision) {
    depth_collision_ = std::move(depth_collision);
    voxel_collision_ = std::move(voxel_collision);
}

void CpuParticleSystem::spawn(const ParticleEmitEvent& event, std::uint32_t& spawn_budget,
                              ParticleEmitterId source_emitter) {
    const auto* prototype = find_prototype(event.prototype_id);
    if (prototype == nullptr) {
        stats_.dropped_particles += event.count;
        return;
    }
    std::uint32_t requested = event.count;
    if (viewpoint_.has_value()) {
        const auto relative =
            event.position.relative_to(viewpoint_->anchor) - viewpoint_->local_offset;
        const auto distance = std::sqrt(relative.x * relative.x + relative.y * relative.y +
                                        relative.z * relative.z);
        const auto lod_factor =
            std::clamp(1.0 - (distance - prototype->lod_start_distance) /
                                 (prototype->lod_end_distance - prototype->lod_start_distance),
                       0.0, 1.0);
        auto random = event.seed;
        const auto scaled = static_cast<double>(event.count) * lod_factor;
        requested = static_cast<std::uint32_t>(std::floor(scaled));
        if (unit_float(random) < static_cast<float>(scaled - std::floor(scaled))) {
            ++requested;
        }
        stats_.lod_rejected_particles += event.count - requested;
    }
    const auto live_count = static_cast<std::uint32_t>(std::ranges::count_if(
        particles_, [&](const ParticleState& particle) {
            return particle.prototype_id == prototype->id;
        }));
    const auto live_remaining =
        prototype->maximum_live_particles > live_count
            ? prototype->maximum_live_particles - live_count
            : 0U;
    auto& prototype_spawns = prototype_spawns_this_update_[prototype->id.value()];
    const auto prototype_budget_remaining =
        prototype->spawn_budget_per_update > prototype_spawns
            ? prototype->spawn_budget_per_update - prototype_spawns
            : 0U;
    const auto capacity = config_.maximum_particles - static_cast<std::uint32_t>(particles_.size());
    const auto accepted =
        std::min({requested, capacity, spawn_budget, live_remaining,
                  prototype_budget_remaining});
    stats_.prototype_budget_rejected_particles += requested - accepted;
    stats_.dropped_particles += static_cast<std::uint64_t>(event.count - accepted);
    spawn_budget -= accepted;
    prototype_spawns += accepted;

    const auto base_direction = normalized_or(event.direction, {0.0F, 1.0F, 0.0F});
    for (std::uint32_t index = 0; index < accepted; ++index) {
        auto random = mix(event.seed ^ static_cast<std::uint64_t>(index) ^
                          (next_particle_serial_ * 0x9e3779b97f4a7c15ULL));
        const auto scatter = random_direction(random);
        const auto direction = normalized_or(
            base_direction * (1.0F - prototype->direction_spread) +
                scatter * prototype->direction_spread,
            base_direction);
        const auto speed =
            lerp(prototype->speed_min, prototype->speed_max, unit_float(random));
        const auto lifetime = lerp(prototype->lifetime_min_seconds,
                                   prototype->lifetime_max_seconds, unit_float(random));
        const auto start_size =
            lerp(prototype->size_min, prototype->size_max, unit_float(random));
        ParticleState particle;
        particle.serial = next_particle_serial_++;
        if (next_particle_serial_ == 0) {
            std::terminate();
        }
        particle.prototype_id = prototype->id;
        particle.previous_position = event.position;
        particle.position = event.position;
        particle.velocity = event.inherited_velocity + direction * speed;
        particle.lifetime_seconds = lifetime;
        particle.start_size = start_size;
        particle.end_size = start_size * prototype->end_size_multiplier;
        particle.roll_degrees = unit_float(random) * 360.0F;
        particle.gravity = prototype->gravity;
        particle.drag = prototype->drag;
        particle.start_color = prototype->start_color;
        particle.end_color = prototype->end_color;
        particle.material_group = prototype->material_group;
        particle.atlas_columns = prototype->atlas_columns;
        particle.atlas_rows = prototype->atlas_rows;
        particle.atlas_frame_count = prototype->atlas_frame_count;
        particle.atlas_frames_per_second = prototype->atlas_frames_per_second;
        particle.blend_mode = prototype->blend_mode;
        particle.shading = prototype->shading;
        particle.geometry = prototype->geometry;
        particle.alignment = prototype->alignment;
        particle.simulation_space = prototype->simulation_space;
        particle.collision_mode = prototype->collision_mode;
        particle.source_emitter = source_emitter;
        particle.mesh_group = prototype->mesh_group;
        particle.emissive_intensity = prototype->emissive_intensity;
        particle.wind_response = prototype->wind_response;
        particle.soft_fade_distance = prototype->soft_fade_distance;
        particle.velocity_stretch = prototype->velocity_stretch;
        particle.collision_radius = prototype->collision_radius;
        particle.collision_restitution = prototype->collision_restitution;
        particles_.push_back(std::move(particle));
    }
    stats_.spawned_this_update += accepted;
}

core::Status CpuParticleSystem::update(float delta_seconds) {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F || delta_seconds > 0.25F) {
        return core::Status::failure("particle.invalid_delta",
                                     "particle update delta must be in (0, 0.25]");
    }
    const auto started = std::chrono::steady_clock::now();
    stats_.spawned_this_update = 0;
    stats_.expired_this_update = 0;
    prototype_spawns_this_update_.clear();
    std::uint32_t spawn_budget = config_.maximum_spawns_per_update;

    for (const auto& event : queued_events_) {
        spawn(event, spawn_budget);
    }
    queued_events_.clear();
    stats_.queued_events = 0;

    std::vector<ParticleEmitterId> expired_emitters;
    for (std::uint32_t index = 0; index < emitters_.size(); ++index) {
        auto& emitter = emitters_[index];
        if (!emitter.occupied) {
            continue;
        }
        std::uint32_t count = 0;
        if (emitter.burst_pending) {
            count += emitter.desc.burst_count;
            emitter.burst_pending = false;
        }
        const auto active_seconds =
            std::min(delta_seconds, emitter.desc.lifetime_seconds - emitter.age_seconds);
        emitter.spawn_accumulator += emitter.desc.rate_per_second * active_seconds;
        const auto rate_count = static_cast<std::uint32_t>(std::floor(emitter.spawn_accumulator));
        emitter.spawn_accumulator -= static_cast<float>(rate_count);
        count += rate_count;
        if (count != 0) {
            const ParticleEmitEvent event{
                emitter.desc.prototype_id,
                emitter.desc.position,
                emitter.desc.direction,
                emitter.desc.inherited_velocity,
                count,
                mix(emitter.desc.seed ^ emitter.emission_serial++),
            };
            spawn(event, spawn_budget, {index + 1U, emitter.generation});
        }
        emitter.age_seconds += delta_seconds;
        if (emitter.age_seconds >= emitter.desc.lifetime_seconds) {
            expired_emitters.push_back({index + 1U, emitter.generation});
        }
    }
    for (const auto id : expired_emitters) {
        (void)destroy_emitter(id);
    }

    std::size_t write = 0;
    for (std::size_t read = 0; read < particles_.size(); ++read) {
        auto& particle = particles_[read];
        particle.previous_position = particle.position;
        particle.velocity.y += particle.gravity * delta_seconds;
        if (particle.wind_response > 0.0F) {
            const auto wind_blend =
                1.0F - std::exp(-particle.wind_response * delta_seconds);
            particle.velocity += (wind_velocity_ - particle.velocity) * wind_blend;
        }
        particle.velocity *= std::max(0.0F, 1.0F - particle.drag * delta_seconds);
        if (math::length_squared(particle.velocity) > 0.0F) {
            auto next = translated(particle.position, particle.velocity * delta_seconds);
            if (!next) {
                return core::Status::failure(next.error().code, next.error().message);
            }
            const auto* query =
                particle.collision_mode == ParticleCollisionMode::depth
                    ? &depth_collision_
                    : particle.collision_mode == ParticleCollisionMode::voxel
                          ? &voxel_collision_
                          : nullptr;
            if (query != nullptr && *query) {
                const auto hit = (*query)(particle.position, next.value(),
                                          particle.collision_radius);
                if (hit.has_value() && hit->position.is_valid() &&
                    hit->normal.is_finite() &&
                    math::length_squared(hit->normal) > 0.00001F) {
                    const auto normal =
                        hit->normal / std::sqrt(math::length_squared(hit->normal));
                    const auto normal_velocity = math::dot(particle.velocity, normal);
                    if (normal_velocity < 0.0F) {
                        particle.velocity -=
                            normal * normal_velocity *
                            (1.0F + particle.collision_restitution);
                    }
                    particle.position = hit->position;
                    ++stats_.collision_count;
                } else {
                    particle.position = next.value();
                }
            } else {
                particle.position = next.value();
            }
        }
        particle.age_seconds += delta_seconds;
        if (particle.age_seconds >= particle.lifetime_seconds) {
            ++stats_.expired_this_update;
            continue;
        }
        if (write != read) {
            particles_[write] = std::move(particle);
        }
        ++write;
    }
    particles_.resize(write);
    stats_.active_particles = static_cast<std::uint32_t>(particles_.size());
    stats_.active_emitters =
        static_cast<std::uint32_t>(emitters_.size() - free_emitters_.size());
    stats_.update_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    return core::Status::ok();
}

void CpuParticleSystem::clear() noexcept {
    particles_.clear();
    queued_events_.clear();
    emitters_.clear();
    free_emitters_.clear();
    next_particle_serial_ = 1;
    prototype_spawns_this_update_.clear();
    stats_ = {};
}

std::span<const ParticleState> CpuParticleSystem::particles() const noexcept {
    return particles_;
}

const ParticleSystemStats& CpuParticleSystem::stats() const noexcept {
    return stats_;
}

} // namespace heartstead::renderer
