#pragma once

#include "engine/core/result.hpp"
#include "engine/entities/physical_resource.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/world/coords/world_position.hpp"
#include "engine/world/world_state.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

namespace heartstead::world {
class VoxelPalette;
}

namespace heartstead::physics {

struct PhysicalResourcePhysicsSystemConfig {
    world::PhysicsIslandFrame physics_island{};
    std::uint32_t freeze_after_sleeping_ticks = 120;
    bool fluid_buoyancy = true;
    float fluid_density_kg_per_cubic_meter = 1'000.0F;
    float buoyancy_acceleration = 9.81F;
    float fluid_linear_drag = 2.5F;

    [[nodiscard]] core::Status validate() const;
};

struct PhysicalResourcePhysicsSystemStats {
    std::size_t active_body_count = 0;
    std::size_t dynamic_body_count = 0;
    std::size_t sleeping_body_count = 0;
    std::size_t frozen_body_count = 0;
    std::uint32_t created_this_tick = 0;
    std::uint32_t restored_this_tick = 0;
    std::uint32_t synchronized_this_tick = 0;
    std::uint32_t settled_this_tick = 0;
    std::uint32_t woken_this_tick = 0;
    std::uint32_t frozen_this_tick = 0;
    std::uint32_t buoyant_this_tick = 0;
    std::uint64_t created_bodies = 0;
    std::uint64_t restored_bodies = 0;
    std::uint64_t synchronized_bodies = 0;
    std::uint64_t settled_bodies = 0;
    std::uint64_t woken_bodies = 0;
    std::uint64_t frozen_bodies = 0;
    std::uint64_t buoyant_bodies = 0;
};

class PhysicalResourcePhysicsSystem final {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<PhysicalResourcePhysicsSystem>>
    create(IPhysicsWorld& physics_world, PhysicalResourcePhysicsSystemConfig config = {});

    ~PhysicalResourcePhysicsSystem();

    PhysicalResourcePhysicsSystem(const PhysicalResourcePhysicsSystem&) = delete;
    PhysicalResourcePhysicsSystem& operator=(const PhysicalResourcePhysicsSystem&) = delete;

    [[nodiscard]] core::Status activate(entities::PhysicalResourceRecord& resource,
                                        Vec3 linear_velocity = {}, Vec3 angular_velocity = {});
    [[nodiscard]] core::Status prepare(world::WorldState& world);
    [[nodiscard]] core::Status prepare(world::WorldState& world,
                                       const world::VoxelPalette& palette,
                                       float fixed_delta_seconds);
    [[nodiscard]] core::Status synchronize(world::WorldState& world);
    void shutdown() noexcept;

    [[nodiscard]] const PhysicalResourcePhysicsSystemStats& stats() const noexcept;

  private:
    PhysicalResourcePhysicsSystem(IPhysicsWorld& physics_world,
                                  PhysicalResourcePhysicsSystemConfig config);

    [[nodiscard]] core::Status create_attached_body(entities::PhysicalResourceRecord& resource,
                                                    bool restored);
    [[nodiscard]] core::Status replace_with_static_body(entities::PhysicalResourceRecord& resource);
    [[nodiscard]] core::Status apply_fluid_forces(world::WorldState& world,
                                                  const world::VoxelPalette& palette,
                                                  float fixed_delta_seconds);
    void reset_tick_stats() noexcept;
    void refresh_counts(const world::PhysicalResourceDatabase& resources) noexcept;

    IPhysicsWorld* physics_world_ = nullptr;
    PhysicalResourcePhysicsSystemConfig config_{};
    std::map<std::uint64_t, PhysicsBodyId> owned_bodies_;
    std::map<std::uint64_t, std::uint32_t> sleeping_ticks_;
    PhysicalResourcePhysicsSystemStats stats_{};
};

} // namespace heartstead::physics
