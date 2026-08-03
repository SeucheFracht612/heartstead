#pragma once

#include "engine/entities/entity_motion_snapshot.hpp"
#include "engine/entities/entity_world.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/movement/movement_prediction.hpp"
#include "engine/movement/physics_character_collision.hpp"
#include "engine/movement/player_controller_store.hpp"
#include "engine/net/host_session.hpp"
#include "engine/net/replication_budget.hpp"
#include "engine/physics/chunk_collision_system.hpp"
#include "engine/physics/physical_resource_physics_system.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/processes/process_temporal_aggregation.hpp"
#include "engine/save/save_snapshot.hpp"
#include "engine/scenarios/scenario.hpp"
#include "engine/scenarios/scenario_fixture.hpp"
#include "engine/simulation/simulation_scheduler.hpp"
#include "engine/simulation/world_time.hpp"
#include "engine/world/chunks/chunk_subscription.hpp"
#include "engine/world/fluids/chunk_fluid_system.hpp"
#include "engine/world/lighting/chunk_light_system.hpp"
#include "engine/world/replication_delta.hpp"
#include "engine/world/streaming/predictive_chunk_streaming_controller.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "engine/world/world_state.hpp"
#include "game/framework/gameplay_module.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace heartstead::game {

enum class ServerRuntimeStartupPhase {
    restoring_world,
    initializing_physics,
    generating_spawn_area,
    registering_gameplay_systems,
};

using ServerRuntimeStartupProgressCallback = std::function<void(ServerRuntimeStartupPhase phase)>;

struct ServerRuntimeDesc {
    world::WorldStateDesc world;
    net::HostSessionConfig host;
    physics::PhysicsWorldDesc physics;
    physics::ChunkCollisionSystemConfig chunk_collision;
    physics::PhysicalResourcePhysicsSystemConfig physical_resources;
    world::ChunkFluidSystemConfig chunk_fluids;
    world::ChunkLightSystemConfig chunk_lighting;
    world::ChunkLoadSchedulerConfig chunk_loading;
    world::PredictiveChunkStreamingPolicy chunk_streaming;
    world::ChunkSubscriptionPolicy chunk_subscriptions;
    std::uint64_t max_chunk_snapshot_serialization_time_us_per_tick = 4'000;
    std::uint32_t simulation_ticks_per_second = 60;
    net::ReplicationTickBudgetConfig transient_replication_budget;
    bool direct_local_chunk_replication = false;
    simulation::WorldTimeConfig world_time;
    processes::ProcessTemporalAggregationConfig process_temporal_aggregation;
    const modding::PrototypeRegistry* prototypes = nullptr;
    const world::VoxelPalette* voxel_palette = nullptr;
    scenarios::ScenarioDefinition scenario;
    std::optional<save::SaveSnapshot> initial_snapshot;
    std::vector<std::shared_ptr<IGameplayModule>> gameplay_modules;
    std::stop_token stop_token;
    ServerRuntimeStartupProgressCallback startup_progress;
};

struct ServerChunkSubscriptionTickStats {
    std::uint32_t client_count = 0;
    std::size_t subscription_count = 0;
    std::size_t maximum_client_subscription_count = 0;
    std::size_t published_chunk_count = 0;
    std::size_t partial_snapshot_count = 0;
    std::size_t stale_publication_count = 0;
    std::uint32_t converged_client_count = 0;
    std::uint32_t pending_initial_state_client_count = 0;
    std::uint32_t published_initial_state_count = 0;
    std::uint32_t added_subscription_count = 0;
    std::uint32_t removed_subscription_count = 0;
    std::uint32_t removal_message_count = 0;
    std::uint32_t snapshot_chunk_count = 0;
    std::uint32_t snapshot_slice_message_count = 0;
    std::uint32_t snapshot_serialization_operation_count = 0;
    std::uint64_t snapshot_payload_bytes = 0;
    std::uint64_t snapshot_serialization_time_us = 0;
    std::uint64_t snapshot_serialization_time_overshoot_us = 0;
    std::uint64_t deferred_addition_count = 0;
    std::uint64_t capacity_deferred_addition_count = 0;
    std::uint64_t deferred_removal_count = 0;
    std::uint64_t deferred_snapshot_count = 0;
    std::uint64_t serialization_budget_deferred_snapshot_count = 0;
    std::uint32_t reliable_admission_deferral_count = 0;
    std::uint32_t delta_advanced_publication_count = 0;
    std::uint32_t delta_avoided_snapshot_count = 0;
    std::uint32_t delta_publication_gap_count = 0;
};

