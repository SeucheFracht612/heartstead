#pragma once

#include "engine/core/result.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/movement/player_input.hpp"
#include "engine/net/transport_client.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/save/save_database.hpp"
#include "engine/save/save_metadata.hpp"
#include "engine/save/save_snapshot.hpp"
#include "engine/simulation/fixed_step.hpp"
#include "engine/simulation/world_time.hpp"
#include "engine/world/coords/world_position.hpp"
#include "engine/world/fluids/chunk_fluid_system.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "game/features/interaction/voxel_commands.hpp"
#include "game/framework/gameplay_module.hpp"
#include "game/presentation/client_presentation.hpp"
#include "game/runtime/client_runtime.hpp"
#include "game/runtime/server_runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace heartstead::game {

struct RuntimeConfiguration {
    bool create_server = true;
    bool create_client = true;
    bool use_in_memory_transport = true;
    bool headless = true;
    simulation::FixedStepConfig fixed_step{};
    simulation::WorldTimeConfig world_time{};
    physics::PhysicsBackend physics_backend = physics::PhysicsBackend::headless;
    world::ChunkFluidSystemConfig chunk_fluids{};
    world::ChunkLightSystemConfig chunk_lighting{};
    world::ChunkLoadSchedulerConfig chunk_loading{};
    world::ChunkSubscriptionPolicy chunk_subscriptions{};
    std::uint64_t max_chunk_snapshot_serialization_time_us_per_tick = 4'000;
    std::uint32_t max_transient_snapshot_messages_per_tick = 512;
    std::uint32_t max_transient_snapshot_payload_bytes_per_tick = 256u * 1024u;
    std::uint64_t max_transient_snapshot_serialization_time_us_per_tick = 4'000;
    std::uint32_t max_transient_snapshot_messages_per_client_per_tick = 128;
    std::uint64_t max_transient_snapshot_payload_bytes_per_client_per_tick = 64u * 1024u;
    std::uint64_t max_transient_snapshot_serialization_time_us_per_client_per_tick = 2'000;
    std::uint32_t max_outbound_bytes_per_client_per_second = 256u * 1024u;
    std::uint32_t max_pending_reliable_messages = 8'192;
    std::uint64_t max_pending_reliable_bytes = 64u * 1024u * 1024u;
    std::uint32_t max_pending_reliable_messages_per_client = 1'024;
    std::uint64_t max_pending_reliable_bytes_per_client = 8u * 1024u * 1024u;
    std::uint32_t max_reliable_delivery_messages_per_tick = 512;
    std::uint64_t max_reliable_delivery_bytes_per_tick = 1u * 1024u * 1024u;
    std::uint32_t max_reliable_delivery_messages_per_client_per_tick = 128;
    std::uint64_t max_reliable_delivery_bytes_per_client_per_tick = 256u * 1024u;
    std::uint32_t simulated_network_one_way_latency_ms = 0;
    std::uint32_t simulated_network_jitter_ms = 0;
    std::uint32_t simulated_network_unreliable_loss_basis_points = 0;
    std::uint64_t simulated_network_seed = 0x6a09e667f3bcc909ULL;
    net::TransportEndpoint server_bind_endpoint{"0.0.0.0", 7777};
    std::optional<net::TransportEndpoint> remote_server_endpoint;
    std::vector<std::shared_ptr<IGameplayModule>> gameplay_modules;

    [[nodiscard]] core::Status validate() const;
};

enum class SessionMode {
    local_single_player,
    hosted_multiplayer,
    remote_multiplayer,
    dedicated_server,
    automated,
    replay,
};

enum class WorldSourceKind {
    generated,
    existing_save,
    developer_scenario,
    packaged_fixture,
    remote_server,
    automated_scenario,
    replay,
};

enum class PersistencePolicy {
    ephemeral,
    temporary_copy,
    persistent,
};

enum class SessionStartupPhase {
    validating_request,
    initializing_content,
    reading_world,
    restoring_world,
    preparing_world,
    initializing_physics,
    generating_spawn_area,
    registering_gameplay_systems,
    starting_authoritative_server,
    starting_client,
    connecting_transport,
    constructing_presentation,
    ready,
    cancelling,
};

using SessionStartupProgressCallback = std::function<void(SessionStartupPhase)>;

struct PlayerSpawnOverride {
    world::WorldPosition position;
    float yaw_degrees = 0.0F;
    float pitch_degrees = 0.0F;
};

