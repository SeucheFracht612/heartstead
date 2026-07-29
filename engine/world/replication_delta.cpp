#include "engine/world/replication_delta.hpp"

#include "engine/net/binary_message.hpp"
#include "engine/net/client_session.hpp"
#include "engine/net/host_session.hpp"
#include "engine/save/save_binary_codec.hpp"
#include "engine/save/save_text_codec.hpp"
#include "engine/workpieces/workpiece_codec.hpp"
#include "engine/workpieces/workpiece_state.hpp"
#include "engine/world/voxel_change.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace heartstead::world {

namespace {

constexpr std::string_view delta_magic = "heartstead.replication_delta_snapshot.v1";
constexpr std::string_view snapshot_begin_marker = "snapshot_begin";
constexpr std::string_view snapshot_end_marker = "snapshot_end";
constexpr std::uint32_t delta_binary_magic = 0x31445248U; // "HRD1" in little-endian byte order.
constexpr std::size_t max_delta_binary_bytes = 4U * 1024U * 1024U;
constexpr std::size_t max_delta_command_type_bytes = 128;
constexpr std::size_t max_delta_event_type_bytes = 256;
constexpr std::size_t max_delta_event_message_bytes = 1024U * 1024U;
constexpr std::size_t max_delta_records = 100'000;

[[nodiscard]] std::uint64_t key(core::SaveId id) noexcept {
    return id.value();
}

[[nodiscard]] std::uint64_t key(core::ProcessId id) noexcept {
    return id.value();
}

void classify_subject(const WorldState& state, WorldReplicationDeltaSubjectPlan& subject) {
    subject.has_build_piece = state.build_objects().find(subject.subject_id) != nullptr;
    subject.has_entity = state.entities().find_by_save_id(subject.subject_id) != nullptr ||
                         state.physical_resources().find(subject.subject_id) != nullptr;
    subject.has_cargo = state.cargo().find(subject.subject_id) != nullptr;
    subject.has_assembly = state.assemblies().find(subject.subject_id) != nullptr;
    subject.has_inventory = state.inventories().find(subject.subject_id) != nullptr;
    subject.has_workpiece =
        state.workpieces().find(core::WorkpieceId::from_value(subject.subject_id.value())) !=
        nullptr;
    subject.process_count =
        static_cast<std::uint32_t>(state.processes().find_by_owner(subject.subject_id).size());

    subject.materialized_record_count = 0;
    subject.materialized_record_count += subject.has_build_piece ? 1U : 0U;
    subject.materialized_record_count += subject.has_entity ? 1U : 0U;
    subject.materialized_record_count += subject.has_cargo ? 1U : 0U;
    subject.materialized_record_count += subject.has_assembly ? 1U : 0U;
    subject.materialized_record_count += subject.has_inventory ? 1U : 0U;
    subject.materialized_record_count += subject.has_workpiece ? 1U : 0U;
    subject.materialized_record_count += subject.process_count;
    subject.missing_subject = subject.materialized_record_count == 0;
}

[[nodiscard]] save::WorkpieceSaveRecord
public_workpiece_save_record(const WorkpieceRecord& record) {
    std::string encoded_server_state;
    if (record.server_state.has_value()) {
        auto public_state = *record.server_state;
        for (std::size_t index = 0; index < public_state.hidden_flaw_mask.size(); ++index) {
            public_state.hidden_flaw_mask[index] &= public_state.revealed_mask[index];
        }
        encoded_server_state =
            workpieces::WorkpieceServerStateTextCodec::encode(public_state, record.grid.shape());
    }
    return {
        record.workpiece_id,
        record.prototype_id,
        record.grid.shape(),
        workpieces::WorkpieceGridTextCodec::encode(record.grid),
        record.material_prototype_id,
        std::move(encoded_server_state),
        record.owner_session,
        record.revision,
        record.committed,
    };
}

[[nodiscard]] save::EntitySaveRecord entity_save_record(const entities::EntityRecord& entity) {
    return save::EntitySaveRecord{
        entity.save_id, entity.prototype_id, entity.kind, entity.sleeping, {}, entity.transform,
    };
}

[[nodiscard]] save::EntitySaveRecord
physical_resource_save_record(const entities::PhysicalResourceRecord& resource) {
    entities::Transform transform;
    transform.position = resource.position;
    return {
        resource.resource_id,
        resource.prototype_id,
        entities::EntityKind::temporary_physics,
        resource.state == entities::PhysicalResourceState::settled_sleeping ||
            resource.state == entities::PhysicalResourceState::frozen_static,
        entities::PhysicalResourceTextCodec::encode(resource),
        transform,
    };
}

[[nodiscard]] std::vector<const processes::ProcessInstance*>
sorted_processes_by_owner(const WorldState& state, core::SaveId owner_id) {
    auto processes = state.processes().find_by_owner(owner_id);
    std::ranges::sort(processes, [](const auto* lhs, const auto* rhs) {
        return lhs->process_id.value() < rhs->process_id.value();
    });
    return processes;
}

[[nodiscard]] bool is_hex_digit(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

[[nodiscard]] char hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<char>(10 + value - 'a');
    }
    return static_cast<char>(10 + value - 'A');
}

[[nodiscard]] std::string delta_percent_escape(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto value : input) {
        const auto byte = static_cast<unsigned char>(value);
        if (value == '%' || value == '|' || value == '=' || value == '\n' || value == '\r') {
            result.push_back('%');
            result.push_back(hex[(byte >> 4u) & 0x0Fu]);
            result.push_back(hex[byte & 0x0Fu]);
        } else {
            result.push_back(value);
        }
    }

    return result;
}

[[nodiscard]] core::Result<std::string> delta_percent_unescape(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] != '%') {
            result.push_back(input[index]);
            continue;
        }
        if (index + 2 >= input.size() || !is_hex_digit(input[index + 1]) ||
            !is_hex_digit(input[index + 2])) {
            return core::Result<std::string>::failure(
                "replication_delta.invalid_escape",
                "replication delta payload contains an invalid escape");
        }

        const auto high = hex_value(input[index + 1]);
        const auto low = hex_value(input[index + 2]);
        result.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }

    return core::Result<std::string>::success(std::move(result));
}

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

[[nodiscard]] core::Result<std::uint64_t> parse_u64(std::string_view value,
                                                    std::string_view field_name) {
    std::uint64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return core::Result<std::uint64_t>::failure("replication_delta.invalid_number",
                                                    "invalid numeric replication delta field: " +
                                                        std::string(field_name));
    }
    return core::Result<std::uint64_t>::success(parsed);
}

[[nodiscard]] core::Result<std::uint32_t> parse_u32(std::string_view value,
                                                    std::string_view field_name) {
    auto parsed = parse_u64(value, field_name);
    if (!parsed) {
        return core::Result<std::uint32_t>::failure(parsed.error().code, parsed.error().message);
    }
    if (parsed.value() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<std::uint32_t>::failure(
            "replication_delta.number_out_of_range",
            "numeric replication delta field is too large: " + std::string(field_name));
    }
    return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(parsed.value()));
}

[[nodiscard]] core::Result<bool> parse_bool(std::string_view value, std::string_view field_name) {
    if (value == "0") {
        return core::Result<bool>::success(false);
    }
    if (value == "1") {
        return core::Result<bool>::success(true);
    }
    return core::Result<bool>::failure("replication_delta.invalid_bool",
                                       "boolean replication delta field must be 0 or 1: " +
                                           std::string(field_name));
}

[[nodiscard]] char encode_bool(bool value) {
    return value ? '1' : '0';
}

[[nodiscard]] std::string encode_global_event(const OperationEvent& event) {
    std::ostringstream output;
    output << delta_percent_escape(event.type) << '|' << event.subject.value() << '|'
           << delta_percent_escape(event.message);
    return output.str();
}

[[nodiscard]] core::Result<OperationEvent> parse_global_event(std::string_view value) {
    const auto parts = split(value, '|');
    if (parts.size() != 3) {
        return core::Result<OperationEvent>::failure(
            "replication_delta.invalid_global_event",
            "global replication delta event must contain type, subject, and message");
    }

    auto type = delta_percent_unescape(parts[0]);
    auto subject = parse_u64(parts[1], "global_event_subject");
    auto message = delta_percent_unescape(parts[2]);
    if (!type || !subject || !message) {
        return core::Result<OperationEvent>::failure(
            "replication_delta.invalid_global_event",
            "global replication delta event contains invalid fields");
    }
    return core::Result<OperationEvent>::success(
        OperationEvent{std::move(type).value(), core::SaveId::from_value(subject.value()),
                       std::move(message).value()});
}

[[nodiscard]] std::string encode_subject_plan(const WorldReplicationDeltaSubjectPlan& subject) {
    std::ostringstream output;
    output << subject.subject_id.value() << '|' << subject.event_count << '|'
           << delta_percent_escape(subject.first_event_type) << '|'
           << encode_bool(subject.has_build_piece) << '|' << encode_bool(subject.has_entity) << '|'
           << encode_bool(subject.has_cargo) << '|' << encode_bool(subject.has_assembly) << '|'
           << encode_bool(subject.has_inventory) << '|' << subject.process_count << '|'
           << subject.materialized_record_count << '|' << encode_bool(subject.missing_subject)
           << '|' << encode_bool(subject.has_workpiece) << '|' << subject.private_event_count;
    return output.str();
}

