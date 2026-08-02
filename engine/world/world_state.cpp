#include "engine/world/world_state.hpp"

#include <utility>

namespace heartstead::world {

namespace {

[[nodiscard]] std::uint64_t key(core::SaveId id) noexcept {
    return id.value();
}

[[nodiscard]] std::uint64_t key(core::RuntimeHandle id) noexcept {
    return id.value();
}

[[nodiscard]] std::uint64_t key(core::NetId id) noexcept {
    return id.value();
}

[[nodiscard]] std::uint64_t key(core::WorkpieceId id) noexcept {
    return id.value();
}

[[nodiscard]] std::uint64_t key(core::ProcessId id) noexcept {
    return id.value();
}

[[nodiscard]] std::uint64_t key(networks::NetworkKind kind) noexcept {
    return static_cast<std::uint64_t>(kind);
}

[[nodiscard]] std::string mod_state_key(std::string_view mod_id, std::string_view state_key) {
    std::string result(mod_id);
    result.push_back(':');
    result.append(state_key);
    return result;
}

} // namespace

core::Status WorkpieceRecord::validate() const {
    if (!workpiece_id.is_valid()) {
        return core::Status::failure("world_state.invalid_workpiece_id",
                                     "workpiece needs a stable workpiece id");
    }
    if (!prototype_id.is_valid()) {
        return core::Status::failure("world_state.invalid_workpiece_prototype",
                                     "workpiece prototype id must be valid");
    }
    const auto shape = grid.shape();
    if (shape.width == 0 || shape.height == 0 || shape.depth == 0) {
        return core::Status::failure("world_state.invalid_workpiece_shape",
                                     "workpiece grid shape must be non-zero");
    }
    if (revision == 0) {
        return core::Status::failure("world_state.invalid_workpiece_revision",
                                     "workpiece revision must be non-zero");
    }
    if (server_state.has_value()) {
        if (!material_prototype_id.is_valid()) {
            return core::Status::failure(
                "world_state.invalid_workpiece_material",
                "server-owned workpiece state requires a material prototype");
        }
        auto status = server_state->validate(shape);
        if (!status)
            return status;
    }
    return core::Status::ok();
}

core::Status InventoryRecord::validate() const {
    if (!owner_id.is_valid()) {
        return core::Status::failure("world_state.invalid_inventory_owner",
                                     "inventory owner save id must be valid");
    }
    for (const auto& stack : stacks) {
        if (!stack.prototype_id.is_valid()) {
            return core::Status::failure("world_state.invalid_inventory_stack_prototype",
                                         "inventory stack prototype id must be valid");
        }
        if (stack.max_count == 0 || stack.is_empty() || stack.count > stack.max_count) {
            return core::Status::failure("world_state.invalid_inventory_stack_count",
                                         "inventory stack count must be between 1 and max count");
        }
    }
    return core::Status::ok();
}

core::Status transfer_inventory_items(InventoryRecord& source, InventoryRecord& destination,
                                      const InventoryTransferRequest& request) {
    if (source.owner_id != request.source_owner_id) {
        return core::Status::failure("inventory_transfer.source_owner_mismatch",
                                     "source inventory owner does not match transfer request");
    }
    if (destination.owner_id != request.destination_owner_id) {
        return core::Status::failure("inventory_transfer.destination_owner_mismatch",
                                     "destination inventory owner does not match transfer request");
    }
    const bool same_inventory = source.owner_id == destination.owner_id;
    if (same_inventory && &source != &destination) {
        return core::Status::failure(
            "inventory_transfer.owner_alias_mismatch",
            "one inventory owner must resolve to the same inventory record for self-transfer");
    }
    if (request.count == 0) {
        return core::Status::failure("inventory_transfer.invalid_count",
                                     "inventory transfer count must be non-zero");
    }
    if (request.source_slot >= source.stacks.size()) {
        return core::Status::failure("inventory_transfer.source_slot_out_of_range",
                                     "source inventory slot is out of range");
    }
    const auto& destination_stacks = same_inventory ? source.stacks : destination.stacks;
    if (request.destination_slot > destination_stacks.size()) {
        return core::Status::failure("inventory_transfer.destination_slot_out_of_range",
                                     "destination inventory slot is out of range");
    }

    const auto& source_stack = source.stacks[request.source_slot];
    if (request.count > source_stack.count) {
        return core::Status::failure("inventory_transfer.insufficient_items",
                                     "source stack does not contain the requested item count");
    }

    if (same_inventory && request.source_slot == request.destination_slot) {
        return core::Status::ok();
    }

    if (request.destination_slot < destination_stacks.size()) {
        const auto& destination_stack = destination_stacks[request.destination_slot];
        if (!destination_stack.can_merge_with(source_stack)) {
            return core::Status::failure(
                "inventory_transfer.merge_mismatch",
                "destination stack cannot merge the requested source stack");
        }
        if (destination_stack.remaining_capacity() < request.count) {
            return core::Status::failure("inventory_transfer.destination_full",
                                         "destination stack does not have enough capacity");
        }
    }

    if (same_inventory) {
        auto staged = source.stacks;
        if (request.destination_slot < staged.size()) {
            staged[request.source_slot].count -= request.count;
            staged[request.destination_slot].count += request.count;
        } else {
            auto moved_stack = staged[request.source_slot];
            moved_stack.count = request.count;
            staged[request.source_slot].count -= request.count;
            staged.push_back(std::move(moved_stack));
        }
        if (staged[request.source_slot].count == 0) {
            staged.erase(
                staged.begin() +
                static_cast<std::vector<items::ItemStack>::difference_type>(request.source_slot));
        }
        source.stacks.swap(staged);
        return core::Status::ok();
    }

    auto staged_source = source.stacks;
    auto staged_destination = destination.stacks;
    if (request.destination_slot < staged_destination.size()) {
        staged_source[request.source_slot].count -= request.count;
        staged_destination[request.destination_slot].count += request.count;
    } else {
        auto moved_stack = staged_source[request.source_slot];
        moved_stack.count = request.count;
        staged_source[request.source_slot].count -= request.count;
        staged_destination.push_back(std::move(moved_stack));
    }
    if (staged_source[request.source_slot].count == 0) {
        staged_source.erase(
            staged_source.begin() +
            static_cast<std::vector<items::ItemStack>::difference_type>(request.source_slot));
    }
    source.stacks.swap(staged_source);
    destination.stacks.swap(staged_destination);
    return core::Status::ok();
}

core::Status ModStateRecord::validate() const {
    if (!core::is_valid_namespace_id(mod_id)) {
        return core::Status::failure("world_state.invalid_mod_state_mod",
                                     "mod state mod id must be a valid namespace id");
    }
    if (state_key.empty()) {
        return core::Status::failure("world_state.invalid_mod_state_key",
                                     "mod state key is required");
    }
    return core::Status::ok();
}

core::Status BuildObjectDatabase::insert(build::BuildPieceRecord record) {
    auto status = record.validate();
    if (!status) {
        return status;
    }
    const auto record_key = key(record.object_id);
    if (records_.contains(record_key)) {
        return core::Status::failure("world_state.duplicate_build_object",
                                     "build object save id is already present");
    }
    records_.emplace(record_key, std::move(record));
    return core::Status::ok();
}

build::BuildPieceRecord* BuildObjectDatabase::find(core::SaveId id) noexcept {
    const auto found = records_.find(key(id));
    return found == records_.end() ? nullptr : &found->second;
}

const build::BuildPieceRecord* BuildObjectDatabase::find(core::SaveId id) const noexcept {
    const auto found = records_.find(key(id));
    return found == records_.end() ? nullptr : &found->second;
}

bool BuildObjectDatabase::contains(core::SaveId id) const noexcept {
    return records_.contains(key(id));
}

std::vector<const build::BuildPieceRecord*> BuildObjectDatabase::records() const {
    std::vector<const build::BuildPieceRecord*> result;
    result.reserve(records_.size());
    for (const auto& [_, record] : records_) {
        result.push_back(&record);
    }
    return result;
}

std::size_t BuildObjectDatabase::count() const noexcept {
    return records_.size();
}

core::Status EntityDatabase::insert(entities::EntityRecord record) {
    auto status = record.validate();
    if (!status) {
        return status;
    }

    const auto runtime_key = key(record.runtime_handle);
    if (records_by_runtime_.contains(runtime_key)) {
        return core::Status::failure("world_state.duplicate_entity_runtime",
                                     "entity runtime handle is already present");
    }
    const auto net_key = key(record.net_id);
    if (runtime_by_net_id_.contains(net_key)) {
        return core::Status::failure("world_state.duplicate_entity_net",
                                     "entity net id is already present");
    }
    if (record.save_id.is_valid() && runtime_by_save_id_.contains(key(record.save_id))) {
        return core::Status::failure("world_state.duplicate_entity_save",
                                     "entity save id is already present");
    }

    runtime_by_net_id_.emplace(net_key, runtime_key);
    if (record.save_id.is_valid()) {
        runtime_by_save_id_.emplace(key(record.save_id), runtime_key);
    }
    records_by_runtime_.emplace(runtime_key, std::move(record));
    return core::Status::ok();
}

bool EntityDatabase::erase(core::RuntimeHandle handle) noexcept {
    const auto found = records_by_runtime_.find(key(handle));
    if (found == records_by_runtime_.end()) {
        return false;
    }
    runtime_by_net_id_.erase(key(found->second.net_id));
    if (found->second.save_id.is_valid()) {
        runtime_by_save_id_.erase(key(found->second.save_id));
    }
    records_by_runtime_.erase(found);
    return true;
}

entities::EntityRecord* EntityDatabase::find(core::RuntimeHandle handle) noexcept {
    const auto found = records_by_runtime_.find(key(handle));
    return found == records_by_runtime_.end() ? nullptr : &found->second;
}

const entities::EntityRecord* EntityDatabase::find(core::RuntimeHandle handle) const noexcept {
    const auto found = records_by_runtime_.find(key(handle));
    return found == records_by_runtime_.end() ? nullptr : &found->second;
}

entities::EntityRecord* EntityDatabase::find_by_save_id(core::SaveId id) noexcept {
    const auto runtime = runtime_by_save_id_.find(key(id));
    if (runtime == runtime_by_save_id_.end()) {
        return nullptr;
    }
    const auto found = records_by_runtime_.find(runtime->second);
    return found == records_by_runtime_.end() ? nullptr : &found->second;
}

const entities::EntityRecord* EntityDatabase::find_by_net_id(core::NetId id) const noexcept {
    const auto runtime = runtime_by_net_id_.find(key(id));
    if (runtime == runtime_by_net_id_.end()) {
        return nullptr;
    }
    const auto found = records_by_runtime_.find(runtime->second);
    return found == records_by_runtime_.end() ? nullptr : &found->second;
}

const entities::EntityRecord* EntityDatabase::find_by_save_id(core::SaveId id) const noexcept {
    const auto runtime = runtime_by_save_id_.find(key(id));
    if (runtime == runtime_by_save_id_.end()) {
        return nullptr;
    }
    const auto found = records_by_runtime_.find(runtime->second);
    return found == records_by_runtime_.end() ? nullptr : &found->second;
}

std::vector<const entities::EntityRecord*> EntityDatabase::records() const {
    std::vector<const entities::EntityRecord*> result;
    result.reserve(records_by_runtime_.size());
    for (const auto& [_, record] : records_by_runtime_) {
        result.push_back(&record);
    }
    return result;
}

std::size_t EntityDatabase::count() const noexcept {
    return records_by_runtime_.size();
}

std::size_t EntityDatabase::persistent_count() const noexcept {
    return runtime_by_save_id_.size();
}

core::Status CargoDatabase::insert(cargo::CargoRecord record) {
    auto status = record.validate();
    if (!status) {
        return status;
    }
    const auto record_key = key(record.cargo_id);
    if (records_.contains(record_key)) {
        return core::Status::failure("world_state.duplicate_cargo",
                                     "cargo save id is already present");
    }
    records_.emplace(record_key, std::move(record));
    return core::Status::ok();
}

cargo::CargoRecord* CargoDatabase::find(core::SaveId id) noexcept {
    const auto found = records_.find(key(id));
    return found == records_.end() ? nullptr : &found->second;
}

const cargo::CargoRecord* CargoDatabase::find(core::SaveId id) const noexcept {
    const auto found = records_.find(key(id));
    return found == records_.end() ? nullptr : &found->second;
}

std::vector<const cargo::CargoRecord*> CargoDatabase::records() const {
    std::vector<const cargo::CargoRecord*> result;
    result.reserve(records_.size());
    for (const auto& [_, record] : records_) {
        result.push_back(&record);
    }
    return result;
}

std::size_t CargoDatabase::count() const noexcept {
    return records_.size();
}

core::Status InventoryDatabase::insert(InventoryRecord record) {
    auto status = record.validate();
    if (!status) {
        return status;
    }
    const auto record_key = key(record.owner_id);
    if (records_by_owner_.contains(record_key)) {
        return core::Status::failure("world_state.duplicate_inventory",
                                     "inventory owner already has an inventory");
    }
    records_by_owner_.emplace(record_key, std::move(record));
    return core::Status::ok();
}

InventoryRecord* InventoryDatabase::find(core::SaveId owner_id) noexcept {
    const auto found = records_by_owner_.find(key(owner_id));
    return found == records_by_owner_.end() ? nullptr : &found->second;
}

const InventoryRecord* InventoryDatabase::find(core::SaveId owner_id) const noexcept {
    const auto found = records_by_owner_.find(key(owner_id));
    return found == records_by_owner_.end() ? nullptr : &found->second;
}

std::vector<const InventoryRecord*> InventoryDatabase::records() const {
    std::vector<const InventoryRecord*> result;
    result.reserve(records_by_owner_.size());
    for (const auto& [_, record] : records_by_owner_) {
        result.push_back(&record);
    }
    return result;
}

std::size_t InventoryDatabase::count() const noexcept {
    return records_by_owner_.size();
}

core::Status WorkpieceDatabase::insert(WorkpieceRecord record) {
    auto status = record.validate();
    if (!status) {
        return status;
    }
    const auto record_key = key(record.workpiece_id);
    if (records_.contains(record_key)) {
        return core::Status::failure("world_state.duplicate_workpiece",
                                     "workpiece id is already present");
    }
    records_.emplace(record_key, std::move(record));
    return core::Status::ok();
}

WorkpieceRecord* WorkpieceDatabase::find(core::WorkpieceId id) noexcept {
    const auto found = records_.find(key(id));
    return found == records_.end() ? nullptr : &found->second;
}

const WorkpieceRecord* WorkpieceDatabase::find(core::WorkpieceId id) const noexcept {
    const auto found = records_.find(key(id));
    return found == records_.end() ? nullptr : &found->second;
}

std::vector<const WorkpieceRecord*> WorkpieceDatabase::records() const {
    std::vector<const WorkpieceRecord*> result;
    result.reserve(records_.size());
    for (const auto& [_, record] : records_) {
        result.push_back(&record);
    }
    return result;
}

std::size_t WorkpieceDatabase::count() const noexcept {
    return records_.size();
}

core::Status PhysicalResourceDatabase::insert(entities::PhysicalResourceRecord record) {
    auto status = record.validate();
    if (!status)
        return status;
    const auto [_, inserted] = records_.emplace(record.resource_id.value(), std::move(record));
    if (!inserted) {
        return core::Status::failure("world_state.duplicate_physical_resource",
                                     "physical resource save id already exists");
    }
    return core::Status::ok();
}

entities::PhysicalResourceRecord* PhysicalResourceDatabase::find(core::SaveId id) noexcept {
    const auto found = records_.find(id.value());
    return found == records_.end() ? nullptr : &found->second;
}

const entities::PhysicalResourceRecord*
PhysicalResourceDatabase::find(core::SaveId id) const noexcept {
    const auto found = records_.find(id.value());
    return found == records_.end() ? nullptr : &found->second;
}

std::vector<const entities::PhysicalResourceRecord*> PhysicalResourceDatabase::records() const {
    std::vector<const entities::PhysicalResourceRecord*> result;
    result.reserve(records_.size());
    for (const auto& [_, record] : records_)
        result.push_back(&record);
    return result;
}

std::size_t PhysicalResourceDatabase::count() const noexcept {
    return records_.size();
}

core::Status AssemblyDatabase::insert(assemblies::AssemblyRecord record) {
    auto status = record.validate_record();
    if (!status) {
        return status;
    }
    const auto record_key = key(record.assembly_id);
    if (records_.contains(record_key)) {
        return core::Status::failure("world_state.duplicate_assembly",
                                     "assembly save id is already present");
    }
    records_.emplace(record_key, std::move(record));
    return core::Status::ok();
}

assemblies::AssemblyRecord* AssemblyDatabase::find(core::SaveId id) noexcept {
    const auto found = records_.find(key(id));
    return found == records_.end() ? nullptr : &found->second;
}

const assemblies::AssemblyRecord* AssemblyDatabase::find(core::SaveId id) const noexcept {
    const auto found = records_.find(key(id));
    return found == records_.end() ? nullptr : &found->second;
}

std::vector<const assemblies::AssemblyRecord*> AssemblyDatabase::records() const {
    std::vector<const assemblies::AssemblyRecord*> result;
    result.reserve(records_.size());
    for (const auto& [_, record] : records_) {
        result.push_back(&record);
    }
    return result;
}

std::size_t AssemblyDatabase::count() const noexcept {
    return records_.size();
}

core::Status ProcessDatabase::insert(processes::ProcessInstance instance) {
    auto status = instance.validate();
    if (!status) {
        return status;
    }

    const auto record_key = key(instance.process_id);
    if (records_.contains(record_key)) {
        return core::Status::failure("world_state.duplicate_process",
                                     "process id is already present");
    }
    const auto process_id = instance.process_id;
    records_.emplace(record_key, std::move(instance));
    insertion_order_.push_back(process_id);
    return core::Status::ok();
}

processes::ProcessInstance* ProcessDatabase::find(core::ProcessId id) noexcept {
    const auto found = records_.find(key(id));
    return found == records_.end() ? nullptr : &found->second;
}

const processes::ProcessInstance* ProcessDatabase::find(core::ProcessId id) const noexcept {
    const auto found = records_.find(key(id));
    return found == records_.end() ? nullptr : &found->second;
}

std::vector<const processes::ProcessInstance*>
ProcessDatabase::find_by_owner(core::SaveId owner_id) const {
    std::vector<const processes::ProcessInstance*> result;
    for (const auto& [_, process] : records_) {
        if (process.owner_id == owner_id) {
            result.push_back(&process);
        }
    }
    return result;
}

std::vector<const processes::ProcessInstance*> ProcessDatabase::records() const {
    std::vector<const processes::ProcessInstance*> result;
    result.reserve(records_.size());
    for (const auto& [_, record] : records_) {
        result.push_back(&record);
    }
    return result;
}

std::span<const core::ProcessId> ProcessDatabase::insertion_order() const noexcept {
    return insertion_order_;
}

core::Result<std::size_t> ProcessDatabase::advance_all(simulation::WorldTick world_time,
                                                       processes::ProcessModifiers modifiers) {
    return advance_all(world_time, [modifiers](const processes::ProcessInstance&) {
        return core::Result<processes::ProcessModifiers>::success(modifiers);
    });
}

core::Result<std::size_t> ProcessDatabase::advance_all(simulation::WorldTick world_time,
                                                       const ProcessModifierResolver& resolver) {
    // Resolve and advance against a staged copy. A bad modifier resolution or a time-reversal in
    // any one process must not leave earlier records advanced by a command that reports failure.
    auto staged_records = records_;
    std::size_t advanced = 0;
    for (auto& [_, process] : staged_records) {
        auto modifiers = resolver(process);
        if (!modifiers) {
            return core::Result<std::size_t>::failure(modifiers.error().code,
                                                      modifiers.error().message);
        }

        const auto previous_state = process.state;
        const auto previous_update_time = process.last_eval;
        const auto previous_work = process.accrued_work_ticks;
        auto status = processes::ProcessRuntime::evaluate(
            process, world_time, modifiers.value(),
            processes::ProcessEvaluationTrigger::save_load_validation);
        if (!status) {
            return core::Result<std::size_t>::failure(status.error().code, status.error().message);
        }
        if (process.state != previous_state || process.last_eval != previous_update_time ||
            process.accrued_work_ticks != previous_work) {
            ++advanced;
        }
    }

    // Commit values into their existing nodes so handles returned by find()/records() remain
    // valid across an advancement. The staged map has exactly the same keys as records_; no
    // insertion or erasure occurs during this operation.
    for (auto& [record_key, process] : staged_records) {
        records_.at(record_key) = std::move(process);
    }
    return core::Result<std::size_t>::success(advanced);
}

core::Result<std::size_t>
ProcessDatabase::evaluate_owner(core::SaveId owner_id, simulation::WorldTick world_time,
                                processes::ProcessEvaluationTrigger trigger,
                                const ProcessModifierResolver& resolver) {
    auto staged_records = records_;
    std::size_t evaluated = 0;
    for (auto& [_, process] : staged_records) {
        if (process.owner_id != owner_id) {
            continue;
        }
        auto modifiers = resolver(process);
        if (!modifiers) {
            return core::Result<std::size_t>::failure(modifiers.error().code,
                                                      modifiers.error().message);
        }
        auto status =
            processes::ProcessRuntime::evaluate(process, world_time, modifiers.value(), trigger);
        if (!status) {
            return core::Result<std::size_t>::failure(status.error().code, status.error().message);
        }
        ++evaluated;
    }
    for (auto& [record_key, process] : staged_records) {
        records_.at(record_key) = std::move(process);
    }
    return core::Result<std::size_t>::success(evaluated);
}

std::size_t ProcessDatabase::count() const noexcept {
    return records_.size();
}

networks::SpatialNetwork& NetworkDatabase::get_or_create(networks::NetworkKind kind) {
    const auto record_key = key(kind);
    auto found = networks_.find(record_key);
    if (found != networks_.end()) {
        return found->second;
    }

    auto [it, _] = networks_.emplace(record_key, networks::SpatialNetwork(kind));
    return it->second;
}

networks::SpatialNetwork* NetworkDatabase::find(networks::NetworkKind kind) noexcept {
    const auto found = networks_.find(key(kind));
    return found == networks_.end() ? nullptr : &found->second;
}

const networks::SpatialNetwork* NetworkDatabase::find(networks::NetworkKind kind) const noexcept {
    const auto found = networks_.find(key(kind));
    return found == networks_.end() ? nullptr : &found->second;
}

std::vector<const networks::SpatialNetwork*> NetworkDatabase::records() const {
    std::vector<const networks::SpatialNetwork*> result;
    result.reserve(networks_.size());
    for (const auto& [_, network] : networks_) {
        result.push_back(&network);
    }
    return result;
}

core::Result<networks::SpatialNetworkDerivationStats>
NetworkDatabase::rebuild_from_ports(const BuildObjectDatabase& build_objects,
                                    const AssemblyDatabase& assemblies) {
    networks::SpatialNetworkDerivationInput input;
    input.build_pieces = build_objects.records();
    input.assemblies = assemblies.records();

    auto derived = networks::SpatialNetworkDeriver::derive(input);
    if (!derived) {
        return core::Result<networks::SpatialNetworkDerivationStats>::failure(
            derived.error().code, derived.error().message);
    }

    const auto stats = derived.value().stats;
    networks_.clear();
    for (auto& network : derived.value().networks) {
        networks_.emplace(key(network.kind()), std::move(network));
    }

    return core::Result<networks::SpatialNetworkDerivationStats>::success(stats);
}

std::size_t NetworkDatabase::count() const noexcept {
    return networks_.size();
}

core::Status ModStateDatabase::insert(ModStateRecord record) {
    auto status = record.validate();
    if (!status) {
        return status;
    }
    const auto record_key = mod_state_key(record.mod_id, record.state_key);
    if (records_.contains(record_key)) {
        return core::Status::failure("world_state.duplicate_mod_state",
                                     "mod state key is already present for this mod");
    }
    records_.emplace(record_key, std::move(record));
    return core::Status::ok();
}

const ModStateRecord* ModStateDatabase::find(std::string_view mod_id,
                                             std::string_view state_key) const {
    const auto found = records_.find(mod_state_key(mod_id, state_key));
    return found == records_.end() ? nullptr : &found->second;
}

std::vector<const ModStateRecord*> ModStateDatabase::records() const {
    std::vector<const ModStateRecord*> result;
    result.reserve(records_.size());
    for (const auto& [_, record] : records_) {
        result.push_back(&record);
    }
    return result;
}

std::size_t ModStateDatabase::count() const noexcept {
    return records_.size();
}

core::Status FireDatabase::insert(simulation::FireInstance fire) {
    auto status = fire.validate_record();
    if (!status)
        return status;
    if (!records_.emplace(fire.fire_id.value(), std::move(fire)).second) {
        return core::Status::failure("world_state.duplicate_fire", "fire id is already present");
    }
    return core::Status::ok();
}

simulation::FireInstance* FireDatabase::find(core::SaveId id) noexcept {
    auto found = records_.find(id.value());
    return found == records_.end() ? nullptr : &found->second;
}

const simulation::FireInstance* FireDatabase::find(core::SaveId id) const noexcept {
    auto found = records_.find(id.value());
    return found == records_.end() ? nullptr : &found->second;
}

std::vector<const simulation::FireInstance*> FireDatabase::records() const {
    std::vector<const simulation::FireInstance*> result;
    result.reserve(records_.size());
    for (const auto& [_, fire] : records_)
        result.push_back(&fire);
    return result;
}

std::size_t FireDatabase::count() const noexcept {
    return records_.size();
}

WorldState::WorldState(WorldStateDesc desc)
    : metadata_(std::move(desc.metadata)), voxel_palette_manifest_(std::move(desc.voxel_palette)),
      save_ids_(desc.next_save_id), runtime_handles_(desc.next_runtime_handle),
      entity_net_ids_(desc.next_entity_net_id), process_ids_(desc.next_process_id) {}

const save::SaveMetadata& WorldState::metadata() const noexcept {
    return metadata_;
}

const VoxelPaletteManifest& WorldState::voxel_palette_manifest() const noexcept {
    return voxel_palette_manifest_;
}

simulation::WorldTick WorldState::world_time() const noexcept {
    return metadata_.world_time;
}

core::Status WorldState::advance_world_time(simulation::WorldTick delta) noexcept {
    simulation::WorldClock clock(metadata_.world_time);
    auto status = clock.advance(delta);
    if (status) {
        metadata_.world_time = clock.now();
    }
    return status;
}

core::Status
WorldState::advance_world_time_hours(std::uint64_t hours,
                                     const simulation::WorldTimeConfig& config) noexcept {
    simulation::WorldClock clock(metadata_.world_time);
    auto status = clock.advance_hours(hours, config);
    if (status) {
        metadata_.world_time = clock.now();
    }
    return status;
}

save::SaveIdAllocator& WorldState::save_ids() noexcept {
    return save_ids_;
}

const save::SaveIdAllocator& WorldState::save_ids() const noexcept {
    return save_ids_;
}

entities::RuntimeHandleAllocator& WorldState::runtime_handles() noexcept {
    return runtime_handles_;
}

const entities::RuntimeHandleAllocator& WorldState::runtime_handles() const noexcept {
    return runtime_handles_;
}

entities::EntityNetIdAllocator& WorldState::entity_net_ids() noexcept {
    return entity_net_ids_;
}

const entities::EntityNetIdAllocator& WorldState::entity_net_ids() const noexcept {
    return entity_net_ids_;
}

processes::ProcessIdAllocator& WorldState::process_ids() noexcept {
    return process_ids_;
}

const processes::ProcessIdAllocator& WorldState::process_ids() const noexcept {
    return process_ids_;
}

dirty::DirtyRegionTracker& WorldState::dirty_regions() noexcept {
    return dirty_regions_;
}

const dirty::DirtyRegionTracker& WorldState::dirty_regions() const noexcept {
    return dirty_regions_;
}

RegionGraph& WorldState::regions() noexcept {
    return regions_;
}

const RegionGraph& WorldState::regions() const noexcept {
    return regions_;
}

ChunkDatabase& WorldState::chunks() noexcept {
    return chunks_;
}

const ChunkDatabase& WorldState::chunks() const noexcept {
    return chunks_;
}

BuildObjectDatabase& WorldState::build_objects() noexcept {
    return build_objects_;
}

const BuildObjectDatabase& WorldState::build_objects() const noexcept {
    return build_objects_;
}

EntityDatabase& WorldState::entities() noexcept {
    return entities_;
}

const EntityDatabase& WorldState::entities() const noexcept {
    return entities_;
}

CargoDatabase& WorldState::cargo() noexcept {
    return cargo_;
}

const CargoDatabase& WorldState::cargo() const noexcept {
    return cargo_;
}

InventoryDatabase& WorldState::inventories() noexcept {
    return inventories_;
}

const InventoryDatabase& WorldState::inventories() const noexcept {
    return inventories_;
}

WorkpieceDatabase& WorldState::workpieces() noexcept {
    return workpieces_;
}

const WorkpieceDatabase& WorldState::workpieces() const noexcept {
    return workpieces_;
}

PhysicalResourceDatabase& WorldState::physical_resources() noexcept {
    return physical_resources_;
}

const PhysicalResourceDatabase& WorldState::physical_resources() const noexcept {
    return physical_resources_;
}

AssemblyDatabase& WorldState::assemblies() noexcept {
    return assemblies_;
}

const AssemblyDatabase& WorldState::assemblies() const noexcept {
    return assemblies_;
}

ProcessDatabase& WorldState::processes() noexcept {
    return processes_;
}

const ProcessDatabase& WorldState::processes() const noexcept {
    return processes_;
}

rooms::RoomGraph& WorldState::rooms() noexcept {
    return rooms_;
}

const rooms::RoomGraph& WorldState::rooms() const noexcept {
    return rooms_;
}

NetworkDatabase& WorldState::networks() noexcept {
    return networks_;
}

const NetworkDatabase& WorldState::networks() const noexcept {
    return networks_;
}

ModStateDatabase& WorldState::mod_states() noexcept {
    return mod_states_;
}

const ModStateDatabase& WorldState::mod_states() const noexcept {
    return mod_states_;
}

std::vector<MissingPrototypeObject>& WorldState::missing_prototypes() noexcept {
    return missing_prototypes_;
}

const std::vector<MissingPrototypeObject>& WorldState::missing_prototypes() const noexcept {
    return missing_prototypes_;
}

FireDatabase& WorldState::fires() noexcept {
    return fires_;
}
const FireDatabase& WorldState::fires() const noexcept {
    return fires_;
}

bool WorldState::contains_saved_object(core::SaveId id) const noexcept {
    if (!id.is_valid()) {
        return false;
    }
    return build_objects_.contains(id) || cargo_.find(id) != nullptr ||
           entities_.find_by_save_id(id) != nullptr || assemblies_.find(id) != nullptr ||
           workpieces_.find(core::WorkpieceId::from_value(id.value())) != nullptr ||
           physical_resources_.find(id) != nullptr;
}

WorldStateStats WorldState::stats() const noexcept {
    return WorldStateStats{
        chunks_.chunk_count(),
        regions_.region_count(),
        regions_.connection_count(),
        dirty_regions_.size(),
        build_objects_.count(),
        entities_.count(),
        entities_.persistent_count(),
        cargo_.count(),
        inventories_.count(),
        workpieces_.count(),
        physical_resources_.count(),
        assemblies_.count(),
        processes_.count(),
        rooms_.room_count(),
        networks_.count(),
        mod_states_.count(),
        missing_prototypes_.size(),
        fires_.count(),
    };
}

} // namespace heartstead::world