// The one description used by menu, command-line, test, local, hosted, and remote launches.
// The metadata/snapshot fields are populated by the loading layer after validation and before the
// request is handed to RuntimeSession.
struct SessionLaunchRequest {
    std::uint64_t ownership_generation = 0;
    std::int64_t initial_runtime_time_ms = 0;
    SessionMode mode = SessionMode::local_single_player;
    WorldSourceKind world_source = WorldSourceKind::generated;
    PersistencePolicy persistence = PersistencePolicy::ephemeral;
    std::string world_name;
    std::string scenario_id = "base:scenarios/homestead";
    std::optional<std::filesystem::path> save_path;
    std::optional<std::uint64_t> seed;
    std::string generator_preset;
    std::vector<std::string> required_mods;
    std::vector<std::string> required_resource_packs;
    std::optional<net::TransportEndpoint> network_endpoint;
    std::optional<PlayerSpawnOverride> player_spawn;
    std::vector<std::string> initial_runtime_options;
    RuntimeConfiguration runtime;
    save::SaveMetadata metadata;
    std::optional<save::SaveSnapshot> initial_snapshot;

    [[nodiscard]] core::Status validate() const;
};

using SessionRequest = SessionLaunchRequest;

enum class RuntimeSessionState {
    created,
    starting,
    running,
    stopping,
    stopped,
    faulted,
};

enum class SessionConnectionState {
    none,
    connecting,
    connected,
    disconnecting,
    disconnected,
};

struct SessionTeardownReport {
    std::uint64_t ownership_generation = 0;
    std::uint32_t invocation_count = 0;
    std::size_t presentation_objects_before = 0;
    std::size_t presentation_objects_after = 0;
    std::size_t server_entities_before = 0;
    std::size_t server_entities_after = 0;
    std::size_t physics_bodies_before = 0;
    std::size_t physics_bodies_after = 0;
    std::size_t session_jobs_before = 0;
    std::size_t session_jobs_after = 0;
    std::size_t registered_cleanup_count = 0;
    std::size_t completed_cleanup_count = 0;
    bool rejected_new_commands = false;
    bool transport_stopped = false;
    bool authoritative_ticking_stopped = false;
    bool presentation_cleared = false;
    bool client_destroyed = false;
    bool server_destroyed = false;
};

struct SessionResourceCounts {
    std::size_t server_entities = 0;
    std::size_t physics_bodies = 0;
    std::size_t presentation_objects = 0;
    std::size_t registered_cleanup_callbacks = 0;
    std::size_t active_jobs = 0;
};

struct RuntimeFrameInput {
    std::uint64_t frame_time_us = 0;
    std::int64_t now_ms = 0;
};

struct RuntimeFrameStats {
    simulation::FixedStepFrame fixed_step;
    std::vector<ServerRuntimeTickStats> server_ticks;
    ClientRuntimeStats client;
    PresentationSynchronizationStats presentation;
    std::optional<core::Error> client_presentation_error;
    std::uint64_t authoritative_world_tick = 0;
};

class RuntimeSession final {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<RuntimeSession>>
    create(RuntimeConfiguration config, SessionRequest request,
           const modding::PrototypeRegistry& prototypes, const world::VoxelPalette& voxel_palette);
    [[nodiscard]] static core::Result<std::unique_ptr<RuntimeSession>>
    create(RuntimeConfiguration config, SessionRequest request,
           const modding::PrototypeRegistry& prototypes, const world::VoxelPalette& voxel_palette,
           SessionStartupProgressCallback progress, std::stop_token stop_token = {});

    RuntimeSession(const RuntimeSession&) = delete;
    RuntimeSession& operator=(const RuntimeSession&) = delete;
    ~RuntimeSession();