[[nodiscard]] core::Result<WorldReplicationDeltaSubjectPlan>
parse_subject_plan(std::string_view value) {
    const auto parts = split(value, '|');
    if (parts.size() != 11 && parts.size() != 12 && parts.size() != 13) {
        return core::Result<WorldReplicationDeltaSubjectPlan>::failure(
            "replication_delta.invalid_subject",
            "replication delta subject record must contain 11, 12, or 13 fields");
    }

    auto subject_id = parse_u64(parts[0], "subject_id");
    auto event_count = parse_u32(parts[1], "subject_event_count");
    auto first_event_type = delta_percent_unescape(parts[2]);
    auto has_build_piece = parse_bool(parts[3], "subject_has_build_piece");
    auto has_entity = parse_bool(parts[4], "subject_has_entity");
    auto has_cargo = parse_bool(parts[5], "subject_has_cargo");
    auto has_assembly = parse_bool(parts[6], "subject_has_assembly");
    auto has_inventory = parse_bool(parts[7], "subject_has_inventory");
    auto process_count = parse_u32(parts[8], "subject_process_count");
    auto materialized_count = parse_u32(parts[9], "subject_materialized_record_count");
    auto missing = parse_bool(parts[10], "subject_missing");
    auto has_workpiece = parts.size() >= 12 ? parse_bool(parts[11], "subject_has_workpiece")
                                            : core::Result<bool>::success(false);
    auto private_event_count = parts.size() == 13
                                   ? parse_u32(parts[12], "subject_private_event_count")
                                   : core::Result<std::uint32_t>::success(0);
    if (!subject_id || !event_count || !first_event_type || !has_build_piece || !has_entity ||
        !has_cargo || !has_assembly || !has_inventory || !process_count || !materialized_count ||
        !missing || !has_workpiece || !private_event_count) {
        return core::Result<WorldReplicationDeltaSubjectPlan>::failure(
            "replication_delta.invalid_subject",
            "replication delta subject record contains invalid fields");
    }

    auto result = WorldReplicationDeltaSubjectPlan{
        core::SaveId::from_value(subject_id.value()),
        event_count.value(),
        std::move(first_event_type).value(),
        has_build_piece.value(),
        has_entity.value(),
        has_cargo.value(),
        has_assembly.value(),
        has_inventory.value(),
        process_count.value(),
        materialized_count.value(),
        missing.value(),
    };
    result.has_workpiece = has_workpiece.value();
    result.private_event_count = private_event_count.value();
    return core::Result<WorldReplicationDeltaSubjectPlan>::success(std::move(result));
}

[[nodiscard]] save::SaveSnapshot
save_snapshot_from_delta(const WorldReplicationDeltaSnapshot& snapshot) {
    save::SaveSnapshot result;
    result.metadata.schema_version = save::current_save_schema_version;
    result.metadata.game_version = "replication_delta";
    result.metadata.world_seed = snapshot.plan.replication_sequence != 0
                                     ? snapshot.plan.replication_sequence
                                     : snapshot.plan.command_sequence;
    result.build_pieces = snapshot.build_pieces;
    result.entities = snapshot.entities;
    result.cargo_records = snapshot.cargo_records;
    result.inventories = snapshot.inventories;
    result.workpieces = snapshot.workpieces;
    result.assemblies = snapshot.assemblies;
    result.processes = snapshot.processes;
    return result;
}

[[nodiscard]] core::Status apply_save_snapshot_to_delta(WorldReplicationDeltaSnapshot& target,
                                                        save::SaveSnapshot snapshot) {
    if (!snapshot.chunk_edits.empty() || !snapshot.mod_states.empty()) {
        return core::Status::failure(
            "replication_delta.unsupported_snapshot_section",
            "replication delta snapshot payload contains sections that are not delta-materialized");
    }

    target.build_pieces = std::move(snapshot.build_pieces);
    target.entities = std::move(snapshot.entities);
    target.cargo_records = std::move(snapshot.cargo_records);
    target.inventories = std::move(snapshot.inventories);
    target.workpieces = std::move(snapshot.workpieces);
    target.assemblies = std::move(snapshot.assemblies);
    target.processes = std::move(snapshot.processes);
    return core::Status::ok();
}

[[nodiscard]] std::uint32_t section_record_count(const WorldReplicationDeltaSnapshot& snapshot) {
    return static_cast<std::uint32_t>(snapshot.build_pieces.size() + snapshot.entities.size() +
                                      snapshot.cargo_records.size() + snapshot.inventories.size() +
                                      snapshot.workpieces.size() + snapshot.assemblies.size() +
                                      snapshot.processes.size());
}

