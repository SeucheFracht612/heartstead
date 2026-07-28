#pragma once

#include "engine/assemblies/assembly.hpp"
#include "engine/assets/asset_catalog.hpp"
#include "engine/assets/asset_cooker.hpp"
#include "engine/assets/cooked_asset_store.hpp"
#include "engine/assets/resource_pack.hpp"
#include "engine/audio/audio_types.hpp"
#include "engine/audio/sound_event.hpp"
#include "engine/build/build_piece.hpp"
#include "engine/cargo/cargo.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/core/result.hpp"
#include "engine/dirty/dirty_region.hpp"
#include "engine/entities/entity.hpp"
#include "engine/entities/physical_resource.hpp"
#include "engine/items/item_stack.hpp"
#include "engine/modding/mod_lifecycle.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/net/client_session.hpp"
#include "engine/net/host_session.hpp"
#include "engine/net/transport.hpp"
#include "engine/net/transport_control.hpp"
#include "engine/networks/spatial_network.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/platform/platform.hpp"
#include "engine/processes/process.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/shaders/shader_compiler.hpp"
#include "engine/replay/command_replay.hpp"
#include "engine/rooms/room_graph.hpp"
#include "engine/save/save_database.hpp"
#include "engine/save/save_slot.hpp"
#include "engine/save/save_snapshot.hpp"
#include "engine/scripting/script_host_event.hpp"
#include "engine/scripting/script_runtime.hpp"
#include "engine/simulation/simulation_lod.hpp"
#include "engine/workpieces/workpiece_grid.hpp"
#include "engine/world/operations/world_operation.hpp"
#include "engine/world/replication_delta.hpp"
#include "engine/world/replication_interest.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::world {
class RegionGraph;
class WorldState;
} // namespace heartstead::world

namespace heartstead::debug {

enum class InspectionSeverity {
    info,
    warning,
    error,
};

struct InspectionField {
    std::string name;
    std::string value;
};

struct InspectionIssue {
    InspectionSeverity severity = InspectionSeverity::warning;
    std::string code;
    std::string message;
};

struct InspectionData {
    std::string object_type;
    std::string display_name;
    std::string prototype_id;
    std::string save_id;
    std::string runtime_id;
    std::string state;
    std::vector<InspectionField> fields;
    std::vector<InspectionIssue> issues;

    [[nodiscard]] bool has_errors() const noexcept;
    [[nodiscard]] const InspectionField* find_field(std::string_view name) const noexcept;
};

class Inspector {
  public:
    [[nodiscard]] static InspectionData
    inspect(const platform::PlatformBackendCapabilities& capabilities);
    [[nodiscard]] static InspectionData inspect(const platform::DisplayInfo& display);
    [[nodiscard]] static InspectionData
    inspect(const renderer::rhi::RenderBackendCapabilities& capabilities);
    [[nodiscard]] static InspectionData
    inspect(const physics::PhysicsBackendCapabilities& capabilities);
    [[nodiscard]] static InspectionData inspect(const assets::MountPoint& mount);
    [[nodiscard]] static InspectionData inspect(const assets::VirtualFileEntry& entry);
    [[nodiscard]] static InspectionData inspect(const assets::VirtualFileSystem& vfs);
    [[nodiscard]] static InspectionData inspect(const assets::AssetRecord& record);
    [[nodiscard]] static InspectionData inspect(const assets::AssetCatalog& catalog);
    [[nodiscard]] static InspectionData inspect(const audio::SoundEventDefinition& event);
    [[nodiscard]] static InspectionData inspect(const audio::SoundEventRegistry& registry);
    [[nodiscard]] static InspectionData inspect(const audio::AudioSystemStats& stats);
    [[nodiscard]] static InspectionData inspect(const assets::CookedAssetRecord& record);
    [[nodiscard]] static InspectionData inspect(const assets::CookedAssetManifest& manifest);
    [[nodiscard]] static InspectionData inspect(const assets::CookedAssetStore& store);
    [[nodiscard]] static InspectionData inspect(const assets::ResourcePackLoadPlan& plan);
    [[nodiscard]] static InspectionData inspect(const content::ContentValidationReport& report);
    [[nodiscard]] static InspectionData inspect(const assets::AssetCookBackendInfo& backend);
    [[nodiscard]] static InspectionData inspect(const assets::AssetCookPipelineInfo& pipeline);
    [[nodiscard]] static InspectionData
    inspect(const renderer::shaders::CompiledShaderRecord& record);
    [[nodiscard]] static InspectionData
    inspect(const renderer::shaders::ShaderCompileResult& result);
    [[nodiscard]] static InspectionData inspect(const modding::ModLifecyclePlan& plan);
    [[nodiscard]] static InspectionData inspect(const scripting::ScriptBackendInfo& backend);
    [[nodiscard]] static InspectionData inspect(const scripting::ScriptModuleDesc& module);
    [[nodiscard]] static InspectionData inspect(const scripting::ScriptRuntimeStats& stats);
    [[nodiscard]] static InspectionData inspect(const scripting::ScriptHostApiDesc& host_api);
    [[nodiscard]] static InspectionData inspect(const scripting::ScriptHostEvent& event);
    [[nodiscard]] static InspectionData inspect(const scripting::ScriptHostEventBatch& batch);
    [[nodiscard]] static InspectionData inspect(const simulation::SimulationLodPolicy& policy);
    [[nodiscard]] static InspectionData inspect(const simulation::SimulationSubject& subject);
    [[nodiscard]] static InspectionData inspect(const simulation::SimulationLodDecision& decision);
    [[nodiscard]] static InspectionData inspect(const simulation::SimulationFramePlan& plan);
    [[nodiscard]] static InspectionData inspect(const net::TransportBackendInfo& backend);
    [[nodiscard]] static InspectionData inspect(const net::TransportCapabilities& capabilities);
    [[nodiscard]] static InspectionData inspect(const net::TransportServerWelcome& welcome);
    [[nodiscard]] static InspectionData inspect(const net::TransportClientSession& session);
    [[nodiscard]] static InspectionData inspect(const net::CommandOperationTrace& trace);
    [[nodiscard]] static InspectionData inspect(const net::CommandDispatchResult& result);
    [[nodiscard]] static InspectionData inspect(const net::CommandDispatchReport& report);
    [[nodiscard]] static InspectionData inspect(const net::HostSessionCommandReport& report);
    [[nodiscard]] static InspectionData inspect(const net::HostSessionCommandResult& result);
    [[nodiscard]] static InspectionData inspect(const net::ReplicationBatch& batch);
    [[nodiscard]] static InspectionData inspect(const net::ReplicationRelevanceReport& report);
    [[nodiscard]] static InspectionData inspect(const net::ReplicationIntakeReport& report);
    [[nodiscard]] static InspectionData inspect(const net::HostSessionTickResult& result);
    [[nodiscard]] static InspectionData inspect(const net::ClientSession& session);
    [[nodiscard]] static InspectionData inspect(const replay::CommandReplayLog& log);
    [[nodiscard]] static InspectionData inspect(const replay::CommandReplayStep& step);
    [[nodiscard]] static InspectionData inspect(const replay::CommandReplayReport& report);

