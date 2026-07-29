#pragma once

#include "engine/entities/entity_motion_snapshot.hpp"
#include "engine/entities/entity_world.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/movement/movement_prediction.hpp"
#include "engine/movement/physics_character_collision.hpp"
#include "engine/movement/player_controller_store.hpp"
#include "engine/net/host_session.hpp"
#include "engine/physics/chunk_collision_system.hpp"
#include "engine/physics/physical_resource_physics_system.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/save/save_snapshot.hpp"
#include "engine/scenarios/scenario.hpp"
#include "engine/simulation/simulation_scheduler.hpp"
#include "engine/simulation/world_time.hpp"
#include "engine/world/fluids/chunk_fluid_system.hpp"
#include "engine/world/lighting/chunk_light_system.hpp"
#include "engine/world/replication_delta.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "engine/world/world_state.hpp"
#include "game/framework/gameplay_module.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>

namespace heartstead::game {

struct ServerRuntimeDesc {
    world::WorldStateDesc world;
    net::HostSessionConfig host;
    physics::PhysicsWorldDesc physics;
    physics::ChunkCollisionSystemConfig chunk_collision;
    physics::PhysicalResourcePhysicsSystemConfig physical_resources;
    world::ChunkFluidSystemConfig chunk_fluids;
    world::ChunkLightSystemConfig chunk_lighting;
    std::uint32_t simulation_ticks_per_second = 60;
    std::uint32_t max_transient_snapshot_messages_per_tick = 512;
    std::uint32_t max_transient_snapshot_payload_bytes_per_tick = 256u * 1024u;
    simulation::WorldTimeConfig world_time;
    const modding::PrototypeRegistry* prototypes = nullptr;
    const world::VoxelPalette* voxel_palette = nullptr;
    scenarios::ScenarioDefinition scenario;
    std::optional<save::SaveSnapshot> initial_snapshot;
    std::vector<std::shared_ptr<IGameplayModule>> gameplay_modules;
};

struct ServerRuntimeTickStats {
    simulation::SimulationTickStats simulation;
    net::HostSessionTickResult commands;
    world::WorldReplicationDeltaDeliveryReport replication;
    physics::PhysicsStepStats physics;
    physics::ChunkCollisionSystemStats chunk_collision;
    physics::PhysicalResourcePhysicsSystemStats physical_resources;
    world::ChunkFluidSystemStats chunk_fluids;
    world::ChunkLightSystemStats chunk_lighting;
    std::uint32_t moved_player_count = 0;
    std::uint32_t repeated_input_count = 0;
    std::uint32_t movement_event_count = 0;
    std::uint32_t accepted_movement_input_count = 0;
    std::uint32_t rejected_movement_input_count = 0;
    std::uint32_t movement_snapshot_count = 0;
    std::uint32_t entity_motion_snapshot_count = 0;
    std::uint32_t entity_motion_tombstone_count = 0;
    std::uint32_t player_tombstone_count = 0;
    std::uint32_t deferred_transient_snapshot_count = 0;
    std::uint32_t transient_snapshot_payload_bytes = 0;
};

class ServerRuntime final {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<ServerRuntime>>
    create(ServerRuntimeDesc desc);

    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;

    [[nodiscard]] core::Status start();
    [[nodiscard]] core::Status stop();
    [[nodiscard]] core::Result<ServerRuntimeTickStats>
    run_tick(std::uint64_t tick, double fixed_delta_seconds, std::int64_t now_ms);

    [[nodiscard]] core::Result<core::NetId> connect_client();
    [[nodiscard]] core::Status disconnect_client(core::NetId client_id);
    [[nodiscard]] core::Status submit_command(core::NetId client_id, net::CommandEnvelope command);
    [[nodiscard]] core::Status submit_movement_input(core::NetId client_id,
                                                     movement::PlayerInputBundle bundle,
                                                     std::int64_t now_ms = 0);
    [[nodiscard]] core::Result<std::vector<net::TransportEnvelope>>
    drain_client_messages(core::NetId client_id);

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] world::WorldState& world() noexcept;
    [[nodiscard]] const world::WorldState& world() const noexcept;
    [[nodiscard]] entities::EntityWorld& entities() noexcept;
    [[nodiscard]] const entities::EntityWorld& entities() const noexcept;
    [[nodiscard]] net::HostSession& host() noexcept;
    [[nodiscard]] const net::HostSession& host() const noexcept;
    [[nodiscard]] const simulation::SimulationScheduler& scheduler() const noexcept;
    [[nodiscard]] physics::ChunkCollisionSystem& chunk_collision() noexcept;
    [[nodiscard]] const physics::ChunkCollisionSystem& chunk_collision() const noexcept;
    [[nodiscard]] physics::PhysicalResourcePhysicsSystem& physical_resource_physics() noexcept;
    [[nodiscard]] const physics::PhysicalResourcePhysicsSystem&
    physical_resource_physics() const noexcept;
    [[nodiscard]] world::ChunkFluidSystem& chunk_fluids() noexcept;
    [[nodiscard]] const world::ChunkFluidSystem& chunk_fluids() const noexcept;
    [[nodiscard]] world::ChunkLightSystem& chunk_lighting() noexcept;
    [[nodiscard]] const world::ChunkLightSystem& chunk_lighting() const noexcept;
    [[nodiscard]] core::Status drop_physical_resource(entities::PhysicalResourceRecord resource,
                                                      physics::Vec3 linear_velocity = {},
                                                      physics::Vec3 angular_velocity = {});
    [[nodiscard]] const simulation::TickEvents& events() const noexcept;
    [[nodiscard]] movement::PlayerControllerStore& players() noexcept;
    [[nodiscard]] const movement::PlayerControllerStore& players() const noexcept;
    [[nodiscard]] const GameplayModuleRegistry& gameplay_modules() const noexcept;
    [[nodiscard]] const ComponentRegistry& component_registry() const noexcept;
    [[nodiscard]] const SerializationRegistry& serialization_registry() const noexcept;
    [[nodiscard]] const PersistenceRegistry& persistence_registry() const noexcept;
    [[nodiscard]] const ReplicationRegistry& replication_registry() const noexcept;
    [[nodiscard]] const PresentationRegistry& presentation_registry() const noexcept;
    [[nodiscard]] DomainServiceRegistry& domain_services() noexcept;
    [[nodiscard]] const DomainServiceRegistry& domain_services() const noexcept;
    [[nodiscard]] movement::PlayerControllerRecord*
    player_for_client(core::NetId client_id) noexcept;
    [[nodiscard]] const movement::PlayerControllerRecord*
    player_for_client(core::NetId client_id) const noexcept;