[[nodiscard]] core::Status
validate_delta_snapshot_payload(const WorldReplicationDeltaSnapshot& snapshot) {
    std::uint64_t subject_event_count = 0;
    std::uint64_t materialized_record_count = 0;
    std::uint64_t missing_subject_count = 0;
    for (const auto& subject : snapshot.plan.subjects) {
        if (subject.private_event_count > subject.event_count) {
            return core::Status::failure(
                "replication_delta.private_event_count_mismatch",
                "replication delta private event count exceeds the subject event count");
        }
        subject_event_count += subject.event_count;
        materialized_record_count += subject.materialized_record_count;
        missing_subject_count += subject.missing_subject ? 1ULL : 0ULL;
    }
    if (snapshot.plan.global_events.size() != snapshot.plan.global_event_count ||
        snapshot.plan.subjects.size() != snapshot.plan.unique_subject_count ||
        subject_event_count != snapshot.plan.subject_event_count ||
        materialized_record_count != snapshot.plan.materialized_record_count ||
        missing_subject_count != snapshot.plan.missing_subject_count ||
        snapshot.plan.subject_event_count + snapshot.plan.global_event_count !=
            snapshot.plan.event_count) {
        return core::Status::failure("replication_delta.count_mismatch",
                                     "replication delta payload aggregate counts do not add up");
    }
    if (section_record_count(snapshot) != snapshot.plan.materialized_record_count) {
        return core::Status::failure(
            "replication_delta.section_count_mismatch",
            "replication delta payload section counts do not match the plan");
    }
    if (snapshot.plan.requires_snapshot_resync != (snapshot.plan.missing_subject_count > 0)) {
        return core::Status::failure(
            "replication_delta.resync_mismatch",
            "replication delta payload resync flag does not match missing subject count");
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status track_unique_delta_save_id(std::set<std::uint64_t>& ids, core::SaveId id,
                                                      std::string_view label) {
    if (!id.is_valid()) {
        return core::Status::failure("replication_delta_apply.invalid_save_id",
                                     std::string(label) + " has an invalid save id");
    }
    if (!ids.insert(key(id)).second) {
        return core::Status::failure("replication_delta_apply.duplicate_save_id",
                                     std::string(label) + " duplicates save id " + id.to_string());
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status validate_delta_for_apply(const WorldReplicationDeltaSnapshot& snapshot) {
    auto status = validate_delta_snapshot_payload(snapshot);
    if (!status) {
        return status;
    }
    if (snapshot.plan.requires_snapshot_resync) {
        return core::Status::failure(
            "replication_delta_apply.requires_resync",
            "world replication delta is partial and requires snapshot/resync fallback");
    }

    std::set<std::uint64_t> save_ids;
    std::set<std::uint64_t> inventory_owner_ids;
    std::set<std::uint64_t> process_ids;

    for (const auto& record : snapshot.build_pieces) {
        status = record.validate();
        if (!status) {
            return status;
        }
        status = track_unique_delta_save_id(save_ids, record.object_id, "build piece");
        if (!status) {
            return status;
        }
    }
    for (const auto& record : snapshot.entities) {
        status = record.validate();
        if (!status) {
            return status;
        }
        status = track_unique_delta_save_id(save_ids, record.save_id, "entity");
        if (!status) {
            return status;
        }
    }
    for (const auto& record : snapshot.cargo_records) {
        status = record.validate();
        if (!status) {
            return status;
        }
        status = track_unique_delta_save_id(save_ids, record.cargo_id, "cargo");
        if (!status) {
            return status;
        }
    }
    for (const auto& record : snapshot.assemblies) {
        status = record.validate_record();
        if (!status) {
            return status;
        }
        status = track_unique_delta_save_id(save_ids, record.assembly_id, "assembly");
        if (!status) {
            return status;
        }
    }
    for (const auto& record : snapshot.inventories) {
        status = InventoryRecord{record.owner_id, record.stacks}.validate();
        if (!status) {
            return status;
        }
        if (!inventory_owner_ids.insert(key(record.owner_id)).second) {
            return core::Status::failure("replication_delta_apply.duplicate_inventory",
                                         "replication delta contains duplicate inventory owner " +
                                             record.owner_id.to_string());
        }
    }
    for (const auto& record : snapshot.workpieces) {
        auto grid = workpieces::WorkpieceGridTextCodec::decode(record.encoded_cells);
        if (!grid || grid.value().shape() != record.shape) {
            return core::Status::failure("replication_delta_apply.invalid_workpiece_grid",
                                         "replication delta contains an invalid workpiece grid");
        }
        std::optional<workpieces::WorkpieceServerState> server_state;
        if (!record.encoded_server_state.empty()) {
            auto decoded = workpieces::WorkpieceServerStateTextCodec::decode(
                record.encoded_server_state, record.shape);
            if (!decoded) {
                return core::Status::failure(decoded.error().code, decoded.error().message);
            }
            server_state = std::move(decoded).value();
        }
        auto workpiece_status =
            WorkpieceRecord{record.workpiece_id,     record.prototype_id,
                            std::move(grid).value(), record.material_prototype_id,
                            std::move(server_state), record.owner_session,
                            record.revision,         record.committed}
                .validate();
        if (!workpiece_status)
            return workpiece_status;
        status = track_unique_delta_save_id(
            save_ids, core::SaveId::from_value(record.workpiece_id.value()), "workpiece");
        if (!status)
            return status;
    }
    for (const auto& record : snapshot.processes) {
        status = record.validate();
        if (!status) {
            return status;
        }
        if (!process_ids.insert(key(record.process_id)).second) {
            return core::Status::failure("replication_delta_apply.duplicate_process",
                                         "replication delta contains duplicate process id " +
                                             record.process_id.to_string());
        }
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

[[nodiscard]] core::Status
mark_replicated_build_piece_derived_dirty(WorldState& state, const build::BuildPieceRecord& record,
                                          std::string_view reason) {
    auto bounds = build_piece_dirty_bounds(record).expanded(1);
    if (!bounds) {
        return core::Status::failure(bounds.error().code, bounds.error().message);
    }

    auto status =
        state.dirty_regions().mark(dirty::DirtyRegionKind::room_graph, bounds.value(),
                                   std::string(reason) + " " + record.prototype_id.value());
    if (!status) {
        return status;
    }

    for (const auto& port : record.network_ports) {
        auto& network = state.networks().get_or_create(port.kind);
        status = network.mark_dirty_region(state.dirty_regions(), bounds.value(),
                                           std::string(reason) + " port " + port.name);
        if (!status) {
            return status;
        }
    }

    return core::Status::ok();
}

[[nodiscard]] core::Status
mark_replicated_assembly_derived_dirty(WorldState& state, const assemblies::AssemblyRecord& record,
                                       std::string_view reason) {
    const auto* root = state.build_objects().find(record.root_build_piece_id);
    if (root == nullptr) {
        return core::Status::failure("replication_delta_apply.missing_assembly_root",
                                     "assembly root build piece is not present in world state");
    }

    auto bounds = build_piece_dirty_bounds(*root).expanded(1);
    if (!bounds) {
        return core::Status::failure(bounds.error().code, bounds.error().message);
    }

    auto status =
        state.dirty_regions().mark(dirty::DirtyRegionKind::room_graph, bounds.value(),
                                   std::string(reason) + " " + record.prototype_id.value());
    if (!status) {
        return status;
    }

    for (const auto& port : record.ports) {
        auto& network = state.networks().get_or_create(port.kind);
        status = network.mark_dirty_region(state.dirty_regions(), bounds.value(),
                                           std::string(reason) + " port " + port.name);
        if (!status) {
            return status;
        }
    }

    return core::Status::ok();
}

[[nodiscard]] core::Status require_build_piece_exists(const WorldState& state, core::SaveId id,
                                                      std::string_view code,
                                                      std::string_view message) {
    if (!id.is_valid() || state.build_objects().find(id) == nullptr) {
        return core::Status::failure(std::string(code), std::string(message));
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status validate_assembly_references(const WorldState& state,
                                                        const assemblies::AssemblyRecord& record) {
    auto status = require_build_piece_exists(
        state, record.root_build_piece_id, "replication_delta_apply.missing_assembly_root",
        "assembly root build piece is not present in world state");
    if (!status) {
        return status;
    }
    for (const auto& part : record.parts) {
        status = require_build_piece_exists(
            state, part.build_piece_id, "replication_delta_apply.missing_assembly_part",
            "assembly part build piece is not present in world state");
        if (!status) {
            return status;
        }
    }
    for (const auto& port : record.ports) {
        status = require_build_piece_exists(
            state, port.source_build_piece_id,
            "replication_delta_apply.missing_assembly_port_source",
            "assembly port source build piece is not present in world state");
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status require_saved_owner_exists(const WorldState& state, core::SaveId id,
                                                      std::string_view label) {
    if (!id.is_valid() || !state.contains_saved_object(id)) {
        return core::Status::failure("replication_delta_apply.missing_owner",
                                     std::string(label) + " references a missing owner save id " +
                                         id.to_string());
    }
    return core::Status::ok();
}

void count_inserted(WorldReplicationDeltaApplyReport& report, std::uint32_t& section_count) {
    ++section_count;
    ++report.inserted_record_count;
    ++report.applied_record_count;
}

void count_updated(WorldReplicationDeltaApplyReport& report, std::uint32_t& section_count) {
    ++section_count;
    ++report.updated_record_count;
    ++report.applied_record_count;
}

[[nodiscard]] core::Status upsert_build_piece(WorldState& state,
                                              const build::BuildPieceRecord& record,
                                              WorldReplicationDeltaApplyReport& report) {
    if (auto* existing = state.build_objects().find(record.object_id); existing != nullptr) {
        auto status = mark_replicated_build_piece_derived_dirty(state, *existing,
                                                                "replicated build piece replaced");
        if (!status) {
            return status;
        }
        *existing = record;
        status = mark_replicated_build_piece_derived_dirty(state, *existing,
                                                           "replicated build piece updated");
        if (!status) {
            return status;
        }
        count_updated(report, report.build_pieces_updated);
        return core::Status::ok();
    }

    auto status = state.build_objects().insert(record);
    if (!status) {
        return status;
    }
    status =
        mark_replicated_build_piece_derived_dirty(state, record, "replicated build piece inserted");
    if (!status) {
        return status;
    }
    count_inserted(report, report.build_pieces_inserted);
    return core::Status::ok();
}

[[nodiscard]] core::Status upsert_entity(WorldState& state, const save::EntitySaveRecord& record,
                                         WorldReplicationDeltaApplyReport& report) {
    if (record.encoded_state.starts_with(entities::physical_resource_state_magic)) {
        auto decoded = entities::PhysicalResourceTextCodec::decode(
            record.save_id, record.prototype_id, record.transform.position, record.encoded_state);
        if (!decoded)
            return core::Status::failure(decoded.error().code, decoded.error().message);
        if (auto* existing = state.physical_resources().find(record.save_id); existing != nullptr) {
            *existing = std::move(decoded).value();
            count_updated(report, report.entities_updated);
            return core::Status::ok();
        }
        auto status = state.physical_resources().insert(std::move(decoded).value());
        if (!status)
            return status;
        count_inserted(report, report.entities_inserted);
        return core::Status::ok();
    }
    if (auto* existing = state.entities().find_by_save_id(record.save_id); existing != nullptr) {
        existing->prototype_id = record.prototype_id;
        existing->kind = record.kind;
        existing->transform = record.transform;
        existing->persistent = true;
        existing->sleeping = record.sleeping;
        auto status = existing->validate();
        if (!status) {
            return status;
        }
        count_updated(report, report.entities_updated);
        return core::Status::ok();
    }

    auto runtime_handle = state.runtime_handles().reserve();
    if (!runtime_handle) {
        return core::Status::failure(runtime_handle.error().code, runtime_handle.error().message);
    }
    auto net_id = state.entity_net_ids().reserve();
    if (!net_id) {
        return core::Status::failure(net_id.error().code, net_id.error().message);
    }

    entities::EntityRecord entity;
    entity.runtime_handle = runtime_handle.value();
    entity.net_id = net_id.value();
    entity.save_id = record.save_id;
    entity.prototype_id = record.prototype_id;
    entity.kind = record.kind;
    entity.transform = record.transform;
    entity.persistent = true;
    entity.sleeping = record.sleeping;
    auto status = state.entities().insert(entity);
    if (!status) {
        return status;
    }
    count_inserted(report, report.entities_inserted);
    return core::Status::ok();
}

[[nodiscard]] core::Status upsert_cargo(WorldState& state, const cargo::CargoRecord& record,
                                        WorldReplicationDeltaApplyReport& report) {
    if (auto* existing = state.cargo().find(record.cargo_id); existing != nullptr) {
        *existing = record;
        count_updated(report, report.cargo_updated);
        return core::Status::ok();
    }

    auto status = state.cargo().insert(record);
    if (!status) {
        return status;
    }
    count_inserted(report, report.cargo_inserted);
    return core::Status::ok();
}

[[nodiscard]] core::Status upsert_inventory(WorldState& state,
                                            const save::InventorySaveRecord& record,
                                            WorldReplicationDeltaApplyReport& report) {
    auto status = require_saved_owner_exists(state, record.owner_id, "inventory");
    if (!status) {
        return status;
    }
    if (auto* existing = state.inventories().find(record.owner_id); existing != nullptr) {
        existing->stacks = record.stacks;
        status = existing->validate();
        if (!status) {
            return status;
        }
        count_updated(report, report.inventories_updated);
        return core::Status::ok();
    }

    status = state.inventories().insert(InventoryRecord{record.owner_id, record.stacks});
    if (!status) {
        return status;
    }
    count_inserted(report, report.inventories_inserted);
    return core::Status::ok();
}

[[nodiscard]] core::Status upsert_assembly(WorldState& state,
                                           const assemblies::AssemblyRecord& record,
                                           WorldReplicationDeltaApplyReport& report) {
    auto status = validate_assembly_references(state, record);
    if (!status) {
        return status;
    }

    if (auto* existing = state.assemblies().find(record.assembly_id); existing != nullptr) {
        status = mark_replicated_assembly_derived_dirty(state, *existing,
                                                        "replicated assembly replaced");
        if (!status) {
            return status;
        }
        *existing = record;
        status =
            mark_replicated_assembly_derived_dirty(state, *existing, "replicated assembly updated");
        if (!status) {
            return status;
        }
        count_updated(report, report.assemblies_updated);
        return core::Status::ok();
    }

    status = state.assemblies().insert(record);
    if (!status) {
        return status;
    }
    status = mark_replicated_assembly_derived_dirty(state, record, "replicated assembly inserted");
    if (!status) {
        return status;
    }
    count_inserted(report, report.assemblies_inserted);
    return core::Status::ok();
}

[[nodiscard]] core::Status upsert_workpiece(WorldState& state,
                                            const save::WorkpieceSaveRecord& record,
                                            WorldReplicationDeltaApplyReport& report) {
    auto grid = workpieces::WorkpieceGridTextCodec::decode(record.encoded_cells);
    if (!grid)
        return core::Status::failure(grid.error().code, grid.error().message);
    std::optional<workpieces::WorkpieceServerState> server_state;
    if (!record.encoded_server_state.empty()) {
        auto decoded = workpieces::WorkpieceServerStateTextCodec::decode(
            record.encoded_server_state, record.shape);
        if (!decoded)
            return core::Status::failure(decoded.error().code, decoded.error().message);
        server_state = std::move(decoded).value();
    }
    WorkpieceRecord materialized{record.workpiece_id,     record.prototype_id,
                                 std::move(grid).value(), record.material_prototype_id,
                                 std::move(server_state), record.owner_session,
                                 record.revision,         record.committed};
    auto status = materialized.validate();
    if (!status)
        return status;
    if (auto* existing = state.workpieces().find(record.workpiece_id); existing != nullptr) {
        *existing = std::move(materialized);
        count_updated(report, report.workpieces_updated);
        return core::Status::ok();
    }
    status = state.workpieces().insert(std::move(materialized));
    if (!status)
        return status;
    count_inserted(report, report.workpieces_inserted);
    return core::Status::ok();
}

[[nodiscard]] core::Status upsert_process(WorldState& state,
                                          const processes::ProcessInstance& record,
                                          WorldReplicationDeltaApplyReport& report) {
    auto status = require_saved_owner_exists(state, record.owner_id, "process");
    if (!status) {
        return status;
    }
    if (auto* existing = state.processes().find(record.process_id); existing != nullptr) {
        *existing = record;
        count_updated(report, report.processes_updated);
        return core::Status::ok();
    }

    status = state.processes().insert(record);
    if (!status) {
        return status;
    }
    count_inserted(report, report.processes_inserted);
    return core::Status::ok();
}

[[nodiscard]] bool has_global_events(const net::ReplicationBatch& batch) noexcept {
    return std::ranges::any_of(batch.events,
                               [](const auto& event) { return !event.subject.is_valid(); });
}

[[nodiscard]] bool has_subject_events(const net::ReplicationBatch& batch) noexcept {
    return std::ranges::any_of(batch.events,
                               [](const auto& event) { return event.subject.is_valid(); });
}

[[nodiscard]] bool has_voxel_changed_event(const net::ReplicationBatch& batch) noexcept {
    return std::ranges::any_of(
        batch.events, [](const auto& event) { return event.type == voxel_changed_event_type; });
}

} // namespace

WorldReplicationDeltaPlan plan_replication_delta(const WorldState& state,
                                                 const net::ReplicationBatch& batch) {
    WorldReplicationDeltaPlan plan;
    plan.command_sequence = batch.command_sequence;
    plan.replication_sequence = net::replication_stream_sequence(batch);
    plan.source_client_id = batch.source_client_id;
    plan.command_type = batch.command_type;
    plan.event_count = static_cast<std::uint32_t>(batch.events.size());

    std::map<std::uint64_t, WorldReplicationDeltaSubjectPlan> subjects_by_id;
    for (const auto& event : batch.events) {
        if (!event.subject.is_valid()) {
            ++plan.global_event_count;
            plan.has_global_events = true;
            plan.global_events.push_back(event);
            continue;
        }

        ++plan.subject_event_count;
        auto [it, inserted] =
            subjects_by_id.emplace(event.subject.value(), WorldReplicationDeltaSubjectPlan{});
        auto& subject = it->second;
        if (inserted) {
            subject.subject_id = event.subject;
            subject.first_event_type = event.type;
        }
        ++subject.event_count;
        subject.private_event_count +=
            net::replication_event_requires_private_access(event) ? 1U : 0U;
    }

    plan.subjects.reserve(subjects_by_id.size());
    for (auto& [_, subject] : subjects_by_id) {
        classify_subject(state, subject);
        plan.materialized_record_count += subject.materialized_record_count;
        if (subject.missing_subject) {
            ++plan.missing_subject_count;
        }
        plan.subjects.push_back(std::move(subject));
    }

    plan.unique_subject_count = static_cast<std::uint32_t>(plan.subjects.size());
    plan.requires_snapshot_resync = plan.missing_subject_count > 0;
    return plan;
}

WorldReplicationDeltaSnapshot materialize_replication_delta(const WorldState& state,
                                                            const net::ReplicationBatch& batch) {
    WorldReplicationDeltaSnapshot snapshot;
    snapshot.plan = plan_replication_delta(state, batch);

    for (const auto& subject : snapshot.plan.subjects) {
        if (subject.has_build_piece) {
            if (const auto* record = state.build_objects().find(subject.subject_id)) {
                snapshot.build_pieces.push_back(*record);
            }
        }
        if (subject.has_entity) {
            if (const auto* record = state.entities().find_by_save_id(subject.subject_id)) {
                snapshot.entities.push_back(entity_save_record(*record));
            } else if (const auto* resource = state.physical_resources().find(subject.subject_id)) {
                snapshot.entities.push_back(physical_resource_save_record(*resource));
            }
        }
        if (subject.has_cargo) {
            if (const auto* record = state.cargo().find(subject.subject_id)) {
                snapshot.cargo_records.push_back(*record);
            }
        }
        if (subject.has_inventory) {
            if (const auto* record = state.inventories().find(subject.subject_id)) {
                snapshot.inventories.push_back(save::InventorySaveRecord{
                    record->owner_id,
                    record->stacks,
                });
            }
        }
        if (subject.has_workpiece) {
            if (const auto* record = state.workpieces().find(
                    core::WorkpieceId::from_value(subject.subject_id.value()))) {
                snapshot.workpieces.push_back(public_workpiece_save_record(*record));
            }
        }
        if (subject.has_assembly) {
            if (const auto* record = state.assemblies().find(subject.subject_id)) {
                snapshot.assemblies.push_back(*record);
            }
        }
        for (const auto* process : sorted_processes_by_owner(state, subject.subject_id)) {
            snapshot.processes.push_back(*process);
        }
    }

    return snapshot;
}

core::Result<WorldReplicationDeltaSnapshot>
filter_replication_delta_snapshot(const WorldReplicationDeltaSnapshot& snapshot,
                                  const net::ReplicationRelevancePolicy& policy,
                                  core::NetId recipient) {
    auto status = validate_delta_snapshot_payload(snapshot);
    if (!status) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(status.error().code,
                                                                    status.error().message);
    }

    WorldReplicationDeltaSnapshot filtered;
    filtered.plan.command_sequence = snapshot.plan.command_sequence;
    filtered.plan.command_type = snapshot.plan.command_type;
    filtered.plan.replication_sequence = snapshot.plan.replication_sequence != 0
                                             ? snapshot.plan.replication_sequence
                                             : snapshot.plan.command_sequence;
    filtered.plan.source_client_id = snapshot.plan.source_client_id;

    if (net::ReplicationRelevance::subject_is_visible(policy, recipient, core::SaveId{})) {
        filtered.plan.global_events = snapshot.plan.global_events;
    }
    filtered.plan.global_event_count =
        static_cast<std::uint32_t>(filtered.plan.global_events.size());
    filtered.plan.has_global_events = !filtered.plan.global_events.empty();

    std::set<std::uint64_t> visible_subjects;
    for (const auto& subject : snapshot.plan.subjects) {
        if (!net::ReplicationRelevance::subject_is_visible(policy, recipient, subject.subject_id)) {
            continue;
        }
        const auto private_visible = net::ReplicationRelevance::private_subject_is_visible(
            policy, recipient, subject.subject_id);
        if (!private_visible && subject.private_event_count == subject.event_count) {
            continue;
        }
        auto visible_subject = subject;
        if (!private_visible) {
            visible_subject.event_count -= visible_subject.private_event_count;
            visible_subject.private_event_count = 0;
            if (visible_subject.first_event_type.starts_with("inventory.")) {
                visible_subject.first_event_type.clear();
            }
            if (visible_subject.has_inventory) {
                visible_subject.has_inventory = false;
                --visible_subject.materialized_record_count;
            }
            visible_subject.missing_subject = visible_subject.materialized_record_count == 0;
        }
        filtered.plan.subjects.push_back(visible_subject);
        visible_subjects.insert(subject.subject_id.value());
        filtered.plan.subject_event_count += visible_subject.event_count;
        filtered.plan.materialized_record_count += visible_subject.materialized_record_count;
        filtered.plan.missing_subject_count += visible_subject.missing_subject ? 1U : 0U;
    }
    filtered.plan.unique_subject_count = static_cast<std::uint32_t>(filtered.plan.subjects.size());
    filtered.plan.event_count =
        filtered.plan.global_event_count + filtered.plan.subject_event_count;
    filtered.plan.requires_snapshot_resync = filtered.plan.missing_subject_count > 0;

    const auto visible = [&visible_subjects](core::SaveId id) {
        return visible_subjects.contains(id.value());
    };
    for (const auto& record : snapshot.build_pieces) {
        if (visible(record.object_id)) {
            filtered.build_pieces.push_back(record);
        }
    }
    for (const auto& record : snapshot.entities) {
        if (visible(record.save_id)) {
            filtered.entities.push_back(record);
        }
    }
    for (const auto& record : snapshot.cargo_records) {
        if (visible(record.cargo_id)) {
            filtered.cargo_records.push_back(record);
        }
    }
    for (const auto& record : snapshot.inventories) {
        if (visible(record.owner_id) && net::ReplicationRelevance::private_subject_is_visible(
                                            policy, recipient, record.owner_id)) {
            filtered.inventories.push_back(record);
        }
    }
    for (const auto& record : snapshot.workpieces) {
        if (visible(core::SaveId::from_value(record.workpiece_id.value()))) {
            filtered.workpieces.push_back(record);
        }
    }
    for (const auto& record : snapshot.assemblies) {
        if (visible(record.assembly_id)) {
            filtered.assemblies.push_back(record);
        }
    }
    for (const auto& record : snapshot.processes) {
        if (visible(record.owner_id)) {
            filtered.processes.push_back(record);
        }
    }

    if (section_record_count(filtered) != filtered.plan.materialized_record_count) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(
            "replication_delta.recipient_filter_requires_resync",
            "recipient-visible delta records cannot be represented as a complete typed snapshot");
    }
    status = validate_delta_snapshot_payload(filtered);
    if (!status) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(status.error().code,
                                                                    status.error().message);
    }
    return core::Result<WorldReplicationDeltaSnapshot>::success(std::move(filtered));
}

WorldReplicationDeltaTickReport
materialize_replication_deltas_for_tick(const WorldState& state,
                                        const net::HostSessionTickResult& tick) {
    WorldReplicationDeltaTickReport report;
    report.command_report_count = static_cast<std::uint32_t>(tick.command_reports.size());
    report.commands.reserve(tick.command_reports.size());

    for (const auto& command : tick.command_reports) {
        WorldReplicationDeltaTickCommand entry;
        entry.client_id = command.client_id;
        entry.command_sequence = command.sequence;
        entry.replication_sequence =
            command.replication_sequence != 0 ? command.replication_sequence : command.sequence;
        entry.command_type = command.command_type;
        entry.error_code = command.error_code;
        entry.error_message = command.error_message;
        entry.operation_trace = command.operation_trace;

        if (!command.success) {
            entry.skipped = true;
            entry.skip_reason = "command_failed";
        } else if (!command.committed_world_mutation) {
            entry.skipped = true;
            entry.skip_reason = "not_mutating";
        } else if (command.events.empty()) {
            entry.skipped = true;
            entry.skip_reason = "no_events";
        } else {
            const net::ReplicationBatch batch{
                command.sequence,     command.command_type,       command.events,
                command.reserved_ids, entry.replication_sequence, command.client_id,
            };
            entry.snapshot = materialize_replication_delta(state, batch);
            ++report.materialized_command_count;
            report.total_event_count += entry.snapshot.plan.event_count;
            report.total_materialized_record_count += entry.snapshot.plan.materialized_record_count;
            report.requires_snapshot_resync =
                report.requires_snapshot_resync || entry.snapshot.plan.requires_snapshot_resync;
        }

        if (entry.skipped) {
            ++report.skipped_command_count;
        }
        report.commands.push_back(std::move(entry));
    }

    return report;
}

core::Result<WorldReplicationDeltaDeliveryReport> send_replication_delta_snapshots_for_tick(
    net::HostSession& host, const WorldReplicationDeltaTickReport& delta_report,
    const net::HostSessionTickResult& tick, std::int64_t server_time_ms) {
    std::map<std::uint64_t, std::size_t> relevance_by_sequence;
    for (std::size_t index = 0; index < tick.replication_relevance_reports.size(); ++index) {
        const auto sequence =
            net::replication_stream_sequence(tick.replication_relevance_reports[index]);
        const auto [_, inserted] = relevance_by_sequence.emplace(sequence, index);
        if (!inserted) {
            return core::Result<WorldReplicationDeltaDeliveryReport>::failure(
                "replication_delta_delivery.duplicate_relevance_sequence",
                "replication delta delivery tick contains duplicate relevance reports");
        }
    }

    WorldReplicationDeltaDeliveryReport report;
    report.command_delta_count = static_cast<std::uint32_t>(delta_report.commands.size());
    report.relevance_report_count =
        static_cast<std::uint32_t>(tick.replication_relevance_reports.size());
    report.commands.reserve(delta_report.commands.size());

    std::set<std::uint64_t> used_relevance_sequences;
    for (const auto& command : delta_report.commands) {
        WorldReplicationDeltaDeliveryCommandReport command_report;
        command_report.source_client_id = command.client_id;
        command_report.command_sequence = command.command_sequence;
        command_report.replication_sequence = command.replication_sequence != 0
                                                  ? command.replication_sequence
                                                  : command.command_sequence;
        command_report.command_type = command.command_type;
        command_report.error_code = command.error_code;
        command_report.error_message = command.error_message;
        command_report.operation_trace = command.operation_trace;

        const auto relevance = relevance_by_sequence.find(command_report.replication_sequence);
        const net::ReplicationRelevanceReport* relevance_report = nullptr;
        if (relevance != relevance_by_sequence.end()) {
            relevance_report = &tick.replication_relevance_reports[relevance->second];
            if (relevance_report->command_type != command.command_type) {
                return core::Result<WorldReplicationDeltaDeliveryReport>::failure(
                    "replication_delta_delivery.command_type_mismatch",
                    "replication delta delivery relevance report command type does not match the "
                    "materialized delta");
            }
            const auto source_identity_mismatch =
                relevance_report->source_client_id.is_valid() && command.client_id.is_valid() &&
                relevance_report->source_client_id != command.client_id;
            if (relevance_report->command_sequence != command.command_sequence ||
                source_identity_mismatch) {
                return core::Result<WorldReplicationDeltaDeliveryReport>::failure(
                    "replication_delta_delivery.command_identity_mismatch",
                    "replication delta delivery relevance report command identity does not match "
                    "the materialized delta");
            }
            command_report.candidate_client_count = relevance_report->candidate_client_count;
            command_report.relevant_client_count = relevance_report->relevant_client_count;
        }

        if (command.skipped) {
            command_report.skipped = true;
            command_report.skip_reason = command.skip_reason;
        } else if (command.snapshot.plan.requires_snapshot_resync) {
            if (relevance_report != nullptr) {
                used_relevance_sequences.insert(command_report.replication_sequence);
            }
            command_report.skipped = true;
            command_report.skip_reason = "requires_snapshot_resync";
            ++report.resync_skipped_count;
        } else {
            if (relevance_report == nullptr) {
                command_report.skipped = true;
                command_report.skip_reason = "missing_relevance_report";
            } else {
                used_relevance_sequences.insert(command_report.replication_sequence);
                for (const auto& decision : relevance_report->decisions) {
                    if (!decision.relevant) {
                        continue;
                    }

                    auto filtered = filter_replication_delta_snapshot(
                        command.snapshot, host.replication_relevance_policy(), decision.client_id);
                    if (!filtered || filtered.value().plan.requires_snapshot_resync) {
                        command_report.skipped_recipients.push_back(decision.client_id);
                        command_report.recipient_diagnostics.push_back(
                            filtered ? "recipient_filter_requires_snapshot_resync"
                                     : filtered.error().code + ": " + filtered.error().message);
                        ++report.resync_skipped_count;
                        continue;
                    }

                    auto message =
                        make_replication_delta_transport_message(filtered.value(), server_time_ms);
                    if (!message) {
                        return core::Result<WorldReplicationDeltaDeliveryReport>::failure(
                            message.error().code, message.error().message);
                    }
                    auto status = host.send_replication_message(decision.client_id,
                                                                std::move(message).value());
                    if (!status) {
                        return core::Result<WorldReplicationDeltaDeliveryReport>::failure(
                            status.error().code, status.error().message);
                    }

                    command_report.recipients.push_back(decision.client_id);
                    ++command_report.sent_message_count;
                    ++report.sent_message_count;
                }
                if (command_report.sent_message_count == 0 &&
                    !command_report.skipped_recipients.empty()) {
                    command_report.skipped = true;
                    command_report.skip_reason = "recipient_filter_requires_resync";
                }
            }
        }

        if (command_report.skipped) {
            ++report.skipped_command_count;
        } else {
            ++report.materialized_command_count;
        }
        report.commands.push_back(std::move(command_report));
    }

    report.unmatched_relevance_count = static_cast<std::uint32_t>(
        tick.replication_relevance_reports.size() - used_relevance_sequences.size());
    return core::Result<WorldReplicationDeltaDeliveryReport>::success(std::move(report));
}

core::Result<WorldReplicationDeltaApplyReport>
apply_replication_delta(WorldState& state, const WorldReplicationDeltaSnapshot& snapshot) {
    auto status = validate_delta_for_apply(snapshot);
    if (!status) {
        return core::Result<WorldReplicationDeltaApplyReport>::failure(status.error().code,
                                                                       status.error().message);
    }

    WorldReplicationDeltaApplyReport report;
    report.command_sequence = snapshot.plan.command_sequence;
    report.replication_sequence = snapshot.plan.replication_sequence != 0
                                      ? snapshot.plan.replication_sequence
                                      : snapshot.plan.command_sequence;
    report.source_client_id = snapshot.plan.source_client_id;
    report.command_type = snapshot.plan.command_type;
    report.event_count = snapshot.plan.event_count;
    report.global_event_count = snapshot.plan.global_event_count;
    report.planned_record_count = snapshot.plan.materialized_record_count;
    report.requires_snapshot_resync = snapshot.plan.requires_snapshot_resync;
    report.dirty_region_count_before = static_cast<std::uint32_t>(state.dirty_regions().size());

    for (const auto& event : snapshot.plan.global_events) {
        if (event.type != voxel_changed_event_type) {
            continue;
        }
        auto change = VoxelChangeTextCodec::decode(event.message);
        if (!change) {
            return core::Result<WorldReplicationDeltaApplyReport>::failure(change.error().code,
                                                                           change.error().message);
        }
        const auto address = block_to_chunk_local(change.value().position);
        (void)state.chunks().get_or_create(address.chunk);
        status = state.chunks().set(address.chunk, address.local, change.value().current,
                                    state.dirty_regions());
        if (!status) {
            return core::Result<WorldReplicationDeltaApplyReport>::failure(status.error().code,
                                                                           status.error().message);
        }
        ++report.voxel_edits_applied;
        ++report.applied_record_count;
        ++report.updated_record_count;
    }

    for (const auto& record : snapshot.build_pieces) {
        status = upsert_build_piece(state, record, report);
        if (!status) {
            return core::Result<WorldReplicationDeltaApplyReport>::failure(status.error().code,
                                                                           status.error().message);
        }
    }
    for (const auto& record : snapshot.entities) {
        status = upsert_entity(state, record, report);
        if (!status) {
            return core::Result<WorldReplicationDeltaApplyReport>::failure(status.error().code,
                                                                           status.error().message);
        }
    }
    for (const auto& record : snapshot.cargo_records) {
        status = upsert_cargo(state, record, report);
        if (!status) {
            return core::Result<WorldReplicationDeltaApplyReport>::failure(status.error().code,
                                                                           status.error().message);
        }
    }
    for (const auto& record : snapshot.assemblies) {
        status = upsert_assembly(state, record, report);
        if (!status) {
            return core::Result<WorldReplicationDeltaApplyReport>::failure(status.error().code,
                                                                           status.error().message);
        }
    }
    for (const auto& record : snapshot.inventories) {
        status = upsert_inventory(state, record, report);
        if (!status) {
            return core::Result<WorldReplicationDeltaApplyReport>::failure(status.error().code,
                                                                           status.error().message);
        }
    }
    for (const auto& record : snapshot.workpieces) {
        status = upsert_workpiece(state, record, report);
        if (!status) {
            return core::Result<WorldReplicationDeltaApplyReport>::failure(status.error().code,
                                                                           status.error().message);
        }
    }
    for (const auto& record : snapshot.processes) {
        status = upsert_process(state, record, report);
        if (!status) {
            return core::Result<WorldReplicationDeltaApplyReport>::failure(status.error().code,
                                                                           status.error().message);
        }
    }

    report.dirty_region_count_after = static_cast<std::uint32_t>(state.dirty_regions().size());
    report.applied = true;
    return core::Result<WorldReplicationDeltaApplyReport>::success(std::move(report));
}

core::Result<WorldClientReplicationApplyReport> apply_client_replication_deltas(
    WorldState& state, net::ClientSession& client_session,
    std::span<const WorldReplicationDeltaSnapshot> decoded_delta_snapshots) {
    std::map<std::uint64_t, std::size_t> deltas_by_sequence;
    for (std::size_t index = 0; index < decoded_delta_snapshots.size(); ++index) {
        const auto& plan = decoded_delta_snapshots[index].plan;
        const auto sequence =
            plan.replication_sequence != 0 ? plan.replication_sequence : plan.command_sequence;
        const auto [_, inserted] = deltas_by_sequence.emplace(sequence, index);
        if (!inserted) {
            return core::Result<WorldClientReplicationApplyReport>::failure(
                "world_client_replication.duplicate_delta_sequence",
                "decoded client replication deltas contain a duplicate replication sequence");
        }
    }

    auto batches = client_session.drain_replication_batches();

    WorldClientReplicationApplyReport report;
    report.drained_batch_count = static_cast<std::uint32_t>(batches.size());
    report.delta_snapshot_count = static_cast<std::uint32_t>(decoded_delta_snapshots.size());
    report.batches.reserve(batches.size());

    std::set<std::size_t> used_delta_indices;
    for (const auto& batch : batches) {
        WorldClientReplicationBatchApplyReport batch_report;
        batch_report.command_sequence = batch.command_sequence;
        batch_report.replication_sequence = net::replication_stream_sequence(batch);
        batch_report.source_client_id = batch.source_client_id;
        batch_report.command_type = batch.command_type;
        batch_report.event_count = static_cast<std::uint32_t>(batch.events.size());
        batch_report.has_global_events = has_global_events(batch);
        batch_report.has_subject_events = has_subject_events(batch);
        report.total_event_count += batch_report.event_count;

        const auto delta = deltas_by_sequence.find(batch_report.replication_sequence);
        if (delta == deltas_by_sequence.end()) {
            if (!batch_report.has_subject_events && has_voxel_changed_event(batch)) {
                WorldReplicationDeltaSnapshot event_snapshot;
                event_snapshot.plan = plan_replication_delta(state, batch);
                auto applied = apply_replication_delta(state, event_snapshot);
                if (!applied) {
                    return core::Result<WorldClientReplicationApplyReport>::failure(
                        applied.error().code, applied.error().message);
                }
                ++report.applied_delta_count;
                report.total_applied_record_count += applied.value().applied_record_count;
                batch_report.applied_delta = true;
                batch_report.state = "applied_event_delta";
                batch_report.delta_apply_report = std::move(applied).value();
            } else if (batch_report.has_subject_events) {
                batch_report.state = "pending_delta";
                batch_report.skip_reason = "missing_delta_snapshot";
                ++report.pending_delta_count;
            } else {
                batch_report.state =
                    batch_report.event_count == 0 ? "empty_events" : "observed_event_only";
                batch_report.skip_reason = "event_only";
                ++report.observed_event_only_count;
            }
            report.observed_events.insert(report.observed_events.end(), batch.events.begin(),
                                          batch.events.end());
            report.batches.push_back(std::move(batch_report));
            continue;
        }

        const auto& snapshot = decoded_delta_snapshots[delta->second];
        batch_report.has_delta_snapshot = true;
        if (snapshot.plan.command_type != batch.command_type) {
            return core::Result<WorldClientReplicationApplyReport>::failure(
                "world_client_replication.command_type_mismatch",
                "client replication delta command type does not match queued event batch");
        }
        const auto source_identity_mismatch =
            snapshot.plan.source_client_id.is_valid() && batch.source_client_id.is_valid() &&
            snapshot.plan.source_client_id != batch.source_client_id;
        if (snapshot.plan.command_sequence != batch.command_sequence || source_identity_mismatch) {
            return core::Result<WorldClientReplicationApplyReport>::failure(
                "world_client_replication.command_identity_mismatch",
                "client replication delta command identity does not match queued event batch");
        }
        if (snapshot.plan.event_count != batch_report.event_count) {
            return core::Result<WorldClientReplicationApplyReport>::failure(
                "world_client_replication.event_count_mismatch",
                "client replication delta event count does not match queued event batch");
        }

        auto applied = apply_replication_delta(state, snapshot);
        if (!applied) {
            return core::Result<WorldClientReplicationApplyReport>::failure(
                applied.error().code, applied.error().message);
        }

        used_delta_indices.insert(delta->second);
        ++report.matched_delta_count;
        ++report.applied_delta_count;
        report.total_applied_record_count += applied.value().applied_record_count;
        batch_report.applied_delta = true;
        batch_report.state =
            applied.value().applied_record_count == 0 ? "applied_empty_delta" : "applied_delta";
        batch_report.delta_apply_report = std::move(applied).value();
        report.observed_events.insert(report.observed_events.end(), batch.events.begin(),
                                      batch.events.end());
        report.batches.push_back(std::move(batch_report));
    }

    for (std::size_t index = 0; index < decoded_delta_snapshots.size(); ++index) {
        if (used_delta_indices.contains(index)) {
            continue;
        }
        const auto& snapshot = decoded_delta_snapshots[index];
        if (snapshot.plan.command_type != "world.initial_snapshot") {
            continue;
        }
        auto applied = apply_replication_delta(state, snapshot);
        if (!applied) {
            return core::Result<WorldClientReplicationApplyReport>::failure(
                applied.error().code, applied.error().message);
        }
        WorldClientReplicationBatchApplyReport batch_report;
        batch_report.command_sequence = snapshot.plan.command_sequence;
        batch_report.replication_sequence = snapshot.plan.replication_sequence;
        batch_report.command_type = snapshot.plan.command_type;
        batch_report.event_count = snapshot.plan.event_count;
        batch_report.has_delta_snapshot = true;
        batch_report.applied_delta = true;
        batch_report.state = "applied_standalone_snapshot";
        batch_report.delta_apply_report = std::move(applied).value();
        report.total_event_count += batch_report.event_count;
        report.total_applied_record_count += batch_report.delta_apply_report.applied_record_count;
        ++report.applied_delta_count;
        used_delta_indices.insert(index);
        report.batches.push_back(std::move(batch_report));
    }
    report.unmatched_delta_count =
        static_cast<std::uint32_t>(decoded_delta_snapshots.size() - used_delta_indices.size());
    return core::Result<WorldClientReplicationApplyReport>::success(std::move(report));
}

core::Result<std::vector<WorldReplicationDeltaSnapshot>>
drain_client_replication_delta_snapshots(net::ClientSession& client_session) {
    auto messages =
        client_session.drain_replication_messages(replication_delta_snapshot_payload_type);
    auto legacy_messages =
        client_session.drain_replication_messages(replication_delta_snapshot_legacy_payload_type);
    messages.insert(messages.end(), std::make_move_iterator(legacy_messages.begin()),
                    std::make_move_iterator(legacy_messages.end()));
    std::ranges::sort(messages, {}, [](const net::TransportEnvelope& envelope) {
        return envelope.message.sequence;
    });

    std::vector<WorldReplicationDeltaSnapshot> snapshots;
    snapshots.reserve(messages.size());
    for (const auto& message : messages) {
        auto snapshot = replication_delta_snapshot_from_transport(message);
        if (!snapshot) {
            return core::Result<std::vector<WorldReplicationDeltaSnapshot>>::failure(
                snapshot.error().code, snapshot.error().message);
        }
        snapshots.push_back(std::move(snapshot).value());
    }

    return core::Result<std::vector<WorldReplicationDeltaSnapshot>>::success(std::move(snapshots));
}

core::Result<WorldClientReplicationApplyReport>
apply_client_queued_replication_deltas(WorldState& state, net::ClientSession& client_session) {
    auto snapshots = drain_client_replication_delta_snapshots(client_session);
    if (!snapshots) {
        return core::Result<WorldClientReplicationApplyReport>::failure(snapshots.error().code,
                                                                        snapshots.error().message);
    }

    return apply_client_replication_deltas(
        state, client_session, std::span<const WorldReplicationDeltaSnapshot>(snapshots.value()));
}

std::string
WorldReplicationDeltaSnapshotTextCodec::encode(const WorldReplicationDeltaSnapshot& snapshot) {
    std::ostringstream output;
    output << delta_magic << '\n';
    output << "sequence="
           << (snapshot.plan.replication_sequence != 0 ? snapshot.plan.replication_sequence
                                                       : snapshot.plan.command_sequence)
           << '\n';
    output << "command_sequence=" << snapshot.plan.command_sequence << '\n';
    output << "source_client=" << snapshot.plan.source_client_id.value() << '\n';
    output << "command=" << delta_percent_escape(snapshot.plan.command_type) << '\n';
    output << "event_count=" << snapshot.plan.event_count << '\n';
    output << "global_event_count=" << snapshot.plan.global_event_count << '\n';
    output << "subject_event_count=" << snapshot.plan.subject_event_count << '\n';
    output << "unique_subject_count=" << snapshot.plan.unique_subject_count << '\n';
    output << "missing_subject_count=" << snapshot.plan.missing_subject_count << '\n';
    output << "materialized_record_count=" << snapshot.plan.materialized_record_count << '\n';
    output << "has_global_events=" << encode_bool(snapshot.plan.has_global_events) << '\n';
    output << "requires_snapshot_resync=" << encode_bool(snapshot.plan.requires_snapshot_resync)
           << '\n';
    for (const auto& event : snapshot.plan.global_events) {
        output << "global_event=" << encode_global_event(event) << '\n';
    }
    for (const auto& subject : snapshot.plan.subjects) {
        output << "subject=" << encode_subject_plan(subject) << '\n';
    }
    output << snapshot_begin_marker << '\n';
    output << save::SaveTextCodec::encode_snapshot(save_snapshot_from_delta(snapshot));
    output << snapshot_end_marker << '\n';
    output << "end\n";
    return output.str();
}

core::Result<WorldReplicationDeltaSnapshot>
WorldReplicationDeltaSnapshotTextCodec::decode(std::string_view text) {
    WorldReplicationDeltaSnapshot snapshot;
    bool saw_magic = false;
    bool saw_end = false;
    bool saw_sequence = false;
    bool saw_command_sequence = false;
    bool saw_command = false;
    bool saw_event_count = false;
    bool saw_global_event_count = false;
    bool saw_subject_event_count = false;
    bool saw_unique_subject_count = false;
    bool saw_missing_subject_count = false;
    bool saw_materialized_record_count = false;
    bool saw_has_global_events = false;
    bool saw_requires_snapshot_resync = false;
    bool saw_snapshot = false;
    bool in_snapshot = false;
    std::string snapshot_payload;

    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        const auto line_end = text.find('\n', line_start);
        auto line = line_end == std::string_view::npos
                        ? text.substr(line_start)
                        : text.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (in_snapshot) {
            if (line == snapshot_end_marker) {
                auto decoded_snapshot = save::SaveTextCodec::decode_snapshot(snapshot_payload);
                if (!decoded_snapshot) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        decoded_snapshot.error().code, decoded_snapshot.error().message);
                }
                auto applied =
                    apply_save_snapshot_to_delta(snapshot, std::move(decoded_snapshot).value());
                if (!applied) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        applied.error().code, applied.error().message);
                }
                in_snapshot = false;
                saw_snapshot = true;
            } else {
                snapshot_payload.append(line);
                snapshot_payload.push_back('\n');
            }
        } else if (!saw_magic) {
            if (line != delta_magic) {
                return core::Result<WorldReplicationDeltaSnapshot>::failure(
                    "replication_delta.invalid_magic",
                    "replication delta payload does not start with the expected magic");
            }
            saw_magic = true;
        } else if (line == "end") {
            saw_end = true;
            break;
        } else if (line == snapshot_begin_marker) {
            if (saw_snapshot) {
                return core::Result<WorldReplicationDeltaSnapshot>::failure(
                    "replication_delta.duplicate_snapshot",
                    "replication delta payload contains multiple embedded snapshots");
            }
            in_snapshot = true;
            snapshot_payload.clear();
        } else if (!line.empty()) {
            const auto separator = line.find('=');
            if (separator == std::string_view::npos) {
                return core::Result<WorldReplicationDeltaSnapshot>::failure(
                    "replication_delta.invalid_line",
                    "replication delta payload line is missing key/value separator");
            }

            const auto key = line.substr(0, separator);
            const auto value = line.substr(separator + 1);
            if (key == "sequence") {
                auto parsed = parse_u64(value, key);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.replication_sequence = parsed.value();
                saw_sequence = true;
            } else if (key == "command_sequence") {
                auto parsed = parse_u64(value, key);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.command_sequence = parsed.value();
                saw_command_sequence = true;
            } else if (key == "source_client") {
                auto parsed = parse_u64(value, key);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.source_client_id = core::NetId::from_value(parsed.value());
            } else if (key == "command") {
                auto parsed = delta_percent_unescape(value);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.command_type = std::move(parsed).value();
                saw_command = true;
            } else if (key == "event_count") {
                auto parsed = parse_u32(value, key);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.event_count = parsed.value();
                saw_event_count = true;
            } else if (key == "global_event_count") {
                auto parsed = parse_u32(value, key);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.global_event_count = parsed.value();
                saw_global_event_count = true;
            } else if (key == "subject_event_count") {
                auto parsed = parse_u32(value, key);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.subject_event_count = parsed.value();
                saw_subject_event_count = true;
            } else if (key == "unique_subject_count") {
                auto parsed = parse_u32(value, key);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.unique_subject_count = parsed.value();
                saw_unique_subject_count = true;
            } else if (key == "missing_subject_count") {
                auto parsed = parse_u32(value, key);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.missing_subject_count = parsed.value();
                saw_missing_subject_count = true;
            } else if (key == "materialized_record_count") {
                auto parsed = parse_u32(value, key);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.materialized_record_count = parsed.value();
                saw_materialized_record_count = true;
            } else if (key == "has_global_events") {
                auto parsed = parse_bool(value, key);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.has_global_events = parsed.value();
                saw_has_global_events = true;
            } else if (key == "requires_snapshot_resync") {
                auto parsed = parse_bool(value, key);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.requires_snapshot_resync = parsed.value();
                saw_requires_snapshot_resync = true;
            } else if (key == "global_event") {
                auto parsed = parse_global_event(value);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.global_events.push_back(std::move(parsed).value());
            } else if (key == "subject") {
                auto parsed = parse_subject_plan(value);
                if (!parsed) {
                    return core::Result<WorldReplicationDeltaSnapshot>::failure(
                        parsed.error().code, parsed.error().message);
                }
                snapshot.plan.subjects.push_back(std::move(parsed).value());
            } else {
                return core::Result<WorldReplicationDeltaSnapshot>::failure(
                    "replication_delta.unknown_key",
                    "unknown replication delta payload key: " + std::string(key));
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }

    if (in_snapshot) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(
            "replication_delta.unterminated_snapshot",
            "replication delta payload embedded snapshot is not terminated");
    }
    if (!saw_magic || !saw_end || !saw_sequence || !saw_command || !saw_event_count ||
        !saw_global_event_count || !saw_subject_event_count || !saw_unique_subject_count ||
        !saw_missing_subject_count || !saw_materialized_record_count || !saw_has_global_events ||
        !saw_requires_snapshot_resync || !saw_snapshot) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(
            "replication_delta.incomplete",
            "replication delta payload is missing required records");
    }
    if (!saw_command_sequence) {
        snapshot.plan.command_sequence = snapshot.plan.replication_sequence;
    }

    auto status = validate_delta_snapshot_payload(snapshot);
    if (!status) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(status.error().code,
                                                                    status.error().message);
    }

    return core::Result<WorldReplicationDeltaSnapshot>::success(std::move(snapshot));
}

