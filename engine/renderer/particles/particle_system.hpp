#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/world/coords/world_position.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::renderer {

enum class ParticleBlendMode : std::uint8_t {
    alpha,
    additive,
    premultiplied_alpha,
};

enum class ParticleShading : std::uint8_t {
    lit,
    unlit,
    emissive,
};

enum class ParticleGeometry : std::uint8_t {
    billboard,
    mesh,
};

enum class ParticleAlignment : std::uint8_t {
    camera,
    velocity,
};

enum class ParticleSimulationSpace : std::uint8_t {
    world,
    local,
};

enum class ParticleCollisionMode : std::uint8_t {
    none,
    depth,
    voxel,
};

[[nodiscard]] std::string_view particle_blend_mode_name(ParticleBlendMode value) noexcept;
[[nodiscard]] std::string_view particle_shading_name(ParticleShading value) noexcept;
[[nodiscard]] std::string_view particle_geometry_name(ParticleGeometry value) noexcept;
[[nodiscard]] std::string_view particle_alignment_name(ParticleAlignment value) noexcept;
[[nodiscard]] std::string_view
particle_simulation_space_name(ParticleSimulationSpace value) noexcept;
[[nodiscard]] std::string_view
particle_collision_mode_name(ParticleCollisionMode value) noexcept;

struct ParticlePrototype {
    core::PrototypeId id;
    std::uint8_t material_group = 0;
    float lifetime_min_seconds = 0.5F;
    float lifetime_max_seconds = 1.0F;
    float speed_min = 0.0F;
    float speed_max = 1.0F;
    float direction_spread = 1.0F;
    float gravity = -9.81F;
    float drag = 0.0F;
    float size_min = 0.1F;
    float size_max = 0.2F;
    float end_size_multiplier = 1.0F;
    std::array<float, 4> start_color{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> end_color{1.0F, 1.0F, 1.0F, 0.0F};
    std::uint16_t atlas_columns = 1;
    std::uint16_t atlas_rows = 1;
    std::uint16_t atlas_frame_count = 1;
    float atlas_frames_per_second = 0.0F;
    ParticleBlendMode blend_mode = ParticleBlendMode::alpha;
    ParticleShading shading = ParticleShading::lit;
    ParticleGeometry geometry = ParticleGeometry::billboard;
    ParticleAlignment alignment = ParticleAlignment::camera;
    ParticleSimulationSpace simulation_space = ParticleSimulationSpace::world;
    ParticleCollisionMode collision_mode = ParticleCollisionMode::none;
    std::uint8_t mesh_group = 0;
    float emissive_intensity = 1.0F;
    float wind_response = 0.0F;
    float soft_fade_distance = 0.0F;
    float velocity_stretch = 0.0F;
    float collision_radius = 0.05F;
    float collision_restitution = 0.25F;
    float lod_start_distance = 48.0F;
    float lod_end_distance = 128.0F;
    std::uint32_t maximum_live_particles = 10'000;
    std::uint32_t spawn_budget_per_update = 2'048;
    std::uint8_t priority = 1;

    [[nodiscard]] core::Status validate() const noexcept;
};

struct ParticleEmitEvent {
    core::PrototypeId prototype_id;
    world::WorldPosition position;
    math::Vec3f direction{0.0F, 1.0F, 0.0F};
    math::Vec3f inherited_velocity{};
    std::uint32_t count = 1;
    std::uint64_t seed = 1;
};

struct ParticleEmitterId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return index != 0 && generation != 0;
    }
    friend constexpr auto operator<=>(const ParticleEmitterId&,
                                      const ParticleEmitterId&) = default;
};

struct ParticleEmitterDesc {
    core::PrototypeId prototype_id;
    world::WorldPosition position;
    math::Vec3f direction{0.0F, 1.0F, 0.0F};
    math::Vec3f inherited_velocity{};
    float lifetime_seconds = 1.0F;
    float rate_per_second = 0.0F;
    std::uint32_t burst_count = 0;
    std::uint64_t seed = 1;
};

struct ParticleState {
    std::uint64_t serial = 0;
    core::PrototypeId prototype_id;
    world::WorldPosition previous_position;
    world::WorldPosition position;
    math::Vec3f velocity{};
    float age_seconds = 0.0F;
    float lifetime_seconds = 1.0F;
    float start_size = 1.0F;
    float end_size = 1.0F;
    float roll_degrees = 0.0F;
    float gravity = 0.0F;
    float drag = 0.0F;
    std::array<float, 4> start_color{};
    std::array<float, 4> end_color{};
    std::uint8_t material_group = 0;
    std::uint16_t atlas_columns = 1;
    std::uint16_t atlas_rows = 1;
    std::uint16_t atlas_frame_count = 1;
    float atlas_frames_per_second = 0.0F;
    ParticleBlendMode blend_mode = ParticleBlendMode::alpha;
    ParticleShading shading = ParticleShading::lit;
    ParticleGeometry geometry = ParticleGeometry::billboard;
    ParticleAlignment alignment = ParticleAlignment::camera;
    ParticleSimulationSpace simulation_space = ParticleSimulationSpace::world;
    ParticleCollisionMode collision_mode = ParticleCollisionMode::none;
    ParticleEmitterId source_emitter;
    std::uint8_t mesh_group = 0;
    float emissive_intensity = 1.0F;
    float wind_response = 0.0F;
    float soft_fade_distance = 0.0F;
    float velocity_stretch = 0.0F;
    float collision_radius = 0.05F;
    float collision_restitution = 0.25F;
    std::uint8_t priority = 1;

