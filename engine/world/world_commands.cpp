#include "engine/world/world_commands.hpp"

#include "engine/assemblies/assembly_prototype.hpp"
#include "engine/build/build_piece_prototype.hpp"
#include "engine/cargo/cargo_prototype.hpp"
#include "engine/entities/entity_prototype.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/net/command_payload.hpp"
#include "engine/processes/process_prototype.hpp"
#include "engine/workpieces/pattern_library.hpp"
#include "engine/workpieces/workpiece_grid.hpp"
#include "engine/workpieces/workpiece_state.hpp"
#include "engine/world/process_modifiers.hpp"
#include "engine/world/voxel_change.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace heartstead::world {

namespace {

struct VoxelCommandPayload {
    ChunkCoord chunk;
    VoxelCoord voxel;
    std::optional<core::PrototypeId> prototype_id;
};

struct BuildPieceCommandPayload {
    core::PrototypeId prototype_id;
    build::Transform transform;
};

struct BuildCompleteCommandPayload {
    core::SaveId object_id;
};

struct WorkpieceEditCommandPayload {
    core::WorkpieceId workpiece_id;
    workpieces::WorkpieceOperation operation;
};

struct WorkpieceFinishCommandPayload {
    core::WorkpieceId workpiece_id;
    std::optional<core::PrototypeId> requested_pattern;
};

struct InventoryTransferCommandPayload {
    InventoryTransferRequest request;
};

struct ProcessStartCommandPayload {
    core::SaveId owner_id;
    core::PrototypeId prototype_id;
};

struct ProcessAdvanceAllCommandPayload {};

struct SleepCommandPayload {
    std::uint64_t hours = 0;
};

struct CargoCreateCommandPayload {
    core::PrototypeId prototype_id;
    WorldPosition position;
};

struct EntitySpawnCommandPayload {
    core::PrototypeId prototype_id;
    entities::Transform transform;
};

struct AssemblyPartCommandPayload {
    std::string name;
    core::SaveId build_piece_id;
};

struct AssemblyCreateCommandPayload {
    core::PrototypeId prototype_id;
    core::SaveId root_build_piece_id;
    std::vector<AssemblyPartCommandPayload> parts;
};

struct AssemblyBlueprintCommandPayload {
    core::PrototypeId prototype_id;
    core::SaveId root_build_piece_id;
};

struct AssemblyPlacePartCommandPayload {
    core::SaveId assembly_id;
    std::string part_name;
    core::SaveId build_piece_id;
};

struct AssemblyAdvanceCommandPayload {
    core::SaveId assembly_id;
};

struct AssemblyTransitionCommandPayload {
    core::SaveId assembly_id;
    assemblies::AssemblyState state = assemblies::AssemblyState::ready;
    std::string reason;
};

[[nodiscard]] std::vector<std::string_view> split(std::string_view value, char delimiter) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(delimiter, start);
        if (end == std::string_view::npos) {
            result.push_back(value.substr(start));
            break;
        }
        result.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

[[nodiscard]] core::Result<std::int64_t> parse_i64(std::string_view value,
                                                   std::string_view field_name) {
    std::int64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return core::Result<std::int64_t>::failure("world_command.invalid_number",
                                                   "invalid numeric command field: " +
                                                       std::string(field_name));
    }
    return core::Result<std::int64_t>::success(parsed);
}

[[nodiscard]] core::Result<std::uint16_t> parse_u16(std::string_view value,
                                                    std::string_view field_name) {
    auto parsed = parse_i64(value, field_name);
    if (!parsed) {
        return core::Result<std::uint16_t>::failure(parsed.error().code, parsed.error().message);
    }
    if (parsed.value() < 0 || parsed.value() > std::numeric_limits<std::uint16_t>::max()) {
        return core::Result<std::uint16_t>::failure("world_command.number_out_of_range",
                                                    "numeric command field is out of range: " +
                                                        std::string(field_name));
    }
    return core::Result<std::uint16_t>::success(static_cast<std::uint16_t>(parsed.value()));
}

[[nodiscard]] core::Result<std::uint8_t> parse_u8(std::string_view value,
                                                  std::string_view field_name) {
    auto parsed = parse_i64(value, field_name);
    if (!parsed) {
        return core::Result<std::uint8_t>::failure(parsed.error().code, parsed.error().message);
    }
    if (parsed.value() < 0 || parsed.value() > std::numeric_limits<std::uint8_t>::max()) {
        return core::Result<std::uint8_t>::failure("world_command.number_out_of_range",
                                                   "numeric command field is out of range: " +
                                                       std::string(field_name));
    }
    return core::Result<std::uint8_t>::success(static_cast<std::uint8_t>(parsed.value()));
}

[[nodiscard]] core::Result<std::uint64_t> parse_u64(std::string_view value,
                                                    std::string_view field_name) {
    std::uint64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return core::Result<std::uint64_t>::failure("world_command.invalid_number",
                                                    "invalid numeric command field: " +
                                                        std::string(field_name));
    }
    return core::Result<std::uint64_t>::success(parsed);
}

[[nodiscard]] core::Result<std::uint64_t> parse_positive_u64(std::string_view value,
                                                             std::string_view field_name) {
    auto parsed = parse_u64(value, field_name);
    if (!parsed) {
        return parsed;
    }
    if (parsed.value() == 0) {
        return core::Result<std::uint64_t>::failure(
            "world_command.number_out_of_range",
            "numeric command field must be a positive id: " + std::string(field_name));
    }
    return parsed;
}

[[nodiscard]] core::Result<std::uint32_t> parse_u32(std::string_view value,
                                                    std::string_view field_name) {
    auto parsed = parse_u64(value, field_name);
    if (!parsed) {
        return core::Result<std::uint32_t>::failure(parsed.error().code, parsed.error().message);
    }
    if (parsed.value() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<std::uint32_t>::failure("world_command.number_out_of_range",
                                                    "numeric command field is out of range: " +
                                                        std::string(field_name));
    }
    return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(parsed.value()));
}

[[nodiscard]] core::Result<std::size_t> parse_size(std::string_view value,
                                                   std::string_view field_name) {
    auto parsed = parse_u64(value, field_name);
    if (!parsed) {
        return core::Result<std::size_t>::failure(parsed.error().code, parsed.error().message);
    }
    if (parsed.value() > std::numeric_limits<std::size_t>::max()) {
        return core::Result<std::size_t>::failure("world_command.number_out_of_range",
                                                  "numeric command field is out of range: " +
                                                      std::string(field_name));
    }
    return core::Result<std::size_t>::success(static_cast<std::size_t>(parsed.value()));
}

[[nodiscard]] core::Result<double> parse_double(std::string_view value,
                                                std::string_view field_name) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stod(std::string(value), &consumed);
        if (consumed != value.size()) {
            return core::Result<double>::failure("world_command.invalid_number",
                                                 "invalid floating-point command field: " +
                                                     std::string(field_name));
        }
        return core::Result<double>::success(parsed);
    } catch (...) {
        return core::Result<double>::failure("world_command.invalid_number",
                                             "invalid floating-point command field: " +
                                                 std::string(field_name));
    }
}

[[nodiscard]] core::Result<ChunkCoord> parse_chunk_coord(std::string_view value) {
    const auto parts = split(value, '|');
    if (parts.size() != 3) {
        return core::Result<ChunkCoord>::failure("world_command.invalid_chunk_coord",
                                                 "chunk coord must contain x|y|z");
    }
    auto x = parse_i64(parts[0], "chunk_x");
    auto y = parse_i64(parts[1], "chunk_y");
    auto z = parse_i64(parts[2], "chunk_z");
    if (!x || !y || !z) {
        return core::Result<ChunkCoord>::failure("world_command.invalid_chunk_coord",
                                                 "chunk coord contains invalid numbers");
    }
    return core::Result<ChunkCoord>::success({x.value(), y.value(), z.value()});
}

[[nodiscard]] core::Result<VoxelCoord> parse_voxel_coord(std::string_view value) {
    const auto parts = split(value, '|');
    if (parts.size() != 3) {
        return core::Result<VoxelCoord>::failure("world_command.invalid_voxel_coord",
                                                 "voxel coord must contain x|y|z");
    }
    auto x = parse_u16(parts[0], "voxel_x");
    auto y = parse_u16(parts[1], "voxel_y");
    auto z = parse_u16(parts[2], "voxel_z");
    if (!x || !y || !z) {
        return core::Result<VoxelCoord>::failure("world_command.invalid_voxel_coord",
                                                 "voxel coord contains invalid numbers");
    }
    return core::Result<VoxelCoord>::success({x.value(), y.value(), z.value()});
}

[[nodiscard]] core::Result<workpieces::WorkpieceCellCoord>
parse_workpiece_cell_coord(std::string_view value) {
    const auto parts = split(value, '|');
    if (parts.size() != 3) {
        return core::Result<workpieces::WorkpieceCellCoord>::failure(
            "world_command.invalid_workpiece_coord", "workpiece coord must contain x|y|z");
    }
    auto x = parse_u16(parts[0], "workpiece_x");
    auto y = parse_u16(parts[1], "workpiece_y");
    auto z = parse_u16(parts[2], "workpiece_z");
    if (!x || !y || !z) {
        return core::Result<workpieces::WorkpieceCellCoord>::failure(
            "world_command.invalid_workpiece_coord", "workpiece coord contains invalid numbers");
    }
    return core::Result<workpieces::WorkpieceCellCoord>::success({x.value(), y.value(), z.value()});
}

