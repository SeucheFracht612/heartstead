#pragma once

#include "engine/assemblies/assembly.hpp"
#include "engine/build/build_piece.hpp"
#include "engine/cargo/cargo.hpp"
#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/dirty/dirty_region.hpp"
#include "engine/entities/entity.hpp"
#include "engine/entities/physical_resource.hpp"
#include "engine/items/item_stack.hpp"
#include "engine/networks/network_derivation.hpp"
#include "engine/networks/spatial_network.hpp"
#include "engine/processes/process.hpp"
#include "engine/rooms/room_graph.hpp"
#include "engine/save/save_metadata.hpp"
#include "engine/simulation/fire.hpp"
#include "engine/simulation/world_time.hpp"
#include "engine/workpieces/workpiece_grid.hpp"
#include "engine/workpieces/workpiece_state.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/missing_prototype.hpp"
#include "engine/world/regions/region_graph.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heartstead::world {

struct WorkpieceRecord {
    core::WorkpieceId workpiece_id;
    core::PrototypeId prototype_id;
    workpieces::WorkpieceGrid grid;
    core::PrototypeId material_prototype_id;
    std::optional<workpieces::WorkpieceServerState> server_state;
    core::NetId owner_session;
    std::uint64_t revision = 1;
    bool committed = false;

    WorkpieceRecord(core::WorkpieceId id, core::PrototypeId prototype,
                    workpieces::WorkpieceGrid value)
        : workpiece_id(id), prototype_id(std::move(prototype)), grid(std::move(value)) {}
    WorkpieceRecord(core::WorkpieceId id, core::PrototypeId prototype,
                    workpieces::WorkpieceGrid value, core::PrototypeId material,
                    std::optional<workpieces::WorkpieceServerState> state, core::NetId owner,
                    std::uint64_t record_revision, bool is_committed)
        : workpiece_id(id), prototype_id(std::move(prototype)), grid(std::move(value)),
          material_prototype_id(std::move(material)), server_state(std::move(state)),
          owner_session(owner), revision(record_revision), committed(is_committed) {}

    [[nodiscard]] core::Status validate() const;
};

struct InventoryRecord {
    core::SaveId owner_id;
    std::vector<items::ItemStack> stacks;

    [[nodiscard]] core::Status validate() const;
};

struct InventoryTransferRequest {
    core::SaveId source_owner_id;
    core::SaveId destination_owner_id;
    std::size_t source_slot = 0;
    std::size_t destination_slot = 0;
    std::uint32_t count = 0;
};

// Provides the strong mutation guarantee: a failed transfer leaves both records unchanged.
// Self-transfer requires both references to name the same record. The same slot is a validated
// no-op; another slot merges, splits, or appends using pre-transfer slot indices.
[[nodiscard]] core::Status transfer_inventory_items(InventoryRecord& source,
                                                    InventoryRecord& destination,
                                                    const InventoryTransferRequest& request);

struct ModStateRecord {
    std::string mod_id;
    std::string state_key;
    std::string encoded_state;

    [[nodiscard]] core::Status validate() const;
};

class BuildObjectDatabase {
  public:
    [[nodiscard]] core::Status insert(build::BuildPieceRecord record);
    [[nodiscard]] build::BuildPieceRecord* find(core::SaveId id) noexcept;
    [[nodiscard]] const build::BuildPieceRecord* find(core::SaveId id) const noexcept;
    [[nodiscard]] bool contains(core::SaveId id) const noexcept;
    [[nodiscard]] std::vector<const build::BuildPieceRecord*> records() const;
    [[nodiscard]] std::size_t count() const noexcept;

  private:
    std::unordered_map<std::uint64_t, build::BuildPieceRecord> records_;
};

class EntityDatabase {
  public:
    [[nodiscard]] core::Status insert(entities::EntityRecord record);
    [[nodiscard]] bool erase(core::RuntimeHandle handle) noexcept;
    [[nodiscard]] entities::EntityRecord* find(core::RuntimeHandle handle) noexcept;
    [[nodiscard]] const entities::EntityRecord* find(core::RuntimeHandle handle) const noexcept;
    [[nodiscard]] entities::EntityRecord* find_by_save_id(core::SaveId id) noexcept;
    [[nodiscard]] const entities::EntityRecord* find_by_net_id(core::NetId id) const noexcept;
    [[nodiscard]] const entities::EntityRecord* find_by_save_id(core::SaveId id) const noexcept;
    [[nodiscard]] std::vector<const entities::EntityRecord*> records() const;
    [[nodiscard]] std::size_t count() const noexcept;
    [[nodiscard]] std::size_t persistent_count() const noexcept;

