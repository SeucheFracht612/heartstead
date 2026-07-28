#include "game/runtime/game_inspection.hpp"

#include "engine/scripting/script_runtime.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace heartstead::game {

namespace {

void add_field(debug::InspectionData& data, std::string name, std::string value) {
    data.fields.push_back(debug::InspectionField{std::move(name), std::move(value)});
}

void add_issue(debug::InspectionData& data, debug::InspectionSeverity severity, std::string code,
               std::string message) {
    data.issues.push_back(debug::InspectionIssue{severity, std::move(code), std::move(message)});
}

void add_status_issue(debug::InspectionData& data, const core::Status& status) {
    if (!status) {
        add_issue(data, debug::InspectionSeverity::error, status.error().code,
                  status.error().message);
    }
}

[[nodiscard]] std::string argument_key_list(const std::vector<std::string>& keys) {
    std::ostringstream output;
    bool first = true;
    for (const auto& key : keys) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << key;
    }
    return output.str();
}

} // namespace

debug::InspectionData GameInspector::inspect(const ScriptHostCommandRoute& route) {
    debug::InspectionData data;
    data.object_type = "script_host_command_route";
    data.display_name = route.api_id;
    data.runtime_id = route.api_id;
    data.state = std::string(scripting::script_stage_name(route.stage));
    add_field(data, "api_id", route.api_id);
    add_field(data, "command_type", route.command_type);
    add_field(data, "stage", std::string(scripting::script_stage_name(route.stage)));
    add_field(data, "required_argument_count", std::to_string(route.required_argument_keys.size()));
    add_field(data, "optional_argument_count", std::to_string(route.optional_argument_keys.size()));
    add_field(data, "required_arguments", argument_key_list(route.required_argument_keys));
    add_field(data, "optional_arguments", argument_key_list(route.optional_argument_keys));
    add_status_issue(data, validate_script_host_command_route(route));
    return data;
}

debug::InspectionData GameInspector::inspect(const ScriptHostCommandReport& report) {
    debug::InspectionData data;
    data.object_type = "script_host_command_report";
    data.display_name = report.api_id;
    data.runtime_id = std::to_string(report.command_sequence);
    data.state = report.success ? "accepted" : "rejected";
    add_field(data, "script_event_sequence", std::to_string(report.script_event_sequence));
    add_field(data, "command_sequence", std::to_string(report.command_sequence));
    add_field(data, "api_id", report.api_id);
    add_field(data, "command_type", report.command_type);
    add_field(data, "success", report.success ? "true" : "false");
    add_field(data, "committed_world_mutation", report.committed_world_mutation ? "true" : "false");
    add_field(data, "event_count", std::to_string(report.events.size()));
    add_field(data, "reserved_id_count", std::to_string(report.reserved_ids.size()));
    add_field(data, "error_code", report.error_code);
    add_field(data, "error_message", report.error_message);
    add_status_issue(data, validate_script_host_command_report(report));
    return data;
}

debug::InspectionData GameInspector::inspect(const ScriptHostCommandBatchResult& batch) {
    debug::InspectionData data;
    data.object_type = "script_host_command_batch";
    data.display_name = "Script Host Command Batch";
    data.runtime_id =
        batch.reports.empty() ? "" : std::to_string(batch.reports.front().command_sequence);
    data.state = batch.failure_count == 0 ? "accepted" : "partial_or_rejected";
    add_field(data, "command_count", std::to_string(batch.command_count));
    add_field(data, "success_count", std::to_string(batch.success_count));
    add_field(data, "failure_count", std::to_string(batch.failure_count));
    add_field(data, "report_count", std::to_string(batch.reports.size()));
    add_status_issue(data, validate_script_host_command_batch_result(batch));
    return data;
}