[[nodiscard]] core::Result<workpieces::WorkpieceCell> parse_workpiece_cell(std::string_view value) {
    const auto parts = split(value, '|');
    if (parts.size() != 2 && parts.size() != 4) {
        return core::Result<workpieces::WorkpieceCell>::failure(
            "world_command.invalid_workpiece_cell",
            "workpiece cell must contain material|occupancy");
    }
    auto material = parse_u16(parts[0], "workpiece_material");
    auto occupancy = parse_u8(parts[1], "workpiece_occupancy");
    auto pattern = parts.size() == 4 ? parse_u16(parts[2], "workpiece_pattern")
                                     : core::Result<std::uint16_t>::success(0);
    auto flags = parts.size() == 4 ? parse_u8(parts[3], "workpiece_flags")
                                   : core::Result<std::uint8_t>::success(0);
    if (!material || !occupancy || !pattern || !flags) {
        return core::Result<workpieces::WorkpieceCell>::failure(
            "world_command.invalid_workpiece_cell", "workpiece cell contains invalid numbers");
    }
    return core::Result<workpieces::WorkpieceCell>::success(
        {material.value(), occupancy.value(), pattern.value(), flags.value()});
}

[[nodiscard]] core::Result<workpieces::WorkpieceOperationKind>
parse_workpiece_operation_kind(std::string_view value) {
    if (value == "add_cell") {
        return core::Result<workpieces::WorkpieceOperationKind>::success(
            workpieces::WorkpieceOperationKind::add_cell);
    }
    if (value == "remove_cell") {
        return core::Result<workpieces::WorkpieceOperationKind>::success(
            workpieces::WorkpieceOperationKind::remove_cell);
    }
    if (value == "set_cell") {
        return core::Result<workpieces::WorkpieceOperationKind>::success(
            workpieces::WorkpieceOperationKind::set_cell);
    }
    return core::Result<workpieces::WorkpieceOperationKind>::failure(
        "world_command.invalid_workpiece_operation",
        "workpiece operation must be add_cell, remove_cell, or set_cell");
}

[[nodiscard]] core::Result<build::Vec3> parse_vec3(std::string_view value, std::string_view name) {
    const auto parts = split(value, '|');
    if (parts.size() != 3) {
        return core::Result<build::Vec3>::failure("world_command.invalid_vec3",
                                                  std::string(name) + " must contain x|y|z");
    }
    auto x = parse_double(parts[0], "x");
    auto y = parse_double(parts[1], "y");
    auto z = parse_double(parts[2], "z");
    if (!x || !y || !z) {
        return core::Result<build::Vec3>::failure("world_command.invalid_vec3",
                                                  std::string(name) + " contains invalid numbers");
    }
    return core::Result<build::Vec3>::success({x.value(), y.value(), z.value()});
}

[[nodiscard]] core::Result<WorldPosition> parse_world_position(const net::CommandPayload& fields) {
    const auto* legacy = fields.find("position");
    const auto* anchor = fields.find("position_anchor");
    const auto* local = fields.find("position_local");
    if ((anchor == nullptr) != (local == nullptr)) {
        return core::Result<WorldPosition>::failure(
            "world_command.incomplete_position",
            "anchored positions require both position_anchor and position_local");
    }
    if (legacy != nullptr && anchor != nullptr) {
        return core::Result<WorldPosition>::failure(
            "world_command.ambiguous_position",
            "command cannot contain both legacy and anchored positions");
    }
    if (anchor != nullptr) {
        auto parsed_anchor = parse_chunk_coord(*anchor);
        auto parsed_local = parse_vec3(*local, "position_local");
        if (!parsed_anchor || !parsed_local) {
            return core::Result<WorldPosition>::failure(
                "world_command.invalid_position", "anchored position contains invalid fields");
        }
        return WorldPosition::from_anchor(
            {parsed_anchor.value().x, parsed_anchor.value().y, parsed_anchor.value().z},
            parsed_local.value());
    }
    if (legacy != nullptr) {
        auto parsed = parse_vec3(*legacy, "position");
        if (!parsed) {
            return core::Result<WorldPosition>::failure(parsed.error().code,
                                                        parsed.error().message);
        }
        return WorldPosition::from_legacy_global(parsed.value());
    }
    return core::Result<WorldPosition>::success({});
}

[[nodiscard]] core::Result<VoxelCommandPayload> parse_voxel_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields) {
        return core::Result<VoxelCommandPayload>::failure(fields.error().code,
                                                          fields.error().message);
    }
    const auto& decoded_fields = fields.value();
    auto chunk_value = decoded_fields.require("chunk");
    auto voxel_value = decoded_fields.require("voxel");
    auto prototype_value = decoded_fields.require("prototype");
    if (!chunk_value || !voxel_value || !prototype_value || decoded_fields.size() != 3) {
        return core::Result<VoxelCommandPayload>::failure(
            "command_payload.missing_required_key",
            "voxel command payload is missing a required key");
    }

    auto chunk = parse_chunk_coord(chunk_value.value());
    auto voxel = parse_voxel_coord(voxel_value.value());
    if (!chunk) {
        return core::Result<VoxelCommandPayload>::failure(chunk.error().code,
                                                          chunk.error().message);
    }
    if (!voxel) {
        return core::Result<VoxelCommandPayload>::failure(voxel.error().code,
                                                          voxel.error().message);
    }
    std::optional<core::PrototypeId> prototype_id;
    if (prototype_value.value() != "air") {
        prototype_id = core::PrototypeId::parse(prototype_value.value());
        if (!prototype_id)
            return core::Result<VoxelCommandPayload>::failure(
                "world_command.invalid_voxel_prototype",
                "voxel intent prototype must be a stable prototype id or air");
    }
    return core::Result<VoxelCommandPayload>::success(
        {chunk.value(), voxel.value(), std::move(prototype_id)});
}

[[nodiscard]] core::Result<BuildPieceCommandPayload>
parse_build_piece_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields) {
        return core::Result<BuildPieceCommandPayload>::failure(fields.error().code,
                                                               fields.error().message);
    }
    const auto& decoded_fields = fields.value();
    auto prototype_value = decoded_fields.require("prototype");
    if (!prototype_value) {
        return core::Result<BuildPieceCommandPayload>::failure(prototype_value.error().code,
                                                               prototype_value.error().message);
    }
    auto prototype_id = core::PrototypeId::parse(prototype_value.value());
    if (!prototype_id) {
        return core::Result<BuildPieceCommandPayload>::failure(
            "world_command.invalid_prototype", "build piece prototype id is invalid");
    }

    build::Transform transform;
    auto position = parse_world_position(decoded_fields);
    if (!position) {
        return core::Result<BuildPieceCommandPayload>::failure(position.error().code,
                                                               position.error().message);
    }
    transform.position = position.value();
    if (const auto* found = decoded_fields.find("rotation"); found != nullptr) {
        auto rotation = parse_vec3(*found, "rotation");
        if (!rotation) {
            return core::Result<BuildPieceCommandPayload>::failure(rotation.error().code,
                                                                   rotation.error().message);
        }
        transform.rotation_degrees = rotation.value();
    }
    if (const auto* found = decoded_fields.find("scale"); found != nullptr) {
        auto scale = parse_vec3(*found, "scale");
        if (!scale) {
            return core::Result<BuildPieceCommandPayload>::failure(scale.error().code,
                                                                   scale.error().message);
        }
        transform.scale = scale.value();
    }

    return core::Result<BuildPieceCommandPayload>::success({prototype_id.value(), transform});
}

[[nodiscard]] core::Result<BuildCompleteCommandPayload>
parse_build_complete_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields) {
        return core::Result<BuildCompleteCommandPayload>::failure(fields.error().code,
                                                                  fields.error().message);
    }
    auto object_value = fields.value().require("object");
    if (!object_value) {
        return core::Result<BuildCompleteCommandPayload>::failure(object_value.error().code,
                                                                  object_value.error().message);
    }

    auto object_id = parse_positive_u64(object_value.value(), "object");
    if (!object_id) {
        return core::Result<BuildCompleteCommandPayload>::failure(object_id.error().code,
                                                                  object_id.error().message);
    }
    return core::Result<BuildCompleteCommandPayload>::success(
        {core::SaveId::from_value(object_id.value())});
}

[[nodiscard]] core::Result<WorkpieceEditCommandPayload>
parse_workpiece_edit_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields) {
        return core::Result<WorkpieceEditCommandPayload>::failure(fields.error().code,
                                                                  fields.error().message);
    }
    const auto& decoded_fields = fields.value();
    auto workpiece_id_value = decoded_fields.require("workpiece_id");
    auto operation_value = decoded_fields.require("operation");
    auto coord_value = decoded_fields.require("coord");
    if (!workpiece_id_value || !operation_value || !coord_value) {
        return core::Result<WorkpieceEditCommandPayload>::failure(
            "command_payload.missing_required_key",
            "workpiece edit payload is missing a required key");
    }

    auto workpiece_id = parse_positive_u64(workpiece_id_value.value(), "workpiece_id");
    auto operation_kind = parse_workpiece_operation_kind(operation_value.value());
    auto coord = parse_workpiece_cell_coord(coord_value.value());
    if (!workpiece_id) {
        return core::Result<WorkpieceEditCommandPayload>::failure(workpiece_id.error().code,
                                                                  workpiece_id.error().message);
    }
    if (!operation_kind) {
        return core::Result<WorkpieceEditCommandPayload>::failure(operation_kind.error().code,
                                                                  operation_kind.error().message);
    }
    if (!coord) {
        return core::Result<WorkpieceEditCommandPayload>::failure(coord.error().code,
                                                                  coord.error().message);
    }

    workpieces::WorkpieceCell cell = workpieces::WorkpieceCell::empty();
    if (const auto* cell_value = decoded_fields.find("cell"); cell_value != nullptr) {
        auto parsed_cell = parse_workpiece_cell(*cell_value);
        if (!parsed_cell) {
            return core::Result<WorkpieceEditCommandPayload>::failure(parsed_cell.error().code,
                                                                      parsed_cell.error().message);
        }
        cell = parsed_cell.value();
    }

    if (operation_kind.value() == workpieces::WorkpieceOperationKind::add_cell &&
        !cell.is_occupied()) {
        return core::Result<WorkpieceEditCommandPayload>::failure(
            "world_command.missing_workpiece_cell", "add_cell requires an occupied cell payload");
    }

    return core::Result<WorkpieceEditCommandPayload>::success(
        {core::WorkpieceId::from_value(workpiece_id.value()),
         {operation_kind.value(), coord.value(), cell}});
}