  private:
    std::unordered_map<std::uint64_t, entities::EntityRecord> records_by_runtime_;
    std::unordered_map<std::uint64_t, std::uint64_t> runtime_by_net_id_;
    std::unordered_map<std::uint64_t, std::uint64_t> runtime_by_save_id_;
};

class CargoDatabase {
  public:
    [[nodiscard]] core::Status insert(cargo::CargoRecord record);
    [[nodiscard]] cargo::CargoRecord* find(core::SaveId id) noexcept;
    [[nodiscard]] const cargo::CargoRecord* find(core::SaveId id) const noexcept;
    [[nodiscard]] std::vector<const cargo::CargoRecord*> records() const;
    [[nodiscard]] std::size_t count() const noexcept;

  private:
    std::unordered_map<std::uint64_t, cargo::CargoRecord> records_;
};

class InventoryDatabase {
  public:
    [[nodiscard]] core::Status insert(InventoryRecord record);
    [[nodiscard]] InventoryRecord* find(core::SaveId owner_id) noexcept;
    [[nodiscard]] const InventoryRecord* find(core::SaveId owner_id) const noexcept;
    [[nodiscard]] std::vector<const InventoryRecord*> records() const;
    [[nodiscard]] std::size_t count() const noexcept;

  private:
    std::unordered_map<std::uint64_t, InventoryRecord> records_by_owner_;
};

class WorkpieceDatabase {
  public:
    [[nodiscard]] core::Status insert(WorkpieceRecord record);
    [[nodiscard]] WorkpieceRecord* find(core::WorkpieceId id) noexcept;
    [[nodiscard]] const WorkpieceRecord* find(core::WorkpieceId id) const noexcept;
    [[nodiscard]] std::vector<const WorkpieceRecord*> records() const;
    [[nodiscard]] std::size_t count() const noexcept;

  private:
    std::unordered_map<std::uint64_t, WorkpieceRecord> records_;
};

class PhysicalResourceDatabase {
  public:
    [[nodiscard]] core::Status insert(entities::PhysicalResourceRecord record);
    [[nodiscard]] entities::PhysicalResourceRecord* find(core::SaveId id) noexcept;
    [[nodiscard]] const entities::PhysicalResourceRecord* find(core::SaveId id) const noexcept;
    [[nodiscard]] std::vector<const entities::PhysicalResourceRecord*> records() const;
    [[nodiscard]] std::size_t count() const noexcept;

  private:
    std::unordered_map<std::uint64_t, entities::PhysicalResourceRecord> records_;
};

class AssemblyDatabase {
  public:
    [[nodiscard]] core::Status insert(assemblies::AssemblyRecord record);
    [[nodiscard]] assemblies::AssemblyRecord* find(core::SaveId id) noexcept;
    [[nodiscard]] const assemblies::AssemblyRecord* find(core::SaveId id) const noexcept;
    [[nodiscard]] std::vector<const assemblies::AssemblyRecord*> records() const;
    [[nodiscard]] std::size_t count() const noexcept;

  private:
    std::unordered_map<std::uint64_t, assemblies::AssemblyRecord> records_;
};

using ProcessModifierResolver =
    std::function<core::Result<processes::ProcessModifiers>(const processes::ProcessInstance&)>;

class ProcessDatabase {
  public:
    [[nodiscard]] core::Status insert(processes::ProcessInstance instance);
    [[nodiscard]] processes::ProcessInstance* find(core::ProcessId id) noexcept;
    [[nodiscard]] const processes::ProcessInstance* find(core::ProcessId id) const noexcept;
    [[nodiscard]] std::vector<const processes::ProcessInstance*>
    find_by_owner(core::SaveId owner_id) const;
    [[nodiscard]] std::vector<const processes::ProcessInstance*> records() const;
    [[nodiscard]] std::span<const core::ProcessId> insertion_order() const noexcept;
    [[nodiscard]] core::Result<std::size_t> advance_all(simulation::WorldTick world_time,
                                                        processes::ProcessModifiers modifiers);
    [[nodiscard]] core::Result<std::size_t> advance_all(simulation::WorldTick world_time,
                                                        const ProcessModifierResolver& resolver);
    [[nodiscard]] core::Result<std::size_t>
    evaluate_owner(core::SaveId owner_id, simulation::WorldTick world_time,
                   processes::ProcessEvaluationTrigger trigger,
                   const ProcessModifierResolver& resolver);
    [[nodiscard]] std::size_t count() const noexcept;