    [[nodiscard]] float normalized_age() const noexcept;
    [[nodiscard]] float size() const noexcept;
    [[nodiscard]] std::array<float, 4> color() const noexcept;
    [[nodiscard]] std::uint16_t atlas_frame() const noexcept;
};

struct ParticleSystemConfig {
    std::uint32_t maximum_particles = 50'000;
    std::uint32_t maximum_emitters = 1'024;
    std::uint32_t maximum_queued_events = 4'096;
    std::uint32_t maximum_spawns_per_update = 50'000;

    [[nodiscard]] core::Status validate() const noexcept;
};

struct ParticleSystemStats {
    std::uint32_t active_particles = 0;
    std::uint32_t active_emitters = 0;
    std::uint32_t queued_events = 0;
    std::uint32_t spawned_this_update = 0;
    std::uint32_t expired_this_update = 0;
    std::uint64_t dropped_particles = 0;
    std::uint64_t dropped_events = 0;
    std::uint64_t lod_rejected_particles = 0;
    std::uint64_t prototype_budget_rejected_particles = 0;
    std::uint64_t priority_budget_rejected_particles = 0;
    std::uint64_t collision_count = 0;
    double update_ms = 0.0;
};

struct ParticleCollisionHit {
    world::WorldPosition position;
    math::Vec3f normal{0.0F, 1.0F, 0.0F};
};

using ParticleCollisionQuery =
    std::function<std::optional<ParticleCollisionHit>(
        const world::WorldPosition& previous, const world::WorldPosition& proposed,
        float radius)>;

class CpuParticleSystem {
  public:
    [[nodiscard]] static core::Result<CpuParticleSystem>
    create(ParticleSystemConfig config, std::span<const ParticlePrototype> prototypes);

    [[nodiscard]] core::Status queue_event(ParticleEmitEvent event);
    [[nodiscard]] core::Result<ParticleEmitterId> create_emitter(ParticleEmitterDesc emitter);
    [[nodiscard]] core::Status update_emitter(ParticleEmitterId id,
                                              const world::WorldPosition& position,
                                              math::Vec3f direction);
    [[nodiscard]] core::Status destroy_emitter(ParticleEmitterId id);
    void set_environment(math::Vec3f wind_velocity,
                         std::optional<world::WorldPosition> viewpoint = {}) noexcept;
    void set_collision_queries(ParticleCollisionQuery depth_collision,
                               ParticleCollisionQuery voxel_collision);
    [[nodiscard]] core::Status update(float delta_seconds);
    void clear() noexcept;

    [[nodiscard]] const ParticlePrototype*
    find_prototype(const core::PrototypeId& id) const noexcept;
    [[nodiscard]] std::span<const ParticleState> particles() const noexcept;
    [[nodiscard]] const ParticleSystemStats& stats() const noexcept;

  private:
    struct EmitterSlot {
        std::uint32_t generation = 1;
        bool occupied = false;
        bool burst_pending = false;
        ParticleEmitterDesc desc;
        float age_seconds = 0.0F;
        float spawn_accumulator = 0.0F;
        std::uint64_t emission_serial = 0;
    };

    explicit CpuParticleSystem(ParticleSystemConfig config);

    [[nodiscard]] core::Status validate_event(const ParticleEmitEvent& event) const noexcept;
    [[nodiscard]] core::Status validate_emitter(const ParticleEmitterDesc& emitter) const noexcept;
    void spawn(const ParticleEmitEvent& event, std::uint32_t& spawn_budget,
               ParticleEmitterId source_emitter = {});

    ParticleSystemConfig config_{};
    std::vector<ParticlePrototype> prototypes_;
    std::vector<ParticleState> particles_;
    std::vector<ParticleEmitEvent> queued_events_;
    std::vector<EmitterSlot> emitters_;
    std::vector<std::uint32_t> free_emitters_;
    std::uint64_t next_particle_serial_ = 1;
    math::Vec3f wind_velocity_{};
    std::optional<world::WorldPosition> viewpoint_;
    ParticleCollisionQuery depth_collision_;
    ParticleCollisionQuery voxel_collision_;
    std::unordered_map<std::string, std::uint32_t> prototype_spawns_this_update_;
    ParticleSystemStats stats_{};
};

} // namespace heartstead::renderer