debug::InspectionData GameInspector::inspect(const GameplayModuleRegistrationReport& report) {
    debug::InspectionData data;
    data.object_type = "gameplay_module_registry";
    data.display_name = "Gameplay Modules";
    data.runtime_id = report.module_ids.empty() ? "" : report.module_ids.front();
    data.state = "registered";
    add_field(data, "module_count", std::to_string(report.module_ids.size()));
    add_field(data, "component_count", std::to_string(report.component_count));
    add_field(data, "service_count", std::to_string(report.service_count));
    add_field(data, "command_count", std::to_string(report.command_count));
    add_field(data, "system_count", std::to_string(report.system_count));
    add_field(data, "serializer_count", std::to_string(report.serializer_count));
    add_field(data, "persistence_count", std::to_string(report.persistence_count));
    add_field(data, "replication_count", std::to_string(report.replication_count));
    add_field(data, "presentation_adapter_count",
              std::to_string(report.presentation_adapter_count));
    std::ostringstream module_ids;
    for (std::size_t index = 0; index < report.module_ids.size(); ++index) {
        if (index != 0) {
            module_ids << ',';
        }
        module_ids << report.module_ids[index];
    }
    add_field(data, "modules", module_ids.str());
    return data;
}

debug::InspectionData GameInspector::inspect(const ClientRuntimeStats& stats) {
    debug::InspectionData data;
    data.object_type = "client_runtime_frame";
    data.display_name = "Client Runtime";
    data.state = "synchronized";
    add_field(data, "received_message_count", std::to_string(stats.received_message_count));
    add_field(data, "command_result_count", std::to_string(stats.command_result_count));
    add_field(data, "movement_snapshot_count", std::to_string(stats.movement_snapshot_count));
    add_field(data, "player_tombstone_count", std::to_string(stats.player_tombstone_count));
    add_field(data, "chunk_snapshot_slice_count", std::to_string(stats.chunk_snapshot_slice_count));
    add_field(data, "completed_chunk_snapshot_count",
              std::to_string(stats.completed_chunk_snapshot_count));
    add_field(data, "replication_batch_count",
              std::to_string(stats.replication.drained_batch_count));
    add_field(data, "replication_event_count", std::to_string(stats.replication.total_event_count));
    add_field(data, "replication_applied_record_count",
              std::to_string(stats.replication.total_applied_record_count));
    add_field(data, "feature_replication_registered_count",
              std::to_string(stats.feature_replication.registered_event_count));
    add_field(data, "feature_replication_callback_count",
              std::to_string(stats.feature_replication.callback_event_count));
    add_field(data, "feature_replication_unhandled_count",
              std::to_string(stats.feature_replication.unhandled_event_count));
    if (stats.feature_replication.unhandled_event_count != 0) {
        add_issue(data, debug::InspectionSeverity::warning,
                  "client_runtime.unhandled_replication_events",
                  "client observed replication events without a registered feature handler");
    }
    return data;
}

debug::InspectionData GameInspector::inspect(const PresentationSynchronizationStats& stats) {
    debug::InspectionData data;
    data.object_type = "presentation_synchronization";
    data.display_name = "Presentation Synchronization";
    data.state = "synchronized";
    add_field(data, "adapter_count", std::to_string(stats.adapter_count));
    add_field(data, "inserted_objects", std::to_string(stats.inserted_objects));
    add_field(data, "updated_objects", std::to_string(stats.updated_objects));
    add_field(data, "removed_objects", std::to_string(stats.removed_objects));
    add_field(data, "unchanged_objects", std::to_string(stats.unchanged_objects));
    return data;
}