  private:
    std::unordered_map<std::uint64_t, processes::ProcessInstance> records_;
    std::vector<core::ProcessId> insertion_order_;
};

class NetworkDatabase {
  public:
    [[nodiscard]] networks::SpatialNetwork& get_or_create(networks::NetworkKind kind);
    [[nodiscard]] networks::SpatialNetwork* find(networks::NetworkKind kind) noexcept;
    [[nodiscard]] const networks::SpatialNetwork* find(networks::NetworkKind kind) const noexcept;
    [[nodiscard]] std::vector<const networks::SpatialNetwork*> records() const;
    [[nodiscard]] core::Result<networks::SpatialNetworkDerivationStats>
    rebuild_from_ports(const BuildObjectDatabase& build_objects,
                       const AssemblyDatabase& assemblies);
    [[nodiscard]] std::size_t count() const noexcept;

  private:
    std::unordered_map<std::uint64_t, networks::SpatialNetwork> networks_;
};

class ModStateDatabase {
  public:
    [[nodiscard]] core::Status insert(ModStateRecord record);
    [[nodiscard]] const ModStateRecord* find(std::string_view mod_id,
                                             std::string_view state_key) const;
    [[nodiscard]] std::vector<const ModStateRecord*> records() const;
    [[nodiscard]] std::size_t count() const noexcept;

  private:
    std::unordered_map<std::string, ModStateRecord> records_;
};

class FireDatabase {
  public:
    [[nodiscard]] core::Status insert(simulation::FireInstance fire);
    [[nodiscard]] simulation::FireInstance* find(core::SaveId id) noexcept;
    [[nodiscard]] const simulation::FireInstance* find(core::SaveId id) const noexcept;
    [[nodiscard]] std::vector<const simulation::FireInstance*> records() const;
    [[nodiscard]] std::size_t count() const noexcept;

  private:
    std::unordered_map<std::uint64_t, simulation::FireInstance> records_;
};

struct WorldStateDesc {
    save::SaveMetadata metadata;
    VoxelPaletteManifest voxel_palette{};
    std::uint64_t next_save_id = 1;
    std::uint64_t next_runtime_handle = 1;
    std::uint64_t next_entity_net_id = 1'000'000;
    std::uint64_t next_process_id = 1;
};

struct WorldStateStats {
    std::size_t chunk_count = 0;
    std::size_t region_count = 0;
    std::size_t region_connection_count = 0;
    std::size_t dirty_region_count = 0;
    std::size_t build_object_count = 0;
    std::size_t entity_count = 0;
    std::size_t persistent_entity_count = 0;
    std::size_t cargo_count = 0;
    std::size_t inventory_count = 0;
    std::size_t workpiece_count = 0;
    std::size_t physical_resource_count = 0;
    std::size_t assembly_count = 0;
    std::size_t process_count = 0;
    std::size_t room_count = 0;
    std::size_t network_count = 0;
    std::size_t mod_state_count = 0;
    std::size_t missing_prototype_count = 0;
    std::size_t fire_count = 0;
};

class WorldState {
  public:
    explicit WorldState(WorldStateDesc desc = {});