[[nodiscard]] core::Result<WorkpieceFinishCommandPayload>
parse_workpiece_finish_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields) {
        return core::Result<WorkpieceFinishCommandPayload>::failure(fields.error().code,
                                                                    fields.error().message);
    }
    auto workpiece_value = fields.value().require("workpiece_id");
    if (!workpiece_value) {
        return core::Result<WorkpieceFinishCommandPayload>::failure(
            workpiece_value.error().code, workpiece_value.error().message);
    }
    auto workpiece_id = parse_positive_u64(workpiece_value.value(), "workpiece_id");
    if (!workpiece_id) {
        return core::Result<WorkpieceFinishCommandPayload>::failure(workpiece_id.error().code,
                                                                    workpiece_id.error().message);
    }
    std::optional<core::PrototypeId> requested_pattern;
    if (const auto* value = fields.value().find("pattern"); value != nullptr) {
        auto parsed = core::PrototypeId::parse(*value);
        if (!parsed) {
            return core::Result<WorkpieceFinishCommandPayload>::failure(
                "world_command.invalid_workpiece_pattern",
                "workpiece finish pattern is not a valid prototype id");
        }
        requested_pattern = parsed.value();
    }
    return core::Result<WorkpieceFinishCommandPayload>::success(
        {core::WorkpieceId::from_value(workpiece_id.value()), std::move(requested_pattern)});
}

[[nodiscard]] core::Result<InventoryTransferCommandPayload>
parse_inventory_transfer_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields) {
        return core::Result<InventoryTransferCommandPayload>::failure(fields.error().code,
                                                                      fields.error().message);
    }
    const auto& decoded_fields = fields.value();
    auto source_owner_value = decoded_fields.require("source_owner");
    auto destination_owner_value = decoded_fields.require("destination_owner");
    auto source_slot_value = decoded_fields.require("source_slot");
    auto destination_slot_value = decoded_fields.require("destination_slot");
    auto count_value = decoded_fields.require("count");
    if (!source_owner_value || !destination_owner_value || !source_slot_value ||
        !destination_slot_value || !count_value) {
        return core::Result<InventoryTransferCommandPayload>::failure(
            "command_payload.missing_required_key",
            "inventory transfer payload is missing a required key");
    }

    auto source_owner = parse_positive_u64(source_owner_value.value(), "source_owner");
    auto destination_owner =
        parse_positive_u64(destination_owner_value.value(), "destination_owner");
    auto source_slot = parse_size(source_slot_value.value(), "source_slot");
    auto destination_slot = parse_size(destination_slot_value.value(), "destination_slot");
    auto count = parse_u32(count_value.value(), "count");
    if (!source_owner) {
        return core::Result<InventoryTransferCommandPayload>::failure(source_owner.error().code,
                                                                      source_owner.error().message);
    }
    if (!destination_owner) {
        return core::Result<InventoryTransferCommandPayload>::failure(
            destination_owner.error().code, destination_owner.error().message);
    }
    if (!source_slot) {
        return core::Result<InventoryTransferCommandPayload>::failure(source_slot.error().code,
                                                                      source_slot.error().message);
    }
    if (!destination_slot) {
        return core::Result<InventoryTransferCommandPayload>::failure(
            destination_slot.error().code, destination_slot.error().message);
    }
    if (!count) {
        return core::Result<InventoryTransferCommandPayload>::failure(count.error().code,
                                                                      count.error().message);
    }

    return core::Result<InventoryTransferCommandPayload>::success(
        {{core::SaveId::from_value(source_owner.value()),
          core::SaveId::from_value(destination_owner.value()), source_slot.value(),
          destination_slot.value(), count.value()}});
}

[[nodiscard]] core::Result<ProcessStartCommandPayload>
parse_process_start_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields) {
        return core::Result<ProcessStartCommandPayload>::failure(fields.error().code,
                                                                 fields.error().message);
    }
    const auto& decoded_fields = fields.value();
    auto owner_value = decoded_fields.require("owner");
    auto prototype_value = decoded_fields.require("prototype");
    if (!owner_value || !prototype_value) {
        return core::Result<ProcessStartCommandPayload>::failure(
            "command_payload.missing_required_key",
            "process start payload is missing a required key");
    }

    auto owner = parse_positive_u64(owner_value.value(), "owner");
    if (!owner) {
        return core::Result<ProcessStartCommandPayload>::failure(owner.error().code,
                                                                 owner.error().message);
    }
    auto prototype_id = core::PrototypeId::parse(prototype_value.value());
    if (!prototype_id) {
        return core::Result<ProcessStartCommandPayload>::failure("world_command.invalid_prototype",
                                                                 "process prototype id is invalid");
    }

    return core::Result<ProcessStartCommandPayload>::success(
        {core::SaveId::from_value(owner.value()), prototype_id.value()});
}

[[nodiscard]] core::Result<ProcessAdvanceAllCommandPayload>
parse_process_advance_all_payload(std::string_view payload) {
    if (payload.empty()) {
        return core::Result<ProcessAdvanceAllCommandPayload>::success({});
    }

    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields) {
        return core::Result<ProcessAdvanceAllCommandPayload>::failure(fields.error().code,
                                                                      fields.error().message);
    }
    return core::Result<ProcessAdvanceAllCommandPayload>::failure(
        "world_command.untrusted_process_modifiers",
        "process advance modifiers are resolved from authoritative world state");
}

[[nodiscard]] core::Result<SleepCommandPayload> parse_sleep_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields) {
        return core::Result<SleepCommandPayload>::failure(fields.error().code,
                                                          fields.error().message);
    }
    auto encoded_hours = fields.value().require("hours");
    if (!encoded_hours) {
        return core::Result<SleepCommandPayload>::failure(encoded_hours.error().code,
                                                          encoded_hours.error().message);
    }
    auto hours = parse_u64(encoded_hours.value(), "sleep_hours");
    if (!hours || hours.value() == 0 || hours.value() > 24) {
        return core::Result<SleepCommandPayload>::failure("world_command.invalid_sleep_hours",
                                                          "sleep time skip must be in 1..24 hours");
    }
    return core::Result<SleepCommandPayload>::success({hours.value()});
}

[[nodiscard]] core::Result<CargoCreateCommandPayload>
parse_cargo_create_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields) {
        return core::Result<CargoCreateCommandPayload>::failure(fields.error().code,
                                                                fields.error().message);
    }
    auto prototype_value = fields.value().require("prototype");
    if (!prototype_value) {
        return core::Result<CargoCreateCommandPayload>::failure(prototype_value.error().code,
                                                                prototype_value.error().message);
    }

    auto prototype_id = core::PrototypeId::parse(prototype_value.value());
    if (!prototype_id) {
        return core::Result<CargoCreateCommandPayload>::failure("world_command.invalid_prototype",
                                                                "cargo prototype id is invalid");
    }

    auto position = parse_world_position(fields.value());
    if (!position || !position.value().is_valid()) {
        return core::Result<CargoCreateCommandPayload>::failure("world_command.invalid_position",
                                                                "cargo position must be finite");
    }

    return core::Result<CargoCreateCommandPayload>::success(
        {prototype_id.value(), position.value()});
}

[[nodiscard]] core::Result<EntitySpawnCommandPayload>
parse_entity_spawn_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields) {
        return core::Result<EntitySpawnCommandPayload>::failure(fields.error().code,
                                                                fields.error().message);
    }
    const auto& decoded_fields = fields.value();
    auto prototype_value = decoded_fields.require("prototype");
    if (!prototype_value) {
        return core::Result<EntitySpawnCommandPayload>::failure(prototype_value.error().code,
                                                                prototype_value.error().message);
    }

    auto prototype_id = core::PrototypeId::parse(prototype_value.value());
    if (!prototype_id) {
        return core::Result<EntitySpawnCommandPayload>::failure("world_command.invalid_prototype",
                                                                "entity prototype id is invalid");
    }

    entities::Transform transform;
    auto position = parse_world_position(decoded_fields);
    if (!position) {
        return core::Result<EntitySpawnCommandPayload>::failure(position.error().code,
                                                                position.error().message);
    }
    transform.position = position.value();
    if (const auto* found = decoded_fields.find("rotation"); found != nullptr) {
        auto rotation = parse_vec3(*found, "rotation");
        if (!rotation) {
            return core::Result<EntitySpawnCommandPayload>::failure(rotation.error().code,
                                                                    rotation.error().message);
        }
        transform.rotation_degrees = rotation.value();
    }
    if (const auto* found = decoded_fields.find("scale"); found != nullptr) {
        auto scale = parse_vec3(*found, "scale");
        if (!scale) {
            return core::Result<EntitySpawnCommandPayload>::failure(scale.error().code,
                                                                    scale.error().message);
        }
        transform.scale = scale.value();
    }
    if (!transform.is_finite()) {
        return core::Result<EntitySpawnCommandPayload>::failure(
            "world_command.invalid_transform", "entity transform must contain finite values");
    }
    if (!transform.has_non_zero_scale()) {
        return core::Result<EntitySpawnCommandPayload>::failure(
            "world_command.invalid_transform_scale", "entity transform scale must be non-zero");
    }

    return core::Result<EntitySpawnCommandPayload>::success({prototype_id.value(), transform});
}