struct ServerChunkStreamingTickStats {
    bool enabled = false;
    world::PredictiveChunkStreamingStats lifetime;
    std::size_t desired_chunk_count = 0;
    std::size_t target_resident_chunk_count = 0;
    std::size_t submitted_required_count = 0;
    std::size_t submitted_speculative_count = 0;
    std::size_t explicit_speculative_cancellation_count = 0;
    std::size_t obsolete_cancellation_signal_count = 0;
    std::size_t deferred_required_load_count = 0;
    std::size_t evicted_chunk_count = 0;
    std::size_t deferred_eviction_count = 0;
    std::size_t projected_resident_overage = 0;
    std::size_t unresolved_resident_overage = 0;
    std::size_t pending_load_count = 0;
    bool teleport_mode = false;
};

struct ServerChunkSubscriptionClientSnapshot {
    core::NetId client_id;
    world::ChunkCoord center;
    std::vector<world::ChunkCoord> subscriptions;
    std::size_t published_chunk_count = 0;
    std::size_t partial_snapshot_count = 0;
    std::size_t stale_publication_count = 0;
    std::size_t deferred_addition_count = 0;
    std::size_t capacity_deferred_addition_count = 0;
    std::size_t deferred_removal_count = 0;
    std::size_t deferred_snapshot_count = 0;
    bool converged = false;
    bool initial_state_published = false;
};

struct ServerRuntimeTickStats {
    std::uint64_t wall_time_us = 0;
    simulation::SimulationTickStats simulation;
    net::HostSessionTickResult commands;
    world::WorldReplicationDeltaDeliveryReport replication;
    physics::PhysicsStepStats physics;
    physics::ChunkCollisionSystemStats chunk_collision;
    physics::PhysicalResourcePhysicsSystemStats physical_resources;
    world::ChunkFluidSystemStats chunk_fluids;
    world::ChunkLightSystemStats chunk_lighting;
    world::ChunkLoadSchedulerStats chunk_loading;
    ServerChunkStreamingTickStats chunk_streaming;
    processes::ProcessTemporalAggregationTickStats process_temporal_aggregation;
    std::uint32_t moved_player_count = 0;
    std::uint32_t repeated_input_count = 0;
    std::uint32_t movement_event_count = 0;
    std::uint32_t accepted_movement_input_count = 0;
    std::uint32_t rejected_movement_input_count = 0;
    std::uint32_t movement_snapshot_count = 0;
    std::uint32_t entity_motion_snapshot_count = 0;
    std::uint32_t entity_motion_tombstone_count = 0;
    std::uint32_t player_tombstone_count = 0;
    std::uint64_t deferred_transient_snapshot_count = 0;
    std::uint64_t transient_snapshot_payload_bytes = 0;
    net::ReplicationTickBudgetStats transient_replication;
    ServerChunkSubscriptionTickStats chunk_subscriptions;
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
    [[nodiscard]] const world::VoxelPalette& voxel_palette() const noexcept;
    [[nodiscard]] std::uint32_t physics_body_count() const noexcept;
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
    [[nodiscard]] const world::ChunkLoadSchedulerStats* chunk_loading_stats() const noexcept;
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
    [[nodiscard]] std::vector<ServerChunkSubscriptionClientSnapshot>
    chunk_subscription_clients() const;

  private:
    struct ChunkPublication {
        world::ChunkIdentity identity;
        std::uint64_t content_revision = 0;
        bool complete = false;
    };

    struct EncodedChunkSnapshot {
        world::ChunkIdentity identity;
        std::uint64_t content_revision = 0;
        std::vector<std::string> slices;
    };

    struct PlayerConnection {
        core::RuntimeHandle runtime_handle;
        entities::EntityId entity_id;
        movement::ServerMovementInputQueue pending_inputs;
        std::optional<movement::PlayerInputFrame> last_input;
        std::unique_ptr<movement::PhysicsCharacterCollisionWorld> physics_collision;
        world::ChunkCoord chunk_subscription_center;
        std::vector<world::ChunkCoord> chunk_subscriptions;
        std::map<world::ChunkCoord, ChunkPublication> chunk_publications;
        std::size_t chunk_snapshot_cursor = 0;
        std::size_t deferred_chunk_additions = 0;
        std::size_t capacity_deferred_chunk_additions = 0;
        std::size_t deferred_chunk_removals = 0;
        std::size_t deferred_chunk_snapshots = 0;
        bool chunk_subscriptions_converged = false;
        bool initial_state_published = false;
    };

