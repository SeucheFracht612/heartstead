#include "engine/world/world_snapshot.hpp"

#include "engine/save/missing_prototype_recovery.hpp"
#include "engine/workpieces/workpiece_codec.hpp"
#include "engine/world/chunks/chunk_edit_delta_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heartstead::world {

namespace {

template <typename T, typename Less>
[[nodiscard]] std::vector<const T*> sorted_records(std::vector<const T*> records, Less less) {
    std::ranges::sort(records, less);
    return records;
}

[[nodiscard]] core::Status track_unique_save_id(std::set<std::uint64_t>& ids, core::SaveId id,
                                                std::string_view label) {
    if (!id.is_valid()) {
        return core::Status::failure("world_snapshot.invalid_save_id",
                                     std::string(label) + " has an invalid save id");
    }
    if (!ids.insert(id.value()).second) {
        return core::Status::failure("world_snapshot.duplicate_save_id",
                                     std::string(label) + " duplicates save id " + id.to_string());
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status require_saved_object_id(const std::set<std::uint64_t>& ids,
                                                   core::SaveId id, std::string_view label) {
    if (!id.is_valid()) {
        return core::Status::failure("world_snapshot.invalid_owner",
                                     std::string(label) + " owner save id is invalid");
    }
    if (!ids.contains(id.value())) {
        return core::Status::failure("world_snapshot.missing_owner",
                                     std::string(label) + " references a missing owner save id " +
                                         id.to_string());
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status require_build_piece_id(const std::set<std::uint64_t>& ids,
                                                  core::SaveId id, std::string_view code,
                                                  std::string_view message) {
    if (!id.is_valid() || !ids.contains(id.value())) {
        return core::Status::failure(std::string(code), std::string(message));
    }
    return core::Status::ok();
}

[[nodiscard]] std::uint64_t max_snapshot_save_id(const save::SaveSnapshot& snapshot) noexcept {
    std::uint64_t max_id = 0;
    const auto visit = [&max_id](core::SaveId id) {
        if (id.value() > max_id) {
            max_id = id.value();
        }
    };
    for (const auto& build_piece : snapshot.build_pieces) {
        visit(build_piece.object_id);
    }
    for (const auto& entity : snapshot.entities) {
        visit(entity.save_id);
    }
    for (const auto& cargo : snapshot.cargo_records) {
        visit(cargo.cargo_id);
    }
    for (const auto& workpiece : snapshot.workpieces) {
        visit(core::SaveId::from_value(workpiece.workpiece_id.value()));
    }
    for (const auto& assembly : snapshot.assemblies) {
        visit(assembly.assembly_id);
    }
    for (const auto& fire : snapshot.fires) {
        visit(fire.fire_id);
    }
    for (const auto& missing : snapshot.missing_prototypes) {
        if (missing.kind != MissingPrototypeKind::process) {
            visit(core::SaveId::from_value(missing.stable_id));
        }
    }
    return max_id;
}

[[nodiscard]] std::uint64_t max_snapshot_process_id(const save::SaveSnapshot& snapshot) noexcept {
    std::uint64_t max_id = 0;
    for (const auto& process : snapshot.processes) {
        if (process.process_id.value() > max_id) {
            max_id = process.process_id.value();
        }
    }
    for (const auto& missing : snapshot.missing_prototypes) {
        if (missing.kind == MissingPrototypeKind::process && missing.stable_id > max_id) {
            max_id = missing.stable_id;
        }
    }
    return max_id;
}

[[nodiscard]] core::Result<std::uint64_t> next_allocator_value(std::uint64_t configured_next,
                                                               std::uint64_t maximum_used,
                                                               std::string_view allocator_name) {
    if (maximum_used == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<std::uint64_t>::failure("world_snapshot.id_range_exhausted",
                                                    std::string(allocator_name) +
                                                        " id range is exhausted by the snapshot");
    }
    return core::Result<std::uint64_t>::success(
        std::max(configured_next, maximum_used + std::uint64_t{1}));
}

} // namespace

core::Result<save::SaveSnapshot> WorldSnapshotBridge::export_snapshot(const WorldState& state) {
    save::SaveSnapshot snapshot;
    snapshot.metadata = state.metadata();
    snapshot.voxel_palette = state.voxel_palette_manifest();

    std::map<ChunkCoord, std::vector<const VoxelEditRecord*>> edits_by_chunk;
    for (const auto& edit : state.chunks().edit_log()) {
        edits_by_chunk[edit.chunk_coord].push_back(&edit);
    }
    for (const auto& [coord, edits] : edits_by_chunk) {
        snapshot.chunk_edits.push_back(save::ChunkEditSaveRecord{
            coord,
            ChunkEditDeltaTextCodec::encode(coord, edits),
        });
    }

    for (const auto* build_piece :
         sorted_records(state.build_objects().records(), [](const auto* lhs, const auto* rhs) {
             return lhs->object_id.value() < rhs->object_id.value();
         })) {
        snapshot.build_pieces.push_back(*build_piece);
    }

    for (const auto* entity :
         sorted_records(state.entities().records(), [](const auto* lhs, const auto* rhs) {
             return lhs->runtime_handle.value() < rhs->runtime_handle.value();
         })) {
        if (entity->persistent) {
            snapshot.entities.push_back(save::EntitySaveRecord{
                entity->save_id,
                entity->prototype_id,
                entity->kind,
                entity->sleeping,
                entity->encoded_state,
                entity->transform,
            });
        }
    }

    for (const auto* resource :
         sorted_records(state.physical_resources().records(), [](const auto* lhs, const auto* rhs) {
             return lhs->resource_id.value() < rhs->resource_id.value();
         })) {
        entities::Transform transform;
        transform.position = resource->position;
        snapshot.entities.push_back(save::EntitySaveRecord{
            resource->resource_id,
            resource->prototype_id,
            entities::EntityKind::temporary_physics,
            resource->state == entities::PhysicalResourceState::settled_sleeping ||
                resource->state == entities::PhysicalResourceState::frozen_static,
            entities::PhysicalResourceTextCodec::encode(*resource),
            transform,
        });
    }

    for (const auto* inventory :
         sorted_records(state.inventories().records(), [](const auto* lhs, const auto* rhs) {
             return lhs->owner_id.value() < rhs->owner_id.value();
         })) {
        snapshot.inventories.push_back(
            save::InventorySaveRecord{inventory->owner_id, inventory->stacks});
    }

    for (const auto* cargo :
         sorted_records(state.cargo().records(), [](const auto* lhs, const auto* rhs) {
             return lhs->cargo_id.value() < rhs->cargo_id.value();
         })) {
        snapshot.cargo_records.push_back(*cargo);
    }

    for (const auto* workpiece :
         sorted_records(state.workpieces().records(), [](const auto* lhs, const auto* rhs) {
             return lhs->workpiece_id.value() < rhs->workpiece_id.value();
         })) {
        snapshot.workpieces.push_back(save::WorkpieceSaveRecord{
            workpiece->workpiece_id,
            workpiece->prototype_id,
            workpiece->grid.shape(),
            workpieces::WorkpieceGridTextCodec::encode(workpiece->grid),
            workpiece->material_prototype_id,
            workpiece->server_state.has_value()
                ? workpieces::WorkpieceServerStateTextCodec::encode(*workpiece->server_state,
                                                                    workpiece->grid.shape())
                : std::string{},
            // NetIds are connection-local and cannot identify the same player after a reload.
            // Persist an unbound owner until a stable player identity can explicitly rebind it.
            core::NetId{},
            workpiece->revision,
            workpiece->committed,
        });
    }

    for (const auto* assembly :
         sorted_records(state.assemblies().records(), [](const auto* lhs, const auto* rhs) {
             return lhs->assembly_id.value() < rhs->assembly_id.value();
         })) {
        snapshot.assemblies.push_back(*assembly);
    }

    for (const auto* process :
         sorted_records(state.processes().records(), [](const auto* lhs, const auto* rhs) {
             return lhs->process_id.value() < rhs->process_id.value();
         })) {
        snapshot.processes.push_back(*process);
    }

    for (const auto* fire :
         sorted_records(state.fires().records(), [](const auto* lhs, const auto* rhs) {
             return lhs->fire_id.value() < rhs->fire_id.value();
         })) {
        snapshot.fires.push_back(*fire);
    }

    for (const auto* mod_state :
         sorted_records(state.mod_states().records(), [](const auto* lhs, const auto* rhs) {
             if (lhs->mod_id == rhs->mod_id) {
                 return lhs->state_key < rhs->state_key;
             }
             return lhs->mod_id < rhs->mod_id;
         })) {
        snapshot.mod_states.push_back(save::ModStateSaveRecord{
            mod_state->mod_id, mod_state->state_key, mod_state->encoded_state});
    }
    snapshot.missing_prototypes = state.missing_prototypes();

    return core::Result<save::SaveSnapshot>::success(std::move(snapshot));
}

core::Result<WorldState>
WorldSnapshotBridge::import_validated_snapshot(const save::SaveSnapshot& snapshot,
                                               const modding::PrototypeRegistry& prototypes,
                                               WorldSnapshotLoadConfig config) {
    auto recoverable = snapshot;
    auto recovery = save::preserve_missing_prototypes(recoverable, prototypes);
    if (!recovery) {
        return core::Result<WorldState>::failure(recovery.error().code, recovery.error().message);
    }
    const auto validation = save::SaveSnapshotValidator::validate(recoverable, prototypes);
    if (!validation.valid()) {
        const auto& first_issue = validation.issues.front();
        return core::Result<WorldState>::failure(first_issue.code, first_issue.message);
    }

    return import_snapshot(recoverable, config);
}

core::Result<WorldState> WorldSnapshotBridge::import_snapshot(const save::SaveSnapshot& snapshot,
                                                              WorldSnapshotLoadConfig config) {
    auto metadata_status = snapshot.metadata.validate();
    if (!metadata_status) {
        return core::Result<WorldState>::failure(metadata_status.error().code,
                                                 metadata_status.error().message);
    }

    auto next_save_id =
        next_allocator_value(config.next_save_id, max_snapshot_save_id(snapshot), "save");
    if (!next_save_id) {
        return core::Result<WorldState>::failure(next_save_id.error().code,
                                                 next_save_id.error().message);
    }
    auto next_process_id =
        next_allocator_value(config.next_process_id, max_snapshot_process_id(snapshot), "process");
    if (!next_process_id) {
        return core::Result<WorldState>::failure(next_process_id.error().code,
                                                 next_process_id.error().message);
    }
    WorldStateDesc desc;
    desc.metadata = snapshot.metadata;
    desc.voxel_palette = snapshot.voxel_palette;
    desc.next_save_id = next_save_id.value();
    desc.next_runtime_handle = config.next_runtime_handle;
    desc.next_entity_net_id = config.next_entity_net_id;
    desc.next_process_id = next_process_id.value();
    WorldState state(desc);

    std::set<std::uint64_t> save_ids;
    std::set<std::uint64_t> build_piece_ids;
    std::set<ChunkCoord> chunk_edit_coords;
    std::vector<VoxelEditRecord> restored_chunk_edits;

    for (const auto& chunk : snapshot.chunk_edits) {
        if (!chunk_edit_coords.insert(chunk.coord).second) {
            return core::Result<WorldState>::failure(
                "world_snapshot.duplicate_chunk_edit",
                "snapshot contains duplicate chunk edit records for one chunk coordinate");
        }
        auto edits = ChunkEditDeltaTextCodec::decode(chunk.coord, chunk.encoded_edit_delta);
        if (!edits) {
            return core::Result<WorldState>::failure(edits.error().code, edits.error().message);
        }
        if (edits.value().size() > restored_chunk_edits.max_size() - restored_chunk_edits.size()) {
            return core::Result<WorldState>::failure(
                "world_snapshot.chunk_delta_too_large",
                "snapshot chunk edit records exceed the addressable import limit");
        }
        restored_chunk_edits.insert(restored_chunk_edits.end(), edits.value().begin(),
                                    edits.value().end());
    }
    if (!restored_chunk_edits.empty()) {
        auto status = state.chunks().apply_saved_edits(restored_chunk_edits, state.dirty_regions());
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
    }

    for (const auto& build_piece : snapshot.build_pieces) {
        auto status = track_unique_save_id(save_ids, build_piece.object_id, "build piece");
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
        build_piece_ids.insert(build_piece.object_id.value());
        status = state.build_objects().insert(build_piece);
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
    }

    for (const auto& entity : snapshot.entities) {
        auto status = track_unique_save_id(save_ids, entity.save_id, "entity");
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
        if (entity.encoded_state.starts_with(entities::physical_resource_state_magic)) {
            auto resource = entities::PhysicalResourceTextCodec::decode(
                entity.save_id, entity.prototype_id, entity.transform.position,
                entity.encoded_state);
            if (!resource) {
                return core::Result<WorldState>::failure(resource.error().code,
                                                         resource.error().message);
            }
            status = state.physical_resources().insert(std::move(resource).value());
            if (!status) {
                return core::Result<WorldState>::failure(status.error().code,
                                                         status.error().message);
            }
            continue;
        }
        auto runtime_handle = state.runtime_handles().reserve();
        if (!runtime_handle) {
            return core::Result<WorldState>::failure(runtime_handle.error().code,
                                                     runtime_handle.error().message);
        }
        entities::EntityRecord record;
        record.runtime_handle = runtime_handle.value();
        auto net_id = state.entity_net_ids().reserve();
        if (!net_id) {
            return core::Result<WorldState>::failure(net_id.error().code, net_id.error().message);
        }
        record.net_id = net_id.value();
        record.save_id = entity.save_id;
        record.prototype_id = entity.prototype_id;
        record.kind = entity.kind;
        record.transform = entity.transform;
        record.encoded_state = entity.encoded_state;
        record.persistent = true;
        record.sleeping = entity.sleeping;
        status = state.entities().insert(record);
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
    }

    for (const auto& cargo : snapshot.cargo_records) {
        auto status = track_unique_save_id(save_ids, cargo.cargo_id, "cargo");
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
        status = state.cargo().insert(cargo);
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
    }

    for (const auto& workpiece : snapshot.workpieces) {
        auto status = track_unique_save_id(
            save_ids, core::SaveId::from_value(workpiece.workpiece_id.value()), "workpiece");
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
        auto grid = workpieces::WorkpieceGridTextCodec::decode(workpiece.encoded_cells);
        if (!grid) {
            return core::Result<WorldState>::failure(grid.error().code, grid.error().message);
        }
        if (grid.value().shape() != workpiece.shape) {
            return core::Result<WorldState>::failure(
                "world_snapshot.workpiece_shape_mismatch",
                "decoded workpiece grid shape does not match save record shape");
        }
        std::optional<workpieces::WorkpieceServerState> server_state;
        if (!workpiece.encoded_server_state.empty()) {
            auto decoded_state = workpieces::WorkpieceServerStateTextCodec::decode(
                workpiece.encoded_server_state, workpiece.shape);
            if (!decoded_state) {
                return core::Result<WorldState>::failure(decoded_state.error().code,
                                                         decoded_state.error().message);
            }
            server_state = std::move(decoded_state).value();
        }
        status = state.workpieces().insert(
            WorkpieceRecord{workpiece.workpiece_id, workpiece.prototype_id, std::move(grid).value(),
                            workpiece.material_prototype_id, std::move(server_state),
                            core::NetId{}, workpiece.revision, workpiece.committed});
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
    }

    for (const auto& assembly : snapshot.assemblies) {
        auto restored_assembly = assembly;
        auto status = track_unique_save_id(save_ids, restored_assembly.assembly_id, "assembly");
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
        status = restored_assembly.validate_record();
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
        status = require_build_piece_id(build_piece_ids, restored_assembly.root_build_piece_id,
                                        "world_snapshot.missing_assembly_root",
                                        "assembly root build piece is not present in snapshot");
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
        restored_assembly.root_coord = state.build_objects()
                                           .find(restored_assembly.root_build_piece_id)
                                           ->transform.position.anchor;
        for (const auto& part : restored_assembly.parts) {
            status = require_build_piece_id(build_piece_ids, part.build_piece_id,
                                            "world_snapshot.missing_assembly_part",
                                            "assembly part build piece is not present in snapshot");
            if (!status) {
                return core::Result<WorldState>::failure(status.error().code,
                                                         status.error().message);
            }
        }
        for (const auto& port : restored_assembly.ports) {
            status = require_build_piece_id(
                build_piece_ids, port.source_build_piece_id,
                "world_snapshot.missing_assembly_port_source",
                "assembly port source build piece is not present in snapshot");
            if (!status) {
                return core::Result<WorldState>::failure(status.error().code,
                                                         status.error().message);
            }
        }
        status = state.assemblies().insert(std::move(restored_assembly));
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
    }

    for (const auto& inventory : snapshot.inventories) {
        auto status = require_saved_object_id(save_ids, inventory.owner_id, "inventory");
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
        status = state.inventories().insert(InventoryRecord{inventory.owner_id, inventory.stacks});
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
    }

    for (const auto& process : snapshot.processes) {
        auto status = require_saved_object_id(save_ids, process.owner_id, "process");
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
        status = state.processes().insert(process);
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
    }

    for (const auto& fire : snapshot.fires) {
        auto status = require_saved_object_id(save_ids, fire.fire_id, "fire");
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
        status = state.fires().insert(fire);
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
    }

    for (const auto& mod_state : snapshot.mod_states) {
        auto status = state.mod_states().insert(
            ModStateRecord{mod_state.mod_id, mod_state.state_key, mod_state.encoded_state});
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
    }

    std::set<std::pair<MissingPrototypeKind, std::uint64_t>> missing_keys;
    for (const auto& missing : snapshot.missing_prototypes) {
        auto status = missing.validate();
        if (!status) {
            return core::Result<WorldState>::failure(status.error().code, status.error().message);
        }
        if (!missing_keys.emplace(missing.kind, missing.stable_id).second) {
            return core::Result<WorldState>::failure(
                "world_snapshot.duplicate_missing_prototype",
                "snapshot contains duplicate missing prototype placeholders");
        }
        state.missing_prototypes().push_back(missing);
    }

    return core::Result<WorldState>::success(std::move(state));
}

} // namespace heartstead::world