  private:
    struct PlayerConnection {
        core::RuntimeHandle runtime_handle;
        entities::EntityId entity_id;
        movement::ServerMovementInputQueue pending_inputs;
        std::optional<movement::PlayerInputFrame> last_input;
        std::unique_ptr<movement::PhysicsCharacterCollisionWorld> physics_collision;
    };

    explicit ServerRuntime(ServerRuntimeDesc desc);
    [[nodiscard]] core::Status initialize();
    [[nodiscard]] core::Status initialize_new_world_scenario();
    [[nodiscard]] core::Status ensure_spawn_area();
    [[nodiscard]] core::Status grant_starting_inventory(core::SaveId owner_id);
    [[nodiscard]] world::WorldPosition scenario_spawn_position() const noexcept;
    [[nodiscard]] core::Status spawn_player(core::NetId client_id);
    [[nodiscard]] core::Status remove_player_connection(core::NetId client_id);
    void process_movement_control_messages(std::span<const net::TransportEnvelope> messages);
    [[nodiscard]] core::Status simulate_players(simulation::SimulationContext& context);
    [[nodiscard]] core::Status replicate_players();
    [[nodiscard]] core::Status replicate_entity_motion(std::uint64_t simulation_tick);
    [[nodiscard]] core::Status replicate_changed_chunks();
    [[nodiscard]] core::Status send_initial_chunks(core::NetId client_id);
    [[nodiscard]] core::Result<std::uint64_t> reserve_custom_replication_sequence();
    [[nodiscard]] bool admit_transient_snapshot(std::size_t payload_bytes) noexcept;
    [[nodiscard]] std::uint64_t collision_world_revision() const noexcept;

    ServerRuntimeDesc desc_;
    world::WorldState world_;
    entities::EntityWorld entities_;
    std::unique_ptr<physics::IPhysicsWorld> physics_;
    std::unique_ptr<physics::ChunkCollisionSystem> chunk_collision_;
    std::unique_ptr<physics::PhysicalResourcePhysicsSystem> physical_resource_physics_;
    std::unique_ptr<world::ChunkFluidSystem> chunk_fluids_;
    std::unique_ptr<world::ChunkLightSystem> chunk_lighting_;
    net::HostSession host_;
    net::ServerCommandDispatcher commands_;
    simulation::SimulationScheduler scheduler_;
    simulation::TickEvents events_;
    movement::PlayerController player_controller_;
    movement::PlayerControllerStore players_;
    ComponentRegistry component_registry_;
    SerializationRegistry serialization_registry_;
    PersistenceRegistry persistence_registry_;
    ReplicationRegistry replication_registry_;
    PresentationRegistry presentation_registry_;
    DomainServiceRegistry domain_services_;
    GameplayModuleRegistry gameplay_modules_;
    std::unordered_map<std::uint64_t, PlayerConnection> player_connections_;
    std::vector<world::VoxelEditRecord> pending_saved_voxel_edits_;
    std::vector<core::NetId> pending_player_removals_;
    std::vector<core::NetId> pending_entity_motion_removals_;
    net::HostSessionTickResult current_commands_;
    world::WorldReplicationDeltaDeliveryReport current_replication_;
    physics::PhysicsStepStats current_physics_;
    std::uint32_t current_moved_player_count_ = 0;
    std::uint32_t current_repeated_input_count_ = 0;
    std::uint32_t current_movement_event_count_ = 0;
    std::uint32_t current_accepted_movement_input_count_ = 0;
    std::uint32_t current_rejected_movement_input_count_ = 0;
    std::uint32_t current_movement_snapshot_count_ = 0;
    std::uint32_t current_entity_motion_snapshot_count_ = 0;
    std::uint32_t current_entity_motion_tombstone_count_ = 0;
    std::uint32_t current_player_tombstone_count_ = 0;
    std::uint32_t current_deferred_transient_snapshot_count_ = 0;
    std::uint32_t current_transient_snapshot_payload_bytes_ = 0;
    std::uint32_t current_transient_snapshot_message_count_ = 0;
    std::int64_t current_time_ms_ = 0;
    std::uint64_t pending_world_time_numerator_ = 0;
    std::uint64_t collision_world_revision_ = 1;
    bool spawn_area_initialized_ = false;
    std::uint64_t next_custom_replication_sequence_ = 1;
    std::uint64_t transient_replication_cursor_ = 0;
};

} // namespace heartstead::game