core::Result<std::string>
WorldReplicationDeltaSnapshotBinaryCodec::encode(const WorldReplicationDeltaSnapshot& snapshot) {
    auto validation = validate_delta_snapshot_payload(snapshot);
    if (!validation) {
        return core::Result<std::string>::failure(validation.error().code,
                                                  validation.error().message);
    }
    auto saved = save::SaveBinaryCodec::encode_snapshot(save_snapshot_from_delta(snapshot));
    if (!saved) {
        return core::Result<std::string>::failure(saved.error().code, saved.error().message);
    }

    net::BinaryMessageWriter writer;
    writer.u32(delta_binary_magic);
    writer.var_u64(snapshot.plan.replication_sequence != 0 ? snapshot.plan.replication_sequence
                                                           : snapshot.plan.command_sequence);
    writer.var_u64(snapshot.plan.command_sequence);
    writer.var_u64(snapshot.plan.source_client_id.value());
    writer.text_var(snapshot.plan.command_type);
    writer.var_u64(snapshot.plan.event_count);
    writer.var_u64(snapshot.plan.global_event_count);
    writer.var_u64(snapshot.plan.subject_event_count);
    writer.var_u64(snapshot.plan.unique_subject_count);
    writer.var_u64(snapshot.plan.missing_subject_count);
    writer.var_u64(snapshot.plan.materialized_record_count);
    std::uint8_t plan_flags = snapshot.plan.has_global_events ? 1U : 0U;
    plan_flags |= snapshot.plan.requires_snapshot_resync ? 2U : 0U;
    writer.u8(plan_flags);
    for (const auto& event : snapshot.plan.global_events) {
        writer.text_var(event.type);
        writer.var_u64(event.subject.value());
        writer.text_var(event.message);
    }
    for (const auto& subject : snapshot.plan.subjects) {
        writer.var_u64(subject.subject_id.value());
        writer.var_u64(subject.event_count);
        writer.text_var(subject.first_event_type);
        std::uint8_t subject_flags = subject.has_build_piece ? 1U : 0U;
        subject_flags |= subject.has_entity ? 2U : 0U;
        subject_flags |= subject.has_cargo ? 4U : 0U;
        subject_flags |= subject.has_assembly ? 8U : 0U;
        subject_flags |= subject.has_inventory ? 16U : 0U;
        subject_flags |= subject.missing_subject ? 32U : 0U;
        subject_flags |= subject.has_workpiece ? 64U : 0U;
        writer.u8(subject_flags);
        writer.var_u64(subject.process_count);
        writer.var_u64(subject.materialized_record_count);
        writer.var_u64(subject.private_event_count);
    }
    const auto* save_bytes = reinterpret_cast<const char*>(saved.value().data());
    writer.text_var(std::string_view(save_bytes, saved.value().size()));
    auto encoded = writer.take();
    if (encoded.size() > max_delta_binary_bytes) {
        return core::Result<std::string>::failure(
            "replication_delta.too_large", "binary replication delta exceeds its payload limit");
    }
    return core::Result<std::string>::success(std::move(encoded));
}