    explicit ServerRuntime(ServerRuntimeDesc desc);
    [[nodiscard]] core::Status initialize();
    [[nodiscard]] core::Status initialize_new_world_scenario();
    [[nodiscard]] core::Status ensure_spawn_area();
    [[nodiscard]] core::Status grant_starting_inventory(core::SaveId owner_id);
    [[nodiscard]] core::Status validate_voxel_placement(world::BlockCoord position,
                                                        world::VoxelCell cell) const;
    [[nodiscard]] world::WorldPosition scenario_spawn_position() const noexcept;
    [[nodiscard]] core::Status spawn_player(core::NetId client_id);
    [[nodiscard]] core::Status remove_player_connection(core::NetId client_id);
    void process_movement_control_messages(std::span<const net::TransportEnvelope> messages);
    [[nodiscard]] core::Status simulate_players(simulation::SimulationContext& context);
    [[nodiscard]] core::Status stream_chunks();
    [[nodiscard]] core::Status replicate_players();
    [[nodiscard]] core::Status replicate_entity_motion(std::uint64_t simulation_tick);
    [[nodiscard]] core::Status advance_process_temporal_aggregation();
    [[nodiscard]] core::Status publish_process_temporal_replication();
    [[nodiscard]] core::Status synchronize_chunk_subscriptions();
    [[nodiscard]] core::Status advance_chunk_publications_from_voxel_deltas();
    [[nodiscard]] core::Status synchronize_client_chunk_subscription(
        core::NetId client_id, world::ChunkSubscriptionTransitionBudget transition_budget,
        std::map<world::ChunkCoord, EncodedChunkSnapshot>& snapshot_cache,
        bool enforce_serialization_budget = true);
    void refresh_replication_chunk_interest();
    [[nodiscard]] bool initial_chunk_state_ready(const PlayerConnection& connection) const noexcept;
    [[nodiscard]] core::Status send_initial_state(core::NetId client_id);
    [[nodiscard]] core::Result<std::uint64_t> reserve_custom_replication_sequence();
    [[nodiscard]] std::uint64_t collision_world_revision() const noexcept;

    ServerRuntimeDesc desc_;
    net::ReplicationTickBudget transient_replication_budget_;
    world::WorldState world_;
    processes::ProcessTemporalAggregationController process_temporal_aggregation_;
    entities::EntityWorld entities_;
    std::unique_ptr<physics::IPhysicsWorld> physics_;
    std::unique_ptr<physics::ChunkCollisionSystem> chunk_collision_;
    std::unique_ptr<physics::PhysicalResourcePhysicsSystem> physical_resource_physics_;
    std::unique_ptr<world::ChunkFluidSystem> chunk_fluids_;
    std::unique_ptr<world::ChunkLightSystem> chunk_lighting_;
    std::unique_ptr<world::ChunkLoadScheduler> chunk_loader_;
    world::PredictiveChunkStreamingController chunk_streaming_controller_;
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
    std::vector<world::ChunkCoord> pending_streamed_chunks_;
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
    ServerChunkSubscriptionTickStats current_chunk_subscriptions_;
    ServerChunkStreamingTickStats current_chunk_streaming_;
    processes::ProcessTemporalAggregationTickStats current_process_temporal_aggregation_;
    std::map<std::uint64_t, world::ChunkCoord> previous_chunk_streaming_viewers_;
    std::int64_t current_time_ms_ = 0;
    std::uint64_t pending_world_time_numerator_ = 0;
    std::uint64_t collision_world_revision_ = 1;
    bool spawn_area_initialized_ = false;
    std::uint64_t next_custom_replication_sequence_ = 1;
    std::uint64_t transient_replication_recipient_cursor_ = 0;
    std::uint64_t movement_replication_source_cursor_ = 0;
    std::uint64_t entity_motion_replication_source_cursor_ = 0;
    std::uint64_t transient_replication_class_cursor_ = 0;
    std::uint64_t chunk_subscription_client_cursor_ = 0;
    bool renderer_proof_streaming_enabled_ = false;
    bool process_temporal_reset_pending_ = false;
};

} // namespace heartstead::game