debug::InspectionData GameInspector::inspect(const RuntimeFrameStats& stats) {
    debug::InspectionData data;
    data.object_type = "runtime_frame";
    data.display_name = "Runtime Frame";
    data.runtime_id = std::to_string(stats.authoritative_world_tick);
    data.state = "completed";
    add_field(data, "authoritative_world_tick", std::to_string(stats.authoritative_world_tick));
    add_field(data, "fixed_step_count", std::to_string(stats.fixed_step.step_count));
    add_field(data, "fixed_step_us", std::to_string(stats.fixed_step.step_us));
    add_field(data, "fixed_step_first_tick", std::to_string(stats.fixed_step.first_tick));
    add_field(data, "interpolation_alpha", std::to_string(stats.fixed_step.interpolation_alpha));
    add_field(data, "dropped_time_us", std::to_string(stats.fixed_step.dropped_time_us));
    double simulation_ms = 0.0;
    std::uint32_t command_count = 0;
    std::uint32_t failed_command_count = 0;
    std::uint32_t event_count = 0;
    std::uint32_t replication_message_count = 0;
    std::uint32_t physics_body_count = 0;
    std::size_t collision_body_count = 0;
    std::uint64_t collision_box_count = 0;
    double collision_cooking_ms = 0.0;
    double collision_apply_ms = 0.0;
    for (const auto& tick : stats.server_ticks) {
        simulation_ms += tick.simulation.total_ms;
        command_count += static_cast<std::uint32_t>(tick.commands.command_reports.size());
        failed_command_count += static_cast<std::uint32_t>(std::ranges::count_if(
            tick.commands.command_reports, [](const auto& report) { return !report.success; }));
        event_count += tick.simulation.event_count;
        replication_message_count += tick.replication.sent_message_count;
        physics_body_count = tick.physics.body_count;
        collision_body_count = tick.chunk_collision.resident_body_count;
        collision_box_count = tick.chunk_collision.current_collision_boxes;
        collision_cooking_ms = tick.chunk_collision.last_cooking_ms;
        collision_apply_ms = tick.chunk_collision.last_apply_ms;
    }
    add_field(data, "server_tick_count", std::to_string(stats.server_ticks.size()));
    add_field(data, "simulation_ms", std::to_string(simulation_ms));
    add_field(data, "command_count", std::to_string(command_count));
    add_field(data, "failed_command_count", std::to_string(failed_command_count));
    add_field(data, "event_count", std::to_string(event_count));
    add_field(data, "replication_message_count", std::to_string(replication_message_count));
    add_field(data, "physics_body_count", std::to_string(physics_body_count));
    add_field(data, "chunk_collision_body_count", std::to_string(collision_body_count));
    add_field(data, "chunk_collision_box_count", std::to_string(collision_box_count));
    add_field(data, "chunk_collision_cooking_ms", std::to_string(collision_cooking_ms));
    add_field(data, "chunk_collision_apply_ms", std::to_string(collision_apply_ms));
    add_field(data, "client_received_message_count",
              std::to_string(stats.client.received_message_count));
    add_field(data, "presentation_adapter_count", std::to_string(stats.presentation.adapter_count));
    if (stats.fixed_step.dropped_time_us != 0) {
        add_issue(data, debug::InspectionSeverity::warning, "runtime_frame.dropped_time",
                  "fixed-step frame discarded accumulated simulation time");
    }
    return data;
}