core::Result<WorldReplicationDeltaSnapshot>
WorldReplicationDeltaSnapshotBinaryCodec::decode(std::string_view bytes) {
    if (bytes.size() > max_delta_binary_bytes) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(
            "replication_delta.too_large", "binary replication delta exceeds its payload limit");
    }

    net::BinaryMessageReader reader(bytes);
    std::uint32_t decoded_magic = 0;
    std::uint64_t source_client = 0;
    std::uint64_t event_count = 0;
    std::uint64_t global_event_count = 0;
    std::uint64_t subject_event_count = 0;
    std::uint64_t unique_subject_count = 0;
    std::uint64_t missing_subject_count = 0;
    std::uint64_t materialized_record_count = 0;
    std::uint8_t plan_flags = 0;
    WorldReplicationDeltaSnapshot snapshot;
    if (!reader.u32(decoded_magic) || decoded_magic != delta_binary_magic ||
        !reader.var_u64(snapshot.plan.replication_sequence) ||
        !reader.var_u64(snapshot.plan.command_sequence) || !reader.var_u64(source_client) ||
        !reader.text_var(snapshot.plan.command_type, max_delta_command_type_bytes) ||
        !reader.var_u64(event_count) || !reader.var_u64(global_event_count) ||
        !reader.var_u64(subject_event_count) || !reader.var_u64(unique_subject_count) ||
        !reader.var_u64(missing_subject_count) || !reader.var_u64(materialized_record_count) ||
        !reader.u8(plan_flags) || plan_flags > 3U || event_count > max_delta_records ||
        global_event_count > max_delta_records || subject_event_count > max_delta_records ||
        unique_subject_count > max_delta_records || missing_subject_count > max_delta_records ||
        materialized_record_count > max_delta_records) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(
            "replication_delta.invalid_binary",
            "binary replication delta header is malformed, truncated, or outside its bounds");
    }
    snapshot.plan.source_client_id = core::NetId::from_value(source_client);
    snapshot.plan.event_count = static_cast<std::uint32_t>(event_count);
    snapshot.plan.global_event_count = static_cast<std::uint32_t>(global_event_count);
    snapshot.plan.subject_event_count = static_cast<std::uint32_t>(subject_event_count);
    snapshot.plan.unique_subject_count = static_cast<std::uint32_t>(unique_subject_count);
    snapshot.plan.missing_subject_count = static_cast<std::uint32_t>(missing_subject_count);
    snapshot.plan.materialized_record_count = static_cast<std::uint32_t>(materialized_record_count);
    snapshot.plan.has_global_events = (plan_flags & 1U) != 0;
    snapshot.plan.requires_snapshot_resync = (plan_flags & 2U) != 0;

    snapshot.plan.global_events.reserve(static_cast<std::size_t>(global_event_count));
    for (std::uint64_t index = 0; index < global_event_count; ++index) {
        OperationEvent event;
        std::uint64_t subject = 0;
        if (!reader.text_var(event.type, max_delta_event_type_bytes) || !reader.var_u64(subject) ||
            !reader.text_var(event.message, max_delta_event_message_bytes)) {
            return core::Result<WorldReplicationDeltaSnapshot>::failure(
                "replication_delta.invalid_binary",
                "binary replication delta global event is malformed or oversized");
        }
        event.subject = core::SaveId::from_value(subject);
        snapshot.plan.global_events.push_back(std::move(event));
    }

    snapshot.plan.subjects.reserve(static_cast<std::size_t>(unique_subject_count));
    for (std::uint64_t index = 0; index < unique_subject_count; ++index) {
        std::uint64_t subject_id = 0;
        std::uint64_t subject_events = 0;
        std::uint64_t process_count = 0;
        std::uint64_t subject_record_count = 0;
        std::uint64_t private_event_count = 0;
        std::uint8_t subject_flags = 0;
        WorldReplicationDeltaSubjectPlan subject;
        if (!reader.var_u64(subject_id) || !reader.var_u64(subject_events) ||
            !reader.text_var(subject.first_event_type, max_delta_event_type_bytes) ||
            !reader.u8(subject_flags) || subject_flags > 127U || !reader.var_u64(process_count) ||
            !reader.var_u64(subject_record_count) || !reader.var_u64(private_event_count) ||
            subject_events > max_delta_records || process_count > max_delta_records ||
            subject_record_count > max_delta_records || private_event_count > subject_events) {
            return core::Result<WorldReplicationDeltaSnapshot>::failure(
                "replication_delta.invalid_binary",
                "binary replication delta subject plan is malformed or outside its bounds");
        }
        subject.subject_id = core::SaveId::from_value(subject_id);
        subject.event_count = static_cast<std::uint32_t>(subject_events);
        subject.has_build_piece = (subject_flags & 1U) != 0;
        subject.has_entity = (subject_flags & 2U) != 0;
        subject.has_cargo = (subject_flags & 4U) != 0;
        subject.has_assembly = (subject_flags & 8U) != 0;
        subject.has_inventory = (subject_flags & 16U) != 0;
        subject.missing_subject = (subject_flags & 32U) != 0;
        subject.has_workpiece = (subject_flags & 64U) != 0;
        subject.process_count = static_cast<std::uint32_t>(process_count);
        subject.materialized_record_count = static_cast<std::uint32_t>(subject_record_count);
        subject.private_event_count = static_cast<std::uint32_t>(private_event_count);
        snapshot.plan.subjects.push_back(std::move(subject));
    }

    std::string embedded_save;
    if (!reader.text_var(embedded_save, max_delta_binary_bytes) || !reader.finished()) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(
            "replication_delta.invalid_binary",
            "binary replication delta embedded snapshot is truncated or has trailing bytes");
    }
    const auto* save_bytes = reinterpret_cast<const std::uint8_t*>(embedded_save.data());
    auto decoded_save = save::SaveBinaryCodec::decode_snapshot(
        std::span<const std::uint8_t>(save_bytes, embedded_save.size()));
    if (!decoded_save) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(decoded_save.error().code,
                                                                    decoded_save.error().message);
    }
    auto applied = apply_save_snapshot_to_delta(snapshot, std::move(decoded_save).value());
    if (!applied) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(applied.error().code,
                                                                    applied.error().message);
    }
    auto validation = validate_delta_snapshot_payload(snapshot);
    if (!validation) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(validation.error().code,
                                                                    validation.error().message);
    }
    return core::Result<WorldReplicationDeltaSnapshot>::success(std::move(snapshot));
}