[[nodiscard]] core::Result<std::vector<AssemblyPartCommandPayload>>
parse_assembly_parts(std::string_view value) {
    std::vector<AssemblyPartCommandPayload> parts;
    if (value.empty()) {
        return core::Result<std::vector<AssemblyPartCommandPayload>>::failure(
            "world_command.missing_assembly_parts", "assembly parts payload must not be empty");
    }

    for (const auto entry : split(value, ',')) {
        const auto separator = entry.find(':');
        if (separator == std::string_view::npos) {
            return core::Result<std::vector<AssemblyPartCommandPayload>>::failure(
                "world_command.invalid_assembly_part",
                "assembly part entries must use name:build_piece_id syntax");
        }
        const auto name = entry.substr(0, separator);
        const auto build_id_text = entry.substr(separator + 1);
        if (!core::is_valid_local_id(name)) {
            return core::Result<std::vector<AssemblyPartCommandPayload>>::failure(
                "world_command.invalid_assembly_part_name",
                "assembly part name is invalid: " + std::string(name));
        }
        auto build_id = parse_positive_u64(build_id_text, "assembly_part_build_id");
        if (!build_id) {
            return core::Result<std::vector<AssemblyPartCommandPayload>>::failure(
                build_id.error().code, build_id.error().message);
        }
        parts.push_back({std::string(name), core::SaveId::from_value(build_id.value())});
    }
    return core::Result<std::vector<AssemblyPartCommandPayload>>::success(std::move(parts));
}

[[nodiscard]] core::Result<AssemblyCreateCommandPayload>
parse_assembly_create_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields) {
        return core::Result<AssemblyCreateCommandPayload>::failure(fields.error().code,
                                                                   fields.error().message);
    }
    const auto& decoded_fields = fields.value();
    auto prototype_value = decoded_fields.require("prototype");
    auto root_value = decoded_fields.require("root");
    auto parts_value = decoded_fields.require("parts");
    if (!prototype_value || !root_value || !parts_value) {
        return core::Result<AssemblyCreateCommandPayload>::failure(
            "command_payload.missing_required_key",
            "assembly create payload is missing a required key");
    }

    auto prototype_id = core::PrototypeId::parse(prototype_value.value());
    if (!prototype_id) {
        return core::Result<AssemblyCreateCommandPayload>::failure(
            "world_command.invalid_prototype", "assembly prototype id is invalid");
    }
    auto root_id = parse_positive_u64(root_value.value(), "root");
    if (!root_id) {
        return core::Result<AssemblyCreateCommandPayload>::failure(root_id.error().code,
                                                                   root_id.error().message);
    }
    auto parts = parse_assembly_parts(parts_value.value());
    if (!parts) {
        return core::Result<AssemblyCreateCommandPayload>::failure(parts.error().code,
                                                                   parts.error().message);
    }

    return core::Result<AssemblyCreateCommandPayload>::success(
        {prototype_id.value(), core::SaveId::from_value(root_id.value()),
         std::move(parts).value()});
}

[[nodiscard]] core::Result<AssemblyBlueprintCommandPayload>
parse_assembly_blueprint_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields)
        return core::Result<AssemblyBlueprintCommandPayload>::failure(fields.error().code,
                                                                      fields.error().message);
    auto prototype = fields.value().require("prototype");
    auto root = fields.value().require("root");
    if (!prototype || !root)
        return core::Result<AssemblyBlueprintCommandPayload>::failure(
            "command_payload.missing_required_key",
            "assembly blueprint requires prototype and root");
    auto prototype_id = core::PrototypeId::parse(prototype.value());
    auto root_id = parse_positive_u64(root.value(), "root");
    if (!prototype_id || !root_id)
        return core::Result<AssemblyBlueprintCommandPayload>::failure(
            "world_command.invalid_assembly_blueprint", "assembly blueprint contains invalid ids");
    return core::Result<AssemblyBlueprintCommandPayload>::success(
        {prototype_id.value(), core::SaveId::from_value(root_id.value())});
}

[[nodiscard]] core::Result<AssemblyPlacePartCommandPayload>
parse_assembly_place_part_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields)
        return core::Result<AssemblyPlacePartCommandPayload>::failure(fields.error().code,
                                                                      fields.error().message);
    auto assembly = fields.value().require("assembly");
    auto name = fields.value().require("name");
    auto build_piece = fields.value().require("build_piece");
    if (!assembly || !name || !build_piece || !core::is_valid_local_id(name.value()))
        return core::Result<AssemblyPlacePartCommandPayload>::failure(
            "world_command.invalid_assembly_part", "assembly part placement fields are invalid");
    auto assembly_id = parse_positive_u64(assembly.value(), "assembly");
    auto build_id = parse_positive_u64(build_piece.value(), "build_piece");
    if (!assembly_id || !build_id)
        return core::Result<AssemblyPlacePartCommandPayload>::failure(
            "world_command.invalid_assembly_part", "assembly part placement ids are invalid");
    return core::Result<AssemblyPlacePartCommandPayload>::success(
        {core::SaveId::from_value(assembly_id.value()), std::string(name.value()),
         core::SaveId::from_value(build_id.value())});
}

[[nodiscard]] core::Result<AssemblyAdvanceCommandPayload>
parse_assembly_advance_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields)
        return core::Result<AssemblyAdvanceCommandPayload>::failure(fields.error().code,
                                                                    fields.error().message);
    auto assembly = fields.value().require("assembly");
    if (!assembly)
        return core::Result<AssemblyAdvanceCommandPayload>::failure(assembly.error().code,
                                                                    assembly.error().message);
    auto parsed = parse_positive_u64(assembly.value(), "assembly");
    if (!parsed)
        return core::Result<AssemblyAdvanceCommandPayload>::failure(parsed.error().code,
                                                                    parsed.error().message);
    return core::Result<AssemblyAdvanceCommandPayload>::success(
        {core::SaveId::from_value(parsed.value())});
}

[[nodiscard]] core::Result<AssemblyTransitionCommandPayload>
parse_assembly_transition_payload(std::string_view payload) {
    auto fields = net::CommandPayloadTextCodec::decode(payload);
    if (!fields)
        return core::Result<AssemblyTransitionCommandPayload>::failure(fields.error().code,
                                                                       fields.error().message);
    auto assembly = fields.value().require("assembly");
    auto state = fields.value().require("state");
    if (!assembly || !state)
        return core::Result<AssemblyTransitionCommandPayload>::failure(
            "command_payload.missing_required_key",
            "assembly transition requires assembly and state");
    auto assembly_id = parse_positive_u64(assembly.value(), "assembly");
    auto parsed_state = assemblies::parse_assembly_state(state.value());
    if (!assembly_id || !parsed_state)
        return core::Result<AssemblyTransitionCommandPayload>::failure(
            "world_command.invalid_assembly_transition", "assembly transition fields are invalid");
    const auto* reason = fields.value().find("reason");
    return core::Result<AssemblyTransitionCommandPayload>::success(
        {core::SaveId::from_value(assembly_id.value()), parsed_state.value(),
         reason != nullptr ? *reason : std::string{}});
}

[[nodiscard]] core::Status
require_authoritative_world(const net::CommandExecutionContext& context) {
    if (context.world_state == nullptr) {
        return core::Status::failure("world_command.missing_world_state",
                                     "command requires authoritative world state");
    }
    return core::Status::ok();
}

[[nodiscard]] dirty::DirtyRegionBounds
build_piece_dirty_bounds(const build::BuildPieceRecord& record) noexcept {
    const dirty::DirtyRegionCoord coord{record.transform.position.anchor.x,
                                        record.transform.position.anchor.y,
                                        record.transform.position.anchor.z};
    return dirty::DirtyRegionBounds::single(coord);
}

[[nodiscard]] core::Status mark_build_piece_derived_dirty(WorldState& state,
                                                          const build::BuildPieceRecord& record) {
    auto bounds = build_piece_dirty_bounds(record).expanded(1);
    if (!bounds) {
        return core::Status::failure(bounds.error().code, bounds.error().message);
    }

    auto status = state.dirty_regions().mark(dirty::DirtyRegionKind::room_graph, bounds.value(),
                                             "build piece placed " + record.prototype_id.value());
    if (!status) {
        return status;
    }

    for (const auto& port : record.network_ports) {
        auto& network = state.networks().get_or_create(port.kind);
        status = network.mark_dirty_region(state.dirty_regions(), bounds.value(),
                                           "build piece port placed " + port.name);
        if (!status) {
            return status;
        }
    }

    return core::Status::ok();
}

[[nodiscard]] const build::BuildNetworkPort*
find_build_piece_port(const build::BuildPieceRecord& record, const assemblies::AssemblyPort& port) {
    for (const auto& build_port : record.network_ports) {
        if (build_port.name == port.name && build_port.kind == port.kind) {
            return &build_port;
        }
    }
    return nullptr;
}