    [[nodiscard]] static InspectionData inspect(const items::ItemStack& stack);
    [[nodiscard]] static InspectionData inspect(const cargo::CargoRecord& cargo);
    [[nodiscard]] static InspectionData inspect(const entities::EntityRecord& entity);
    [[nodiscard]] static InspectionData inspect(const entities::PhysicalResourceRecord& resource);
    [[nodiscard]] static InspectionData inspect(const build::BuildPieceRecord& build_piece);
    [[nodiscard]] static InspectionData inspect(const assemblies::AssemblyRecord& assembly);
    [[nodiscard]] static InspectionData inspect(const processes::ProcessInstance& process);
    [[nodiscard]] static InspectionData inspect(const networks::SpatialNetwork& network);
    [[nodiscard]] static InspectionData inspect(const world::RegionGraph& graph);
    [[nodiscard]] static InspectionData inspect(const dirty::DirtyRegionTracker& tracker);
    [[nodiscard]] static InspectionData inspect(const rooms::RoomGraph& graph);
    [[nodiscard]] static InspectionData inspect(const rooms::RoomRecord& room);
    [[nodiscard]] static InspectionData inspect(const workpieces::WorkpieceGrid& grid);
    [[nodiscard]] static InspectionData inspect(const world::WorldReplicationDeltaPlan& plan);
    [[nodiscard]] static InspectionData
    inspect(const world::WorldReplicationDeltaSnapshot& snapshot);
    [[nodiscard]] static InspectionData
    inspect(const world::WorldReplicationDeltaTickReport& report);
    [[nodiscard]] static InspectionData
    inspect(const world::WorldReplicationDeltaDeliveryReport& report);
    [[nodiscard]] static InspectionData
    inspect(const world::WorldReplicationDeltaApplyReport& report);
    [[nodiscard]] static InspectionData
    inspect(const world::WorldClientReplicationApplyReport& report);
    [[nodiscard]] static InspectionData
    inspect(const world::WorldReplicationInterestReport& report);
    [[nodiscard]] static InspectionData inspect(const world::WorldOperation& operation);
    [[nodiscard]] static InspectionData inspect(const world::WorldState& state);
    [[nodiscard]] static InspectionData inspect(const save::SaveDatabaseStats& stats);
    [[nodiscard]] static InspectionData inspect(const save::SaveDatabaseMaintenanceResult& result);
    [[nodiscard]] static InspectionData inspect(const save::SaveDatabaseMigrationResult& result);
    [[nodiscard]] static InspectionData inspect(const save::SaveSlotCatalogSummary& catalog);
    [[nodiscard]] static InspectionData inspect(const save::SaveSlotSummary& slot);
    [[nodiscard]] static InspectionData
    inspect(const save::SaveSnapshot& snapshot,
            const modding::PrototypeRegistry* prototypes = nullptr);

    [[nodiscard]] static std::string render_text(const InspectionData& data);
};

} // namespace heartstead::debug