    [[nodiscard]] const save::SaveMetadata& metadata() const noexcept;
    [[nodiscard]] const VoxelPaletteManifest& voxel_palette_manifest() const noexcept;
    [[nodiscard]] simulation::WorldTick world_time() const noexcept;
    [[nodiscard]] core::Status advance_world_time(simulation::WorldTick delta) noexcept;
    [[nodiscard]] core::Status
    advance_world_time_hours(std::uint64_t hours,
                             const simulation::WorldTimeConfig& config = {}) noexcept;
    [[nodiscard]] save::SaveIdAllocator& save_ids() noexcept;
    [[nodiscard]] const save::SaveIdAllocator& save_ids() const noexcept;
    [[nodiscard]] entities::RuntimeHandleAllocator& runtime_handles() noexcept;
    [[nodiscard]] const entities::RuntimeHandleAllocator& runtime_handles() const noexcept;
    [[nodiscard]] entities::EntityNetIdAllocator& entity_net_ids() noexcept;
    [[nodiscard]] const entities::EntityNetIdAllocator& entity_net_ids() const noexcept;
    [[nodiscard]] processes::ProcessIdAllocator& process_ids() noexcept;
    [[nodiscard]] const processes::ProcessIdAllocator& process_ids() const noexcept;
    [[nodiscard]] dirty::DirtyRegionTracker& dirty_regions() noexcept;
    [[nodiscard]] const dirty::DirtyRegionTracker& dirty_regions() const noexcept;
    [[nodiscard]] RegionGraph& regions() noexcept;
    [[nodiscard]] const RegionGraph& regions() const noexcept;
    [[nodiscard]] ChunkDatabase& chunks() noexcept;
    [[nodiscard]] const ChunkDatabase& chunks() const noexcept;
    [[nodiscard]] BuildObjectDatabase& build_objects() noexcept;
    [[nodiscard]] const BuildObjectDatabase& build_objects() const noexcept;
    [[nodiscard]] EntityDatabase& entities() noexcept;
    [[nodiscard]] const EntityDatabase& entities() const noexcept;
    [[nodiscard]] CargoDatabase& cargo() noexcept;
    [[nodiscard]] const CargoDatabase& cargo() const noexcept;
    [[nodiscard]] InventoryDatabase& inventories() noexcept;
    [[nodiscard]] const InventoryDatabase& inventories() const noexcept;
    [[nodiscard]] WorkpieceDatabase& workpieces() noexcept;
    [[nodiscard]] const WorkpieceDatabase& workpieces() const noexcept;
    [[nodiscard]] PhysicalResourceDatabase& physical_resources() noexcept;
    [[nodiscard]] const PhysicalResourceDatabase& physical_resources() const noexcept;
    [[nodiscard]] AssemblyDatabase& assemblies() noexcept;
    [[nodiscard]] const AssemblyDatabase& assemblies() const noexcept;
    [[nodiscard]] ProcessDatabase& processes() noexcept;
    [[nodiscard]] const ProcessDatabase& processes() const noexcept;
    [[nodiscard]] rooms::RoomGraph& rooms() noexcept;
    [[nodiscard]] const rooms::RoomGraph& rooms() const noexcept;
    [[nodiscard]] NetworkDatabase& networks() noexcept;
    [[nodiscard]] const NetworkDatabase& networks() const noexcept;
    [[nodiscard]] ModStateDatabase& mod_states() noexcept;
    [[nodiscard]] const ModStateDatabase& mod_states() const noexcept;
    [[nodiscard]] std::vector<MissingPrototypeObject>& missing_prototypes() noexcept;
    [[nodiscard]] const std::vector<MissingPrototypeObject>& missing_prototypes() const noexcept;
    [[nodiscard]] FireDatabase& fires() noexcept;
    [[nodiscard]] const FireDatabase& fires() const noexcept;

    [[nodiscard]] bool contains_saved_object(core::SaveId id) const noexcept;
    [[nodiscard]] WorldStateStats stats() const noexcept;

  private:
    save::SaveMetadata metadata_;
    VoxelPaletteManifest voxel_palette_manifest_;
    save::SaveIdAllocator save_ids_;
    entities::RuntimeHandleAllocator runtime_handles_;
    entities::EntityNetIdAllocator entity_net_ids_;
    processes::ProcessIdAllocator process_ids_;
    dirty::DirtyRegionTracker dirty_regions_;
    RegionGraph regions_;
    ChunkDatabase chunks_;
    BuildObjectDatabase build_objects_;
    EntityDatabase entities_;
    CargoDatabase cargo_;
    InventoryDatabase inventories_;
    WorkpieceDatabase workpieces_;
    PhysicalResourceDatabase physical_resources_;
    AssemblyDatabase assemblies_;
    ProcessDatabase processes_;
    rooms::RoomGraph rooms_;
    NetworkDatabase networks_;
    ModStateDatabase mod_states_;
    std::vector<MissingPrototypeObject> missing_prototypes_;
    FireDatabase fires_;
};

} // namespace heartstead::world