[[nodiscard]] core::Result<assemblies::AssemblyRecord>
build_assembly_record(WorldState& state, const AssemblyCreateCommandPayload& payload,
                      core::SaveId assembly_id, const assemblies::AssemblyDefinition& definition) {
    auto* root = state.build_objects().find(payload.root_build_piece_id);
    if (root == nullptr) {
        return core::Result<assemblies::AssemblyRecord>::failure(
            "world_command.missing_assembly_root",
            "assembly root build piece is not present in world state");
    }
    if (root->construction_state != build::ConstructionState::complete) {
        return core::Result<assemblies::AssemblyRecord>::failure(
            "world_command.incomplete_assembly_root", "assembly root build piece must be complete");
    }

    assemblies::AssemblyRecord record;
    record.assembly_id = assembly_id;
    record.root_build_piece_id = payload.root_build_piece_id;
    record.prototype_id = payload.prototype_id;
    record.root_coord = root->transform.position.anchor;
    record.state = assemblies::AssemblyState::ready;
    record.capabilities = definition.capabilities;
    record.ports.reserve(definition.required_ports.size());
    record.parts.reserve(payload.parts.size());

    std::vector<const build::BuildPieceRecord*> build_parts;
    build_parts.reserve(payload.parts.size() + 1);
    build_parts.push_back(root);
    for (const auto& part_payload : payload.parts) {
        auto* part = state.build_objects().find(part_payload.build_piece_id);
        if (part == nullptr) {
            return core::Result<assemblies::AssemblyRecord>::failure(
                "world_command.missing_assembly_part",
                "assembly part build piece is not present in world state: " + part_payload.name);
        }
        if (part->construction_state != build::ConstructionState::complete) {
            return core::Result<assemblies::AssemblyRecord>::failure(
                "world_command.incomplete_assembly_part",
                "assembly part build piece must be complete: " + part_payload.name);
        }
        const auto requirement =
            std::ranges::find_if(definition.part_requirements, [&part_payload](const auto& value) {
                return value.name == part_payload.name;
            });
        if (requirement == definition.part_requirements.end())
            return core::Result<assemblies::AssemblyRecord>::failure(
                "world_command.unknown_assembly_part", "assembly part name is not in its layout");
        const auto expected_anchor =
            checked_block_coord_offset(record.root_coord, requirement->relative_coord);
        if (!expected_anchor || part->transform.position.anchor != expected_anchor.value())
            return core::Result<assemblies::AssemblyRecord>::failure(
                "world_command.assembly_part_position_mismatch",
                "assembly part does not occupy its prototype-defined relative coordinate");
        record.parts.push_back({part_payload.name, part_payload.build_piece_id, part->prototype_id,
                                requirement->relative_coord});
        build_parts.push_back(part);
    }

    for (const auto& required_port : definition.required_ports) {
        for (const auto* part : build_parts) {
            const auto* build_port = find_build_piece_port(*part, required_port);
            if (build_port != nullptr) {
                record.ports.push_back({required_port.name, required_port.kind, part->object_id,
                                        build_port->capacity, required_port.relative_coord});
                break;
            }
        }
    }

    return core::Result<assemblies::AssemblyRecord>::success(std::move(record));
}