core::Result<net::TransportMessage>
make_replication_delta_transport_message(const WorldReplicationDeltaSnapshot& snapshot,
                                         std::int64_t server_time_ms) {
    auto payload = WorldReplicationDeltaSnapshotBinaryCodec::encode(snapshot);
    if (!payload) {
        return core::Result<net::TransportMessage>::failure(payload.error().code,
                                                            payload.error().message);
    }
    return core::Result<net::TransportMessage>::success(net::TransportMessage{
        net::TransportMessageKind::replication,
        net::TransportChannel::reliable,
        snapshot.plan.replication_sequence != 0 ? snapshot.plan.replication_sequence
                                                : snapshot.plan.command_sequence,
        std::string(replication_delta_snapshot_payload_type),
        std::move(payload).value(),
        server_time_ms,
    });
}

core::Result<WorldReplicationDeltaSnapshot>
replication_delta_snapshot_from_transport(const net::TransportEnvelope& envelope) {
    if (envelope.message.kind != net::TransportMessageKind::replication) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(
            "replication_delta.not_replication_message",
            "transport message is not a replication delta snapshot");
    }
    if (envelope.message.channel != net::TransportChannel::reliable) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(
            "replication_delta.unreliable_replication",
            "replication delta snapshots must arrive on the reliable transport channel");
    }
    if (envelope.message.payload_type != replication_delta_snapshot_payload_type &&
        envelope.message.payload_type != replication_delta_snapshot_legacy_payload_type) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(
            "replication_delta.unexpected_payload_type",
            "replication delta transport message has an unexpected payload type");
    }

    auto snapshot = envelope.message.payload_type == replication_delta_snapshot_payload_type
                        ? WorldReplicationDeltaSnapshotBinaryCodec::decode(envelope.message.payload)
                        : WorldReplicationDeltaSnapshotTextCodec::decode(envelope.message.payload);
    if (!snapshot) {
        return snapshot;
    }
    const auto stream_sequence = snapshot.value().plan.replication_sequence != 0
                                     ? snapshot.value().plan.replication_sequence
                                     : snapshot.value().plan.command_sequence;
    if (stream_sequence != envelope.message.sequence) {
        return core::Result<WorldReplicationDeltaSnapshot>::failure(
            "replication_delta.sequence_mismatch",
            "replication delta snapshot sequence does not match the transport envelope sequence");
    }
    return snapshot;
}

} // namespace heartstead::world