debug::InspectionData GameInspector::inspect(const RuntimeSession& session) {
    debug::InspectionData data;
    data.object_type = "runtime_session";
    data.display_name = "Game Runtime Session";
    data.runtime_id = std::to_string(session.frame_count());
    const auto& fault = session.fault();
    data.state = fault.has_value() ? "faulted" : (session.is_running() ? "running" : "stopped");
    const auto& config = session.config();
    add_field(data, "frame_count", std::to_string(session.frame_count()));
    add_field(data, "headless", config.headless ? "true" : "false");
    add_field(data, "has_server", session.server() != nullptr ? "true" : "false");
    add_field(data, "has_client", session.client() != nullptr ? "true" : "false");
    add_field(data, "in_memory_transport", config.use_in_memory_transport ? "true" : "false");
    add_field(data, "ticks_per_second", std::to_string(config.fixed_step.ticks_per_second));
    add_field(data, "fixed_step_ticks_per_second",
              std::to_string(config.fixed_step.ticks_per_second));
    add_field(data, "world_ticks_per_second", std::to_string(config.world_time.ticks_per_second));
    add_field(data, "persistence_mode", "explicit_snapshot");
    add_field(data, "pending_save_count", "0");
    add_field(data, "faulted", fault.has_value() ? "true" : "false");
    if (fault.has_value()) {
        add_field(data, "fault_code", fault->code);
        add_field(data, "fault_message", fault->message);
        add_issue(data, debug::InspectionSeverity::error, fault->code, fault->message);
    }

    if (const auto* server = session.server(); server != nullptr) {
        const auto world_stats = server->world().stats();
        const auto chunk_stats = server->world().chunks().stats();
        const auto entity_stats = server->entities().stats();
        add_field(data, "authoritative_world_tick", std::to_string(server->world().world_time()));
        add_field(data, "connected_client_count",
                  std::to_string(server->host().connected_client_count()));
        add_field(data, "loaded_chunk_count", std::to_string(world_stats.chunk_count));
        add_field(data, "chunk_edit_count", std::to_string(chunk_stats.edit_count));
        add_field(data, "dirty_mesh_chunk_count", std::to_string(chunk_stats.dirty_mesh_count));
        add_field(data, "dirty_save_chunk_count", std::to_string(chunk_stats.dirty_save_count));
        add_field(data, "dirty_region_count", std::to_string(world_stats.dirty_region_count));
        add_field(data, "persistent_entity_count",
                  std::to_string(world_stats.persistent_entity_count));
        add_field(data, "live_entity_count", std::to_string(entity_stats.live_entities));
        add_field(data, "active_entity_count", std::to_string(entity_stats.active_entities));
        add_field(data, "component_count", std::to_string(entity_stats.component_count));
        add_field(data, "entity_tombstone_count", std::to_string(entity_stats.tombstone_count));
        add_field(data, "player_controller_count", std::to_string(server->players().size()));
        const auto& collision_stats = server->chunk_collision().stats();
        add_field(data, "chunk_collision_body_count",
                  std::to_string(collision_stats.resident_body_count));
        add_field(data, "chunk_collision_pending_count",
                  std::to_string(collision_stats.pending_chunk_count));
        add_field(data, "chunk_collision_box_count",
                  std::to_string(collision_stats.current_collision_boxes));
        add_field(data, "simulation_system_count",
                  std::to_string(server->scheduler().registered_system_count()));
        add_field(data, "gameplay_module_count",
                  std::to_string(server->gameplay_modules().report().module_ids.size()));
        double last_system_ms = 0.0;
        for (const auto& timing : server->scheduler().timings()) {
            last_system_ms += timing.last_ms;
        }
        add_field(data, "last_system_total_ms", std::to_string(last_system_ms));
    }
    if (const auto* client = session.client(); client != nullptr) {
        const auto client_stats = client->session().stats();
        add_field(data, "client_connected", client->is_connected() ? "true" : "false");
        add_field(data, "client_pending_command_count",
                  std::to_string(client_stats.pending_command_count));
        add_field(data, "client_queued_replication_message_count",
                  std::to_string(client_stats.queued_replication_message_count));
        add_field(data, "client_loaded_chunk_count",
                  std::to_string(client->world().chunks().chunk_count()));
        add_field(data, "client_player_snapshot_count",
                  std::to_string(client->movement_snapshots().size()));
    }
    if (const auto* presentation = session.presentation(); presentation != nullptr) {
        const auto presentation_stats = presentation->stats();
        add_field(data, "presentation_object_count",
                  std::to_string(presentation_stats.retained_object_count));
        add_field(data, "presentation_revision",
                  std::to_string(presentation_stats.presentation_revision));
    }
    return data;
}

std::vector<debug::InspectionData>
GameInspector::inspect_system_timings(const ServerRuntime& server) {
    std::vector<debug::InspectionData> records;
    records.reserve(server.scheduler().timings().size());
    for (const auto& timing : server.scheduler().timings()) {
        debug::InspectionData data;
        data.object_type = "simulation_system_timing";
        data.display_name = timing.name;
        data.runtime_id = timing.name;
        data.state = std::string(simulation::simulation_phase_name(timing.phase));
        add_field(data, "phase", std::string(simulation::simulation_phase_name(timing.phase)));
        add_field(data, "last_ms", std::to_string(timing.last_ms));
        add_field(data, "total_ms", std::to_string(timing.total_ms));
        add_field(data, "invocation_count", std::to_string(timing.invocation_count));
        records.push_back(std::move(data));
    }
    return records;
}

} // namespace heartstead::game