[[nodiscard]] core::Status mark_assembly_derived_dirty(WorldState& state,
                                                       const assemblies::AssemblyRecord& record) {
    const auto* root = state.build_objects().find(record.root_build_piece_id);
    if (root == nullptr) {
        return core::Status::failure("world_command.missing_assembly_root",
                                     "assembly root build piece is not present in world state");
    }

    auto bounds = build_piece_dirty_bounds(*root).expanded(1);
    if (!bounds) {
        return core::Status::failure(bounds.error().code, bounds.error().message);
    }

    auto status = state.dirty_regions().mark(dirty::DirtyRegionKind::room_graph, bounds.value(),
                                             "assembly created " + record.prototype_id.value());
    if (!status) {
        return status;
    }

    for (const auto& port : record.ports) {
        auto& network = state.networks().get_or_create(port.kind);
        status = network.mark_dirty_region(state.dirty_regions(), bounds.value(),
                                           "assembly port created " + port.name);
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status rebuild_spatial_networks(WorldState& state) {
    auto rebuilt = state.networks().rebuild_from_ports(state.build_objects(), state.assemblies());
    if (!rebuilt) {
        return core::Status::failure(rebuilt.error().code, rebuilt.error().message);
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status handle_set_voxel(const net::CommandEnvelope& envelope,
                                            const net::CommandExecutionContext& context,
                                            WorldOperation& operation) {
    auto world_status = require_authoritative_world(context);
    if (!world_status) {
        return world_status;
    }
    auto payload = parse_voxel_payload(envelope.payload);
    if (!payload) {
        return core::Status::failure(payload.error().code, payload.error().message);
    }
    if (!context.world_state->chunks().contains(payload.value().chunk)) {
        return core::Status::failure(
            "world_command.chunk_not_loaded",
            "authoritative voxel edits require the target chunk to be loaded first");
    }
    if (context.voxel_palette == nullptr)
        return core::Status::failure("world_command.missing_voxel_palette",
                                     "stable voxel intents require the authoritative palette");

    const auto* chunk = context.world_state->chunks().find(payload.value().chunk);
    auto previous = chunk->get(payload.value().voxel);
    if (!previous) {
        return core::Status::failure(previous.error().code, previous.error().message);
    }
    auto position = chunk_local_to_block(payload.value().chunk, payload.value().voxel);
    if (!position) {
        return core::Status::failure(position.error().code, position.error().message);
    }

    VoxelCell cell = VoxelCell::air();
    if (payload.value().prototype_id.has_value()) {
        const auto* definition =
            context.voxel_palette->find_by_prototype(*payload.value().prototype_id);
        if (definition == nullptr)
            return core::Status::failure("world_command.unknown_voxel_prototype",
                                         "voxel prototype is not present in the resolved palette");
        if (definition->metadata_required)
            return core::Status::failure(
                "world_command.specialized_placement_required",
                "metadata-backed blocks require a specialized authoritative placement command");
        cell = {definition->type, definition->light_emission, 0, 0};
    }
    const auto collision_changed =
        !context.voxel_palette->same_collision_geometry(previous.value(), cell);
    const auto lighting_changed =
        !context.voxel_palette->same_lighting_behavior(previous.value(), cell);
    const auto fluid_changed =
        !context.voxel_palette->same_fluid_simulation_behavior(previous.value(), cell);

    auto status = context.world_state->chunks().set(payload.value().chunk, payload.value().voxel,
                                                    cell, context.world_state->dirty_regions(),
                                                    *context.voxel_palette);
    if (!status) {
        return status;
    }

    chunk = context.world_state->chunks().find(payload.value().chunk);
    if (chunk == nullptr) {
        return core::Status::failure("world_command.chunk_not_loaded",
                                     "voxel edit removed its resident chunk unexpectedly");
    }
    auto current = chunk->get(payload.value().voxel);
    if (!current) {
        return core::Status::failure(current.error().code, current.error().message);
    }
    const VoxelChangeRecord change{position.value(), previous.value(), current.value(),
                                   chunk->identity(), chunk->content_revision()};
    status = change.validate();
    if (!status) {
        return status;
    }

    status = operation.record_mutation("set terrain voxel");
    if (!status) {
        return status;
    }
    operation.record_derived_update("chunk_mesh");
    if (collision_changed) {
        operation.record_derived_update("chunk_collision");
    }
    if (lighting_changed) {
        operation.record_derived_update("chunk_lighting");
    }
    if (fluid_changed) {
        operation.record_derived_update("voxel_fluids");
    }
    operation.emit_event({std::string(voxel_changed_event_type),
                          {},
                          VoxelChangeTextCodec::encode(change),
                          payload.value().chunk});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status handle_place_build_piece(const net::CommandEnvelope& envelope,
                                                    const net::CommandExecutionContext& context,
                                                    WorldOperation& operation) {
    auto world_status = require_authoritative_world(context);
    if (!world_status) {
        return world_status;
    }
    if (context.prototypes == nullptr) {
        return core::Status::failure("world_command.missing_prototypes",
                                     "build placement requires prototype registry");
    }

    auto payload = parse_build_piece_payload(envelope.payload);
    if (!payload) {
        return core::Status::failure(payload.error().code, payload.error().message);
    }

    auto prototype_status = context.prototypes->require_kind(payload.value().prototype_id,
                                                             modding::PrototypeKinds::build_piece);
    if (!prototype_status) {
        return prototype_status;
    }
    const auto* prototype = context.prototypes->find(payload.value().prototype_id);
    if (prototype == nullptr) {
        return core::Status::failure("world_command.missing_prototype",
                                     "build piece prototype is missing");
    }

    auto reserved_id = operation.reserve_save_id(context.world_state->save_ids());
    if (!reserved_id) {
        return core::Status::failure(reserved_id.error().code, reserved_id.error().message);
    }

    auto record = build::build_piece_record_from_prototype(*prototype, reserved_id.value(),
                                                           payload.value().transform);
    if (!record) {
        return core::Status::failure(record.error().code, record.error().message);
    }

    auto status = context.world_state->build_objects().insert(record.value());
    if (!status) {
        return status;
    }
    status = mark_build_piece_derived_dirty(*context.world_state, record.value());
    if (!status) {
        return status;
    }
    status = operation.record_mutation("place build piece " + payload.value().prototype_id.value());
    if (!status) {
        return status;
    }
    operation.record_derived_update("RoomGraph");
    operation.record_derived_update("SpatialNetworks");
    operation.emit_event(
        {"build_piece.placed", reserved_id.value(), payload.value().prototype_id.value()});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status handle_complete_build_piece(const net::CommandEnvelope& envelope,
                                                       const net::CommandExecutionContext& context,
                                                       WorldOperation& operation) {
    auto world_status = require_authoritative_world(context);
    if (!world_status) {
        return world_status;
    }

    auto payload = parse_build_complete_payload(envelope.payload);
    if (!payload) {
        return core::Status::failure(payload.error().code, payload.error().message);
    }

    auto* record = context.world_state->build_objects().find(payload.value().object_id);
    if (record == nullptr) {
        return core::Status::failure("world_command.missing_build_piece",
                                     "build piece is not present in world state");
    }
    if (record->construction_state == build::ConstructionState::complete) {
        return core::Status::failure("world_command.build_piece_already_complete",
                                     "build piece construction is already complete");
    }

    record->construction_state = build::ConstructionState::complete;
    auto status = mark_build_piece_derived_dirty(*context.world_state, *record);
    if (!status) {
        return status;
    }
    status = rebuild_spatial_networks(*context.world_state);
    if (!status) {
        return status;
    }
    status = operation.record_mutation("complete build piece " + record->object_id.to_string());
    if (!status) {
        return status;
    }
    operation.record_derived_update("RoomGraph");
    operation.record_derived_update("SpatialNetworks");
    operation.emit_event(
        {"build_piece.completed", record->object_id, record->prototype_id.value()});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status handle_edit_workpiece(const net::CommandEnvelope& envelope,
                                                 const net::CommandExecutionContext& context,
                                                 WorldOperation& operation) {
    auto world_status = require_authoritative_world(context);
    if (!world_status) {
        return world_status;
    }
    auto payload = parse_workpiece_edit_payload(envelope.payload);
    if (!payload) {
        return core::Status::failure(payload.error().code, payload.error().message);
    }

    auto* record = context.world_state->workpieces().find(payload.value().workpiece_id);
    if (record == nullptr) {
        return core::Status::failure("world_command.missing_workpiece",
                                     "workpiece id is not present in world state");
    }
    if (record->committed) {
        return core::Status::failure("world_command.workpiece_committed",
                                     "committed workpieces cannot be edited");
    }
    if (!record->owner_session.is_valid()) {
        return core::Status::failure(
            "world_command.workpiece_owner_unbound",
            "workpiece ownership must be rebound to a stable player before editing");
    }
    if (record->owner_session != envelope.sender) {
        return core::Status::failure("world_command.workpiece_not_owned",
                                     "workpiece edit sender does not own the active session");
    }
    if (record->server_state.has_value()) {
        const auto shape = record->grid.shape();
        const auto index =
            static_cast<std::size_t>(payload.value().operation.coord.z) * shape.width *
                shape.height +
            static_cast<std::size_t>(payload.value().operation.coord.y) * shape.width +
            payload.value().operation.coord.x;
        if (!record->server_state->in_blob(index)) {
            return core::Status::failure("world_command.workpiece_outside_blob",
                                         "workpiece edit targets a cell outside its server blob");
        }
    }

    auto status = record->grid.apply(payload.value().operation);
    if (!status) {
        return status;
    }
    if (record->server_state.has_value()) {
        const auto shape = record->grid.shape();
        const auto index =
            static_cast<std::size_t>(payload.value().operation.coord.z) * shape.width *
                shape.height +
            static_cast<std::size_t>(payload.value().operation.coord.y) * shape.width +
            payload.value().operation.coord.x;
        auto revealed = record->server_state->reveal(index);
        if (!revealed)
            return core::Status::failure(revealed.error().code, revealed.error().message);
    }
    ++record->revision;
    status =
        operation.record_mutation("edit workpiece " + payload.value().workpiece_id.to_string());
    if (!status) {
        return status;
    }
    operation.record_derived_update("WorkpieceMesh");
    operation.emit_event({"workpiece.edited",
                          core::SaveId::from_value(payload.value().workpiece_id.value()),
                          envelope.payload});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status handle_finish_workpiece(const net::CommandEnvelope& envelope,
                                                   const net::CommandExecutionContext& context,
                                                   WorldOperation& operation) {
    auto world_status = require_authoritative_world(context);
    if (!world_status)
        return world_status;
    if (context.prototypes == nullptr) {
        return core::Status::failure("world_command.missing_prototypes",
                                     "workpiece finishing requires the prototype registry");
    }
    auto payload = parse_workpiece_finish_payload(envelope.payload);
    if (!payload)
        return core::Status::failure(payload.error().code, payload.error().message);

    auto* record = context.world_state->workpieces().find(payload.value().workpiece_id);
    if (record == nullptr) {
        return core::Status::failure("world_command.missing_workpiece",
                                     "workpiece id is not present in world state");
    }
    if (record->committed) {
        return core::Status::failure("world_command.workpiece_committed",
                                     "workpiece is already committed");
    }
    if (!record->owner_session.is_valid()) {
        return core::Status::failure(
            "world_command.workpiece_owner_unbound",
            "workpiece ownership must be rebound to a stable player before finishing");
    }
    if (record->owner_session != envelope.sender) {
        return core::Status::failure("world_command.workpiece_not_owned",
                                     "workpiece finish sender does not own the active session");
    }
    auto library = workpieces::pattern_library_from_prototypes(*context.prototypes);
    if (!library)
        return core::Status::failure(library.error().code, library.error().message);
    auto result = workpieces::finish_workpiece(
        record->grid, record->server_state ? &*record->server_state : nullptr, library.value(),
        record->material_prototype_id, payload.value().requested_pattern);
    if (!result)
        return core::Status::failure(result.error().code, result.error().message);

    if (record->server_state.has_value()) {
        record->server_state->output_metadata = result.value().metadata;
    }
    record->committed = true;
    ++record->revision;
    auto status = operation.record_mutation("finish workpiece " + record->workpiece_id.to_string());
    if (!status)
        return status;
    operation.record_derived_update("WorkpieceOutput");
    operation.emit_event({"workpiece.finished",
                          core::SaveId::from_value(record->workpiece_id.value()),
                          result.value().output_prototype_id.value() +
                              "|pattern=" + result.value().pattern_id.value() +
                              "|mass=" + std::to_string(result.value().metadata.mass_units) +
                              "|byproduct=" + std::to_string(result.value().byproduct_units)});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status
handle_transfer_inventory_items(const net::CommandEnvelope& envelope,
                                const net::CommandExecutionContext& context,
                                WorldOperation& operation) {
    auto world_status = require_authoritative_world(context);
    if (!world_status) {
        return world_status;
    }
    auto payload = parse_inventory_transfer_payload(envelope.payload);
    if (!payload) {
        return core::Status::failure(payload.error().code, payload.error().message);
    }

    auto* source = context.world_state->inventories().find(payload.value().request.source_owner_id);
    if (source == nullptr) {
        return core::Status::failure("world_command.missing_source_inventory",
                                     "source inventory owner is not present in world state");
    }
    auto* destination =
        context.world_state->inventories().find(payload.value().request.destination_owner_id);
    if (destination == nullptr) {
        return core::Status::failure("world_command.missing_destination_inventory",
                                     "destination inventory owner is not present in world state");
    }

    auto status = transfer_inventory_items(*source, *destination, payload.value().request);
    if (!status) {
        return status;
    }
    status = operation.record_mutation("transfer inventory items " +
                                       payload.value().request.source_owner_id.to_string() + "->" +
                                       payload.value().request.destination_owner_id.to_string());
    if (!status) {
        return status;
    }
    operation.record_derived_update("Inventory");
    operation.emit_event(
        {"inventory.items_transferred", payload.value().request.source_owner_id, envelope.payload});
    if (payload.value().request.destination_owner_id != payload.value().request.source_owner_id) {
        operation.emit_event({"inventory.items_transferred",
                              payload.value().request.destination_owner_id, envelope.payload});
    }
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status handle_start_process(const net::CommandEnvelope& envelope,
                                                const net::CommandExecutionContext& context,
                                                WorldOperation& operation) {
    auto world_status = require_authoritative_world(context);
    if (!world_status) {
        return world_status;
    }
    if (context.prototypes == nullptr) {
        return core::Status::failure("world_command.missing_prototypes",
                                     "process start requires prototype registry");
    }

    auto payload = parse_process_start_payload(envelope.payload);
    if (!payload) {
        return core::Status::failure(payload.error().code, payload.error().message);
    }
    if (!context.world_state->contains_saved_object(payload.value().owner_id)) {
        return core::Status::failure("world_command.missing_process_owner",
                                     "process owner is not present in world state");
    }

    auto prototype_status = context.prototypes->require_kind(payload.value().prototype_id,
                                                             modding::PrototypeKinds::process);
    if (!prototype_status) {
        return prototype_status;
    }
    const auto* prototype = context.prototypes->find(payload.value().prototype_id);
    if (prototype == nullptr) {
        return core::Status::failure("world_command.missing_prototype",
                                     "process prototype is missing");
    }

    auto definition = processes::process_definition_from_prototype(*prototype);
    if (!definition) {
        return core::Status::failure(definition.error().code, definition.error().message);
    }

    auto process_id = context.world_state->process_ids().reserve();
    if (!process_id) {
        return core::Status::failure(process_id.error().code, process_id.error().message);
    }
    auto instance =
        processes::ProcessRuntime::create(process_id.value(), payload.value().owner_id,
                                          definition.value(), context.world_state->world_time());
    if (!instance) {
        return core::Status::failure(instance.error().code, instance.error().message);
    }

    auto status = context.world_state->processes().insert(std::move(instance).value());
    if (!status) {
        return status;
    }
    status = operation.record_mutation("start process " + payload.value().prototype_id.value());
    if (!status) {
        return status;
    }
    operation.record_derived_update("Processes");
    operation.emit_event({"process.started", payload.value().owner_id, envelope.payload});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status handle_advance_processes(const net::CommandEnvelope& envelope,
                                                    const net::CommandExecutionContext& context,
                                                    WorldOperation& operation) {
    auto world_status = require_authoritative_world(context);
    if (!world_status) {
        return world_status;
    }

    auto payload = parse_process_advance_all_payload(envelope.payload);
    if (!payload) {
        return core::Status::failure(payload.error().code, payload.error().message);
    }
    if (context.prototypes == nullptr) {
        return core::Status::failure("world_command.missing_prototypes",
                                     "process advancement requires prototype registry");
    }

    auto advanced = context.world_state->processes().advance_all(
        context.world_state->world_time(),
        [state = context.world_state,
         prototypes = context.prototypes](const processes::ProcessInstance& process) {
            return resolve_authoritative_process_modifiers(*state, *prototypes, process);
        });
    if (!advanced) {
        return core::Status::failure(advanced.error().code, advanced.error().message);
    }
    if (advanced.value() == 0) {
        return core::Status::failure("world_command.no_processes_advanced",
                                     "process advance command did not change any process state");
    }

    auto status =
        operation.record_mutation("advance processes " + std::to_string(advanced.value()));
    if (!status) {
        return status;
    }
    operation.record_derived_update("Processes");
    operation.emit_event({"processes.advanced", {}, std::to_string(advanced.value())});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status handle_sleep(const net::CommandEnvelope& envelope,
                                        const net::CommandExecutionContext& context,
                                        WorldOperation& operation) {
    auto world_status = require_authoritative_world(context);
    if (!world_status) {
        return world_status;
    }
    auto payload = parse_sleep_payload(envelope.payload);
    if (!payload) {
        return core::Status::failure(payload.error().code, payload.error().message);
    }
    const simulation::WorldTimeConfig default_config;
    const auto& config =
        context.world_time_config == nullptr ? default_config : *context.world_time_config;
    auto status = context.world_state->advance_world_time_hours(payload.value().hours, config);
    if (!status) {
        return status;
    }
    status = operation.record_mutation("advance authoritative world time by sleep");
    if (!status) {
        return status;
    }
    operation.record_derived_update("WorldTime");
    operation.emit_event(
        {"world.time_advanced", {}, std::to_string(context.world_state->world_time())});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status handle_create_cargo(const net::CommandEnvelope& envelope,
                                               const net::CommandExecutionContext& context,
                                               WorldOperation& operation) {
    auto world_status = require_authoritative_world(context);
    if (!world_status) {
        return world_status;
    }
    if (context.prototypes == nullptr) {
        return core::Status::failure("world_command.missing_prototypes",
                                     "cargo creation requires prototype registry");
    }

    auto payload = parse_cargo_create_payload(envelope.payload);
    if (!payload) {
        return core::Status::failure(payload.error().code, payload.error().message);
    }

    auto prototype_status = context.prototypes->require_kind(payload.value().prototype_id,
                                                             modding::PrototypeKinds::cargo);
    if (!prototype_status) {
        return prototype_status;
    }
    const auto* prototype = context.prototypes->find(payload.value().prototype_id);
    if (prototype == nullptr) {
        return core::Status::failure("world_command.missing_prototype",
                                     "cargo prototype is missing");
    }

    auto definition = cargo::cargo_definition_from_prototype(*prototype);
    if (!definition) {
        return core::Status::failure(definition.error().code, definition.error().message);
    }

    auto reserved_id = operation.reserve_save_id(context.world_state->save_ids());
    if (!reserved_id) {
        return core::Status::failure(reserved_id.error().code, reserved_id.error().message);
    }
    auto record = definition.value().create_record(reserved_id.value(), payload.value().position);
    if (!record) {
        return core::Status::failure(record.error().code, record.error().message);
    }

    auto status = context.world_state->cargo().insert(record.value());
    if (!status) {
        return status;
    }
    status = operation.record_mutation("create cargo " + payload.value().prototype_id.value());
    if (!status) {
        return status;
    }
    operation.record_derived_update("Cargo");
    operation.emit_event(
        {"cargo.created", reserved_id.value(), payload.value().prototype_id.value()});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status handle_spawn_entity(const net::CommandEnvelope& envelope,
                                               const net::CommandExecutionContext& context,
                                               WorldOperation& operation) {
    auto world_status = require_authoritative_world(context);
    if (!world_status) {
        return world_status;
    }
    if (context.prototypes == nullptr) {
        return core::Status::failure("world_command.missing_prototypes",
                                     "entity spawn requires prototype registry");
    }

    auto payload = parse_entity_spawn_payload(envelope.payload);
    if (!payload) {
        return core::Status::failure(payload.error().code, payload.error().message);
    }

    auto prototype_status = context.prototypes->require_kind(payload.value().prototype_id,
                                                             modding::PrototypeKinds::entity);
    if (!prototype_status) {
        return prototype_status;
    }
    const auto* prototype = context.prototypes->find(payload.value().prototype_id);
    if (prototype == nullptr) {
        return core::Status::failure("world_command.missing_prototype",
                                     "entity prototype is missing");
    }

    auto definition = entities::entity_definition_from_prototype(*prototype);
    if (!definition) {
        return core::Status::failure(definition.error().code, definition.error().message);
    }

    auto runtime_handle = context.world_state->runtime_handles().reserve();
    if (!runtime_handle) {
        return core::Status::failure(runtime_handle.error().code, runtime_handle.error().message);
    }
    auto net_id = context.world_state->entity_net_ids().reserve();
    if (!net_id) {
        return core::Status::failure(net_id.error().code, net_id.error().message);
    }

    core::SaveId save_id;
    if (definition.value().persistent) {
        auto reserved_save_id = operation.reserve_save_id(context.world_state->save_ids());
        if (!reserved_save_id) {
            return core::Status::failure(reserved_save_id.error().code,
                                         reserved_save_id.error().message);
        }
        save_id = reserved_save_id.value();
    }

    auto record = definition.value().create_record(runtime_handle.value(), net_id.value(), save_id,
                                                   payload.value().transform);
    if (!record) {
        return core::Status::failure(record.error().code, record.error().message);
    }

    auto status = context.world_state->entities().insert(record.value());
    if (!status) {
        return status;
    }
    status = operation.record_mutation("spawn entity " + payload.value().prototype_id.value());
    if (!status) {
        return status;
    }
    operation.record_derived_update("Entities");
    operation.emit_event({"entity.spawned", save_id, payload.value().prototype_id.value()});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status handle_create_assembly(const net::CommandEnvelope& envelope,
                                                  const net::CommandExecutionContext& context,
                                                  WorldOperation& operation) {
    auto world_status = require_authoritative_world(context);
    if (!world_status) {
        return world_status;
    }
    if (context.prototypes == nullptr) {
        return core::Status::failure("world_command.missing_prototypes",
                                     "assembly creation requires prototype registry");
    }

    auto payload = parse_assembly_create_payload(envelope.payload);
    if (!payload) {
        return core::Status::failure(payload.error().code, payload.error().message);
    }

    auto prototype_status = context.prototypes->require_kind(payload.value().prototype_id,
                                                             modding::PrototypeKinds::assembly);
    if (!prototype_status) {
        return prototype_status;
    }
    const auto* prototype = context.prototypes->find(payload.value().prototype_id);
    if (prototype == nullptr) {
        return core::Status::failure("world_command.missing_prototype",
                                     "assembly prototype is missing");
    }

    auto definition = assemblies::assembly_definition_from_prototype(*prototype);
    if (!definition) {
        return core::Status::failure(definition.error().code, definition.error().message);
    }

    auto reserved_id = operation.reserve_save_id(context.world_state->save_ids());
    if (!reserved_id) {
        return core::Status::failure(reserved_id.error().code, reserved_id.error().message);
    }
    auto record = build_assembly_record(*context.world_state, payload.value(), reserved_id.value(),
                                        definition.value());
    if (!record) {
        return core::Status::failure(record.error().code, record.error().message);
    }
    auto validation = assemblies::AssemblyValidator::validate(definition.value(), record.value());
    if (!validation.valid) {
        return core::Status::failure("world_command.invalid_assembly",
                                     "assembly record does not satisfy prototype requirements");
    }

    auto status = context.world_state->assemblies().insert(record.value());
    if (!status) {
        return status;
    }
    status = mark_assembly_derived_dirty(*context.world_state, record.value());
    if (!status) {
        return status;
    }
    status = rebuild_spatial_networks(*context.world_state);
    if (!status) {
        return status;
    }
    status = operation.record_mutation("create assembly " + payload.value().prototype_id.value());
    if (!status) {
        return status;
    }
    operation.record_derived_update("Assemblies");
    operation.record_derived_update("SpatialNetworks");
    operation.emit_event(
        {"assembly.created", reserved_id.value(), payload.value().prototype_id.value()});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Result<assemblies::AssemblyDefinition>
load_assembly_definition(const modding::PrototypeRegistry* prototypes,
                         const core::PrototypeId& prototype_id) {
    if (prototypes == nullptr) {
        return core::Result<assemblies::AssemblyDefinition>::failure(
            "world_command.missing_prototypes", "assembly command requires prototype registry");
    }
    auto status = prototypes->require_kind(prototype_id, modding::PrototypeKinds::assembly);
    if (!status) {
        return core::Result<assemblies::AssemblyDefinition>::failure(status.error().code,
                                                                     status.error().message);
    }
    return assemblies::assembly_definition_from_prototype(*prototypes->find(prototype_id));
}

[[nodiscard]] core::Status
handle_start_assembly_blueprint(const net::CommandEnvelope& envelope,
                                const net::CommandExecutionContext& context,
                                WorldOperation& operation) {
    auto status = require_authoritative_world(context);
    if (!status)
        return status;
    auto payload = parse_assembly_blueprint_payload(envelope.payload);
    if (!payload)
        return core::Status::failure(payload.error().code, payload.error().message);
    const auto* root =
        context.world_state->build_objects().find(payload.value().root_build_piece_id);
    if (root == nullptr || root->construction_state != build::ConstructionState::complete) {
        return core::Status::failure("world_command.invalid_assembly_root",
                                     "assembly blueprint requires a completed root build piece");
    }
    auto definition = load_assembly_definition(context.prototypes, payload.value().prototype_id);
    if (!definition)
        return core::Status::failure(definition.error().code, definition.error().message);
    auto id = operation.reserve_save_id(context.world_state->save_ids());
    if (!id)
        return core::Status::failure(id.error().code, id.error().message);
    auto record = assemblies::AssemblyRuntime::create_blueprint(
        id.value(), payload.value().root_build_piece_id, definition.value());
    if (!record)
        return core::Status::failure(record.error().code, record.error().message);
    record.value().root_coord = root->transform.position.anchor;
    status = context.world_state->assemblies().insert(std::move(record).value());
    if (!status)
        return status;
    status = mark_assembly_derived_dirty(*context.world_state,
                                         *context.world_state->assemblies().find(id.value()));
    if (!status)
        return status;
    status = operation.record_mutation("start assembly blueprint " + id.value().to_string());
    if (!status)
        return status;
    operation.record_derived_update("AssemblyGhostBlueprint");
    operation.emit_event(
        {"assembly.blueprint_started", id.value(), payload.value().prototype_id.value()});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status handle_place_assembly_part(const net::CommandEnvelope& envelope,
                                                      const net::CommandExecutionContext& context,
                                                      WorldOperation& operation) {
    auto status = require_authoritative_world(context);
    if (!status)
        return status;
    auto payload = parse_assembly_place_part_payload(envelope.payload);
    if (!payload)
        return core::Status::failure(payload.error().code, payload.error().message);
    auto* assembly = context.world_state->assemblies().find(payload.value().assembly_id);
    const auto* part = context.world_state->build_objects().find(payload.value().build_piece_id);
    if (assembly == nullptr || part == nullptr ||
        part->construction_state != build::ConstructionState::complete) {
        return core::Status::failure("world_command.invalid_assembly_part",
                                     "assembly or completed build piece is missing");
    }
    auto definition = load_assembly_definition(context.prototypes, assembly->prototype_id);
    if (!definition)
        return core::Status::failure(definition.error().code, definition.error().message);
    const auto requirement =
        std::ranges::find_if(definition.value().part_requirements, [&payload](const auto& value) {
            return value.name == payload.value().part_name;
        });
    if (requirement == definition.value().part_requirements.end())
        return core::Status::failure("world_command.unknown_assembly_part",
                                     "assembly part is not declared by the prototype");
    auto expected = checked_block_coord_offset(assembly->root_coord, requirement->relative_coord);
    if (!expected || part->transform.position.anchor != expected.value())
        return core::Status::failure("world_command.assembly_part_position_mismatch",
                                     "assembly part is outside its ghost blueprint slot");
    status = assemblies::AssemblyRuntime::place_part(
        *assembly, definition.value(),
        {payload.value().part_name, payload.value().build_piece_id, part->prototype_id, {}});
    if (!status)
        return status;
    status = mark_assembly_derived_dirty(*context.world_state, *assembly);
    if (!status)
        return status;
    status = operation.record_mutation("place assembly part " + payload.value().part_name);
    if (!status)
        return status;
    operation.record_derived_update("AssemblyGhostBlueprint");
    operation.emit_event(
        {"assembly.part_placed", assembly->assembly_id, payload.value().part_name});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status
handle_advance_assembly_stage(const net::CommandEnvelope& envelope,
                              const net::CommandExecutionContext& context,
                              WorldOperation& operation) {
    auto status = require_authoritative_world(context);
    if (!status)
        return status;
    auto payload = parse_assembly_advance_payload(envelope.payload);
    if (!payload)
        return core::Status::failure(payload.error().code, payload.error().message);
    auto* assembly = context.world_state->assemblies().find(payload.value().assembly_id);
    if (assembly == nullptr) {
        return core::Status::failure("world_command.missing_assembly", "assembly is missing");
    }
    auto definition = load_assembly_definition(context.prototypes, assembly->prototype_id);
    if (!definition)
        return core::Status::failure(definition.error().code, definition.error().message);
    status = assemblies::AssemblyRuntime::advance_stage(*assembly, definition.value());
    if (!status)
        return status;
    if (assembly->state == assemblies::AssemblyState::ready) {
        AssemblyCreateCommandPayload completed_payload;
        completed_payload.prototype_id = assembly->prototype_id;
        completed_payload.root_build_piece_id = assembly->root_build_piece_id;
        for (const auto& part : assembly->parts) {
            completed_payload.parts.push_back({part.name, part.build_piece_id});
        }
        auto completed = build_assembly_record(*context.world_state, completed_payload,
                                               assembly->assembly_id, definition.value());
        if (!completed)
            return core::Status::failure(completed.error().code, completed.error().message);
        completed.value().current_stage = assembly->current_stage;
        completed.value().revision = assembly->revision;
        completed.value().process_slots = assembly->process_slots;
        *assembly = std::move(completed).value();
    }
    status = mark_assembly_derived_dirty(*context.world_state, *assembly);
    if (!status)
        return status;
    status =
        operation.record_mutation("advance assembly stage " + assembly->assembly_id.to_string());
    if (!status)
        return status;
    operation.record_derived_update("Assemblies");
    operation.emit_event({"assembly.stage_advanced", assembly->assembly_id,
                          std::to_string(assembly->current_stage)});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

[[nodiscard]] core::Status handle_transition_assembly(const net::CommandEnvelope& envelope,
                                                      const net::CommandExecutionContext& context,
                                                      WorldOperation& operation) {
    auto status = require_authoritative_world(context);
    if (!status)
        return status;
    auto payload = parse_assembly_transition_payload(envelope.payload);
    if (!payload)
        return core::Status::failure(payload.error().code, payload.error().message);
    auto* assembly = context.world_state->assemblies().find(payload.value().assembly_id);
    if (assembly == nullptr) {
        return core::Status::failure("world_command.missing_assembly", "assembly is missing");
    }
    status = assemblies::AssemblyRuntime::transition(*assembly, payload.value().state,
                                                     payload.value().reason);
    if (!status)
        return status;
    status = operation.record_mutation("transition assembly " + assembly->assembly_id.to_string());
    if (!status)
        return status;
    operation.record_derived_update("AssemblyStateMachine");
    operation.emit_event({"assembly.state_changed", assembly->assembly_id,
                          std::string(assemblies::assembly_state_name(assembly->state))});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    return core::Status::ok();
}

} // namespace

core::Status
WorldCommandRegistry::register_engine_commands(net::ServerCommandDispatcher& dispatcher) {
    auto status = dispatcher.register_command(
        net::CommandDescriptor{"world.set_voxel", true, true, handle_set_voxel});
    if (!status) {
        return status;
    }
    status = dispatcher.register_command(
        net::CommandDescriptor{"build.place_piece", true, true, handle_place_build_piece});
    if (!status) {
        return status;
    }
    status = dispatcher.register_command(
        net::CommandDescriptor{"build.complete_piece", true, true, handle_complete_build_piece});
    if (!status) {
        return status;
    }
    status = dispatcher.register_command(
        net::CommandDescriptor{"workpiece.edit_cell", true, true, handle_edit_workpiece});
    if (!status) {
        return status;
    }
    status = dispatcher.register_command(
        net::CommandDescriptor{"workpiece.finish", true, true, handle_finish_workpiece});
    if (!status) {
        return status;
    }
    status = dispatcher.register_command(net::CommandDescriptor{
        "inventory.transfer_items", true, true, handle_transfer_inventory_items});
    if (!status) {
        return status;
    }
    status = dispatcher.register_command(
        net::CommandDescriptor{"process.start", true, true, handle_start_process});
    if (!status) {
        return status;
    }
    status = dispatcher.register_command(
        net::CommandDescriptor{"process.advance_all", true, true, handle_advance_processes});
    if (!status) {
        return status;
    }
    status = dispatcher.register_command(
        net::CommandDescriptor{"world.sleep", true, true, handle_sleep});
    if (!status) {
        return status;
    }
    status = dispatcher.register_command(
        net::CommandDescriptor{"cargo.create", true, true, handle_create_cargo});
    if (!status) {
        return status;
    }
    status = dispatcher.register_command(
        net::CommandDescriptor{"entity.spawn", true, true, handle_spawn_entity});
    if (!status) {
        return status;
    }
    status = dispatcher.register_command(
        net::CommandDescriptor{"assembly.create", true, true, handle_create_assembly});
    if (!status)
        return status;
    status = dispatcher.register_command(net::CommandDescriptor{
        "assembly.start_blueprint", true, true, handle_start_assembly_blueprint});
    if (!status)
        return status;
    status = dispatcher.register_command(
        net::CommandDescriptor{"assembly.place_part", true, true, handle_place_assembly_part});
    if (!status)
        return status;
    status = dispatcher.register_command(net::CommandDescriptor{
        "assembly.advance_stage", true, true, handle_advance_assembly_stage});
    if (!status)
        return status;
    return dispatcher.register_command(
        net::CommandDescriptor{"assembly.transition", true, true, handle_transition_assembly});
}

} // namespace heartstead::world
