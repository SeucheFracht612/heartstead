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
#include "engine/world/fluids/chunk_fluid_system.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "game/features/interaction/voxel_commands.hpp"
#include "game/framework/gameplay_module.hpp"
#include "game/presentation/client_presentation.hpp"
#include "game/runtime/client_runtime.hpp"
#include "game/runtime/server_runtime.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace heartstead::game {

struct RuntimeConfiguration {
    bool create_server = true;
    bool create_client = true;
    bool create_renderer = false;
    bool create_audio = false;
    bool use_in_memory_transport = true;
    bool headless = true;
    simulation::FixedStepConfig fixed_step{};
    simulation::WorldTimeConfig world_time{};
    physics::PhysicsBackend physics_backend = physics::PhysicsBackend::headless;
    world::ChunkFluidSystemConfig chunk_fluids{};
    world::ChunkLightSystemConfig chunk_lighting{};
    std::uint32_t max_transient_snapshot_messages_per_tick = 512;
    std::uint32_t max_transient_snapshot_payload_bytes_per_tick = 256u * 1024u;
    std::uint32_t max_outbound_bytes_per_client_per_second = 256u * 1024u;
    std::uint32_t simulated_network_one_way_latency_ms = 0;
    std::uint32_t simulated_network_jitter_ms = 0;
    std::uint32_t simulated_network_unreliable_loss_basis_points = 0;
    std::uint64_t simulated_network_seed = 0x6a09e667f3bcc909ULL;
    net::TransportEndpoint server_bind_endpoint{"0.0.0.0", 7777};
    std::optional<net::TransportEndpoint> remote_server_endpoint;
    std::vector<std::shared_ptr<IGameplayModule>> gameplay_modules;

    [[nodiscard]] core::Status validate() const;
};

struct SessionRequest {
    save::SaveMetadata metadata;
    std::string scenario_id = "base:scenarios/homestead";
    std::optional<save::SaveSnapshot> initial_snapshot;
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
    std::uint64_t authoritative_world_tick = 0;
};

class RuntimeSession final {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<RuntimeSession>>
    create(RuntimeConfiguration config, SessionRequest request,
           const modding::PrototypeRegistry& prototypes, const world::VoxelPalette& voxel_palette);

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
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] ServerRuntime* server() noexcept;
    [[nodiscard]] const ServerRuntime* server() const noexcept;
    [[nodiscard]] ClientRuntime* client() noexcept;
    [[nodiscard]] const ClientRuntime* client() const noexcept;
    [[nodiscard]] PresentationWorld* presentation() noexcept;
    [[nodiscard]] const PresentationWorld* presentation() const noexcept;
    [[nodiscard]] const RuntimeConfiguration& config() const noexcept;
    [[nodiscard]] std::uint64_t frame_count() const noexcept;
    [[nodiscard]] const std::optional<RuntimeFrameStats>& last_frame_stats() const noexcept;
    [[nodiscard]] const std::optional<core::Error>& fault() const noexcept;

  private:
    RuntimeSession(RuntimeConfiguration config, SessionRequest request,
                   const modding::PrototypeRegistry& prototypes,
                   const world::VoxelPalette& voxel_palette);
    [[nodiscard]] core::Status initialize();
    [[nodiscard]] core::Result<RuntimeFrameStats> fault_frame(const core::Error& error);
    [[nodiscard]] core::Status pump_client_messages(std::int64_t now_ms);
    [[nodiscard]] core::Result<PresentationSynchronizationStats> synchronize_presentation();

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
    std::uint64_t frame_count_ = 0;
    bool running_ = false;
};

} // namespace heartstead::game