    [[nodiscard]] core::Result<RuntimeFrameStats> run_frame(RuntimeFrameInput input);
    [[nodiscard]] core::Status submit_command(std::string type, std::string payload,
                                              std::int64_t now_ms = 0);
    [[nodiscard]] core::Result<std::uint64_t>
    submit_tracked_command(std::string type, std::string payload, std::int64_t now_ms = 0);
    [[nodiscard]] core::Result<std::uint64_t>
    submit_inventory_transfer(const world::InventoryTransferRequest& request,
                              std::int64_t now_ms = 0);
    [[nodiscard]] core::Status submit_player_input(const movement::PlayerInputFrame& input,
                                                   std::int64_t now_ms = 0);
    [[nodiscard]] core::Status submit_place_voxel(const interaction::PlaceVoxelCommand& command,
                                                  std::int64_t now_ms = 0);
    [[nodiscard]] core::Status submit_remove_voxel(const interaction::RemoveVoxelCommand& command,
                                                   std::int64_t now_ms = 0);
    [[nodiscard]] core::Result<save::SaveSnapshot> capture_save_snapshot() const;
    [[nodiscard]] core::Status save_to(const save::FileSaveDatabase& database) const;
    [[nodiscard]] RenderSnapshot capture_render_snapshot() const;
    [[nodiscard]] core::Status request_stop();
    [[nodiscard]] core::Status register_cleanup(std::string name,
                                                std::function<core::Status()> cleanup);
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool accepts_commands() const noexcept;
    [[nodiscard]] std::uint64_t ownership_generation() const noexcept;
    [[nodiscard]] RuntimeSessionState state() const noexcept;
    [[nodiscard]] SessionConnectionState connection_state() const noexcept;
    [[nodiscard]] const SessionLaunchRequest& launch_request() const noexcept;
    [[nodiscard]] const SessionTeardownReport& teardown_report() const noexcept;
    [[nodiscard]] SessionResourceCounts resource_counts() const noexcept;
    [[nodiscard]] ServerRuntime* server() noexcept;
    [[nodiscard]] const ServerRuntime* server() const noexcept;
    [[nodiscard]] ClientRuntime* client() noexcept;
    [[nodiscard]] const ClientRuntime* client() const noexcept;
    [[nodiscard]] PresentationWorld* presentation() noexcept;
    [[nodiscard]] const PresentationWorld* presentation() const noexcept;
    [[nodiscard]] const RuntimeConfiguration& config() const noexcept;
    [[nodiscard]] std::uint64_t frame_count() const noexcept;
    [[nodiscard]] std::uint64_t fixed_step_tick() const noexcept;
    [[nodiscard]] const std::optional<RuntimeFrameStats>& last_frame_stats() const noexcept;
    [[nodiscard]] const std::optional<core::Error>& fault() const noexcept;

  private:
    RuntimeSession(RuntimeConfiguration config, SessionRequest request,
                   const modding::PrototypeRegistry& prototypes,
                   const world::VoxelPalette& voxel_palette);
    [[nodiscard]] core::Status initialize(const SessionStartupProgressCallback& progress,
                                          std::stop_token stop_token);
    [[nodiscard]] core::Result<RuntimeFrameStats> fault_frame(const core::Error& error);
    [[nodiscard]] core::Status pump_client_messages(std::int64_t now_ms);
    [[nodiscard]] core::Status synchronize_local_renderer_proof_chunks();
    [[nodiscard]] core::Result<PresentationSynchronizationStats> synchronize_presentation();

    struct CleanupEntry {
        std::string name;
        std::function<core::Status()> cleanup;
    };

    RuntimeConfiguration config_;
    SessionRequest request_;
    const modding::PrototypeRegistry* prototypes_ = nullptr;
    const world::VoxelPalette* voxel_palette_ = nullptr;
    simulation::FixedStepClock fixed_step_;
    std::unique_ptr<ServerRuntime> server_;
    std::unique_ptr<ClientRuntime> client_;
    std::unique_ptr<net::ITransportClient> remote_transport_;
    PresentationWorld presentation_;
    ClientPresentationSynchronizer presentation_synchronizer_;
    std::optional<RuntimeFrameStats> last_frame_stats_;
    std::optional<core::Error> fault_;
    std::vector<CleanupEntry> cleanup_entries_;
    SessionTeardownReport teardown_report_;
    std::uint64_t frame_count_ = 0;
    std::int64_t last_tick_time_ms_ = 0;
    RuntimeSessionState state_ = RuntimeSessionState::created;
    bool accepting_commands_ = false;
    bool local_renderer_proof_chunk_fast_path_ = false;
};

[[nodiscard]] std::string_view session_mode_name(SessionMode mode) noexcept;
[[nodiscard]] std::string_view world_source_kind_name(WorldSourceKind source) noexcept;
[[nodiscard]] std::string_view persistence_policy_name(PersistencePolicy policy) noexcept;
[[nodiscard]] std::string_view runtime_session_state_name(RuntimeSessionState state) noexcept;
[[nodiscard]] std::string_view session_connection_state_name(SessionConnectionState state) noexcept;
[[nodiscard]] std::string_view session_startup_phase_name(SessionStartupPhase phase) noexcept;
[[nodiscard]] bool session_mode_is_multiplayer(SessionMode mode) noexcept;

} // namespace heartstead::game
